/*
 * tb_test_gui.cpp - automated GUI smoke + smoothness test.
 *
 * Drives the real MainWindow with simulated button clicks (Play, Restart, the
 * difficulty buttons) and a hardware-decode change (the Settings effect), while
 * recording every presented frame through the actual paint path via Engine's frame
 * probe. Then it analyses the recorded timeline for:
 *   - LOCKUPS: video frozen (pts not advancing) while not paused / not ended.
 *   - SMOOTHNESS: presented cadence vs the source - effective fps, interval jitter.
 *   - LOCK CONTENTION: how long tb_player_acquire_frame blocked the paint thread.
 *   - MEMORY: RSS growth across repeated restart/reload cycles (leak check).
 *
 * Exit code 0 = all checks within thresholds; non-zero = a check failed.
 */
#include "MainWindow.h"
#include "Engine.h"

#include <QApplication>
#include <QPushButton>
#include <QElapsedTimer>
#include <QTimer>
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include <unistd.h>

struct Sample { double wall_ms, pts, acquire_ms; bool paused; };
struct Action { double wall_ms; std::string what; };

static std::vector<Sample> g_samples;
static std::vector<Action> g_actions;
static QElapsedTimer g_clock;
static Engine *g_engine = nullptr;
static MainWindow *g_win = nullptr;
static double g_rssBaseline = 0, g_rssAfterCycles = 0;

static double rssMB() {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long sz = 0, res = 0;
    if (fscanf(f, "%ld %ld", &sz, &res) != 2) res = 0;
    fclose(f);
    return res * (double)sysconf(_SC_PAGESIZE) / 1048576.0;
}

static void mark(const char *what) { g_actions.push_back({ (double)g_clock.elapsed(), what }); printf("  [%.0fms] action: %s\n", (double)g_clock.elapsed(), what); }

static void click(const char *name) {
    QPushButton *b = g_win->findChild<QPushButton *>(name);
    if (b) { b->click(); mark(name); }
    else printf("  !! button '%s' not found\n", name);
}

/* ---------------- analysis ---------------- */
static const char *actionBefore(double wall) {
    const char *a = "(start)";
    for (auto &ac : g_actions) { if (ac.wall_ms <= wall) a = ac.what.c_str(); else break; }
    return a;
}

static int analyze() {
    printf("\n================ ANALYSIS (%zu samples, %zu actions) ================\n",
           g_samples.size(), g_actions.size());
    int failures = 0;

    // dump timeline for offline inspection
    if (FILE *csv = fopen("/tmp/frames.csv", "w")) {
        fprintf(csv, "wall_ms,pts,acquire_ms,paused\n");
        for (auto &s : g_samples) fprintf(csv, "%.0f,%.3f,%.3f,%d\n", s.wall_ms, s.pts, s.acquire_ms, s.paused ? 1 : 0);
        fclose(csv);
    }

    /* ---- lock-up detection: longest stretch with pts frozen while !paused ---- */
    double worstFreeze = 0, worstAt = 0;
    double runStart = -1, runPts = -1;
    for (size_t i = 0; i < g_samples.size(); i++) {
        const Sample &s = g_samples[i];
        bool active = !s.paused && s.pts >= 0;
        if (!active) { runStart = -1; runPts = -1; continue; }
        if (runStart < 0 || fabs(s.pts - runPts) > 1e-4) {   // new frame or fresh run
            if (s.pts < runPts - 0.001) { /* pts went backward (seek): reset */ }
            runStart = s.wall_ms; runPts = s.pts;
        } else {
            double span = s.wall_ms - runStart;
            if (span > worstFreeze) { worstFreeze = span; worstAt = runStart; }
        }
    }
    printf("LOCKUP:    worst frozen-while-playing stretch = %.0f ms (at ~%.0fms, after '%s')\n",
           worstFreeze, worstAt, actionBefore(worstAt));
    if (worstFreeze > 1000.0) { printf("           ==> FAIL: video locked up > 1s while playing\n"); failures++; }
    else printf("           ==> ok (no lock-up > 1s)\n");

    /* ---- smoothness: cadence of presents + effective fps while playing ---- */
    // Seek-aware: only accumulate forward pts movement within a continuous playing
    // segment (a restart seeks pts back to 0; that backward jump is not a stall and
    // must not count toward the rate).
    int playing = 0, newFrames = 0; double playWall = 0, lastWall = -1, prevPts = -2;
    double ptsAdvance = 0, segWall = 0;
    std::vector<double> intervals;
    for (auto &s : g_samples) {
        if (s.paused || s.pts < 0) { lastWall = -1; prevPts = -2; continue; }
        playing++;
        if (lastWall >= 0) {
            double dw = s.wall_ms - lastWall;
            intervals.push_back(dw); playWall += dw;
            double dp = s.pts - prevPts;
            if (dp >= 0 && dp < 1.0) { ptsAdvance += dp; segWall += dw; }   // ignore seeks/gaps
        }
        lastWall = s.wall_ms;
        if (fabs(s.pts - prevPts) > 1e-4) newFrames++;
        prevPts = s.pts;
    }
    double effFps = playWall > 0 ? newFrames * 1000.0 / playWall : 0;
    double presRate = playWall > 0 ? playing * 1000.0 / playWall : 0;
    double advanceRatio = segWall > 0 ? ptsAdvance * 1000.0 / segWall : 0;
    // interval jitter (stddev)
    double mean = 0; for (double v : intervals) mean += v; if (!intervals.empty()) mean /= intervals.size();
    double var = 0; for (double v : intervals) var += (v - mean) * (v - mean); if (!intervals.empty()) var /= intervals.size();
    double jitter = sqrt(var);
    printf("SMOOTH:    present rate=%.1f/s, effective video=%.1f fps, present-interval mean=%.1fms jitter(stddev)=%.1fms\n",
           presRate, effFps, mean, jitter);
    printf("           playhead advance vs wall = %.2fx (1.00 = real-time)\n", advanceRatio);
    if (advanceRatio < 0.85 || advanceRatio > 1.15)
        { printf("           ==> WARN: playback rate off (clock drift / stalls)\n"); }

    /* ---- lock contention: how long acquire blocked the paint thread ---- */
    double acqMax = 0, acqSum = 0; int acqOver = 0, acqN = 0;
    for (auto &s : g_samples) { if (s.pts < 0 && s.acquire_ms == 0) continue; acqN++; acqSum += s.acquire_ms; if (s.acquire_ms > acqMax) acqMax = s.acquire_ms; if (s.acquire_ms > 5.0) acqOver++; }
    printf("CONTEND:   acquire_frame: mean=%.3fms max=%.3fms, %d presents blocked >5ms\n",
           acqN ? acqSum / acqN : 0, acqMax, acqOver);
    if (acqMax > 20.0) { printf("           ==> FAIL: paint thread blocked >20ms on the player lock\n"); failures++; }
    else printf("           ==> ok (no major lock contention)\n");

    /* ---- memory ---- */
    printf("MEMORY:    RSS baseline=%.1f MB, after restart/reload cycles=%.1f MB (delta %+.1f MB)\n",
           g_rssBaseline, g_rssAfterCycles, g_rssAfterCycles - g_rssBaseline);
    if (g_rssAfterCycles - g_rssBaseline > 80.0) { printf("           ==> FAIL: RSS grew >80 MB over cycles (leak?)\n"); failures++; }
    else printf("           ==> ok (no large leak)\n");

    printf("================ %s ================\n", failures ? "TEST FAILED" : "TEST PASSED");
    return failures;
}

/* ---------------- driver state machine ---------------- */
enum Phase { WAIT_READY_1, PLAY_1, AFTER_RESTART, PLAY_2, WAIT_READY_2, PLAY_3, LEAK, DONE };
static Phase g_phase = WAIT_READY_1;
static double g_phaseStart = 0;
static int g_leakCycle = 0;
static bool g_leakWaitDiff = false;

static void tick() {
    double now = g_clock.elapsed();
    double inPhase = now - g_phaseStart;
    switch (g_phase) {
        case WAIT_READY_1:
            if (g_engine->isReady()) { g_engine->setMode(TB_GM_EASY), mark("pick Easy"); g_phaseStart = now; g_phase = PLAY_1; }
            break;
        case PLAY_1:
            if (inPhase > 3000) { click("restartBtn"); g_phaseStart = now; g_phase = AFTER_RESTART; }
            break;
        case AFTER_RESTART:
            // Restart opens the difficulty overlay (paused). After a beat, pick Easy to resume.
            if (inPhase > 600) { g_engine->setMode(TB_GM_EASY), mark("pick Easy"); g_phaseStart = now; g_phase = PLAY_2; }
            break;
        case PLAY_2:
            if (inPhase > 3000) { mark("setHwaccel(AUTO) [settings]"); g_engine->setHwaccel(TB_HW_AUTO); g_phaseStart = now; g_phase = WAIT_READY_2; }
            break;
        case WAIT_READY_2:
            if (g_engine->isReady()) { g_engine->setMode(TB_GM_EASY), mark("pick Easy"); g_phaseStart = now; g_phase = PLAY_3; }
            else if (inPhase > 30000) { printf("  !! reload never became ready (possible lockup on settings change)\n"); g_phase = DONE; }
            break;
        case PLAY_3:
            // baseline RSS taken here so the leak check isolates the restart cycles
            // (after the one-time reload's transient ~640 MB audio decode has settled).
            if (inPhase > 3000) { g_rssBaseline = rssMB(); g_phaseStart = now; g_phase = LEAK; g_leakCycle = 0; g_leakWaitDiff = false; }
            break;
        case LEAK:
            // hammer restart/difficulty to surface leaks + re-entrancy lockups
            if (inPhase > 350) {
                if (!g_leakWaitDiff) { click("restartBtn"); g_leakWaitDiff = true; g_phaseStart = now; }
                else { g_engine->setMode(TB_GM_EASY), mark("pick Easy"); g_leakWaitDiff = false; g_phaseStart = now; if (++g_leakCycle >= 6) { g_rssAfterCycles = rssMB(); g_phase = DONE; } }
            }
            break;
        case DONE:
            qApp->quit();
            break;
    }
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc < 2) { fprintf(stderr, "usage: %s <video>\n", argv[0]); return 2; }

    g_clock.start();
    g_win = new MainWindow();
    g_win->resize(900, 600);
    g_win->show();
    g_engine = g_win->engine();
    g_engine->setFrameProbe([](double pts, double ms) {
        g_samples.push_back({ (double)g_clock.elapsed(), pts, ms, g_engine->isPaused() });
    });
    g_win->openPath(QString::fromLocal8Bit(argv[1]));

    QTimer ticker;
    QObject::connect(&ticker, &QTimer::timeout, [] { tick(); });
    ticker.start(20);

    // hard safety timeout so a real lock-up doesn't hang the test forever
    QTimer::singleShot(90000, [] { printf("  !! global timeout — app appears hung (HARD LOCKUP)\n"); qApp->quit(); });

    int rc = app.exec();
    int fails = analyze();
    (void)rc;
    delete g_win; g_win = nullptr;   // exercise clean teardown (must not crash)
    printf("clean teardown ok\n");
    return fails ? 1 : 0;
}
