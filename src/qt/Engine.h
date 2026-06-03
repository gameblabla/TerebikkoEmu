/*
 * Engine.h - C++/Qt bridge over the pure-C core.
 *
 * Owns the player, game machine, input and sfx; implements the tb_host playback
 * callbacks; runs the ~60 Hz game loop on a QTimer; loads/analyses media on a
 * worker thread. The GUI talks only to this class, never to the C API directly.
 */
#pragma once
#include <QObject>
#include <QImage>
#include <QString>
#include <QElapsedTimer>
#include <QTimer>
#include <QVector>
#include <QPair>
#include <atomic>
#include <functional>
#include <thread>

extern "C" {
#include "tb_events.h"
#include "tb_subs.h"
#include "tb_game.h"
#include "tb_player.h"
#include "tb_sfx.h"
#include "tb_input.h"
}

class Engine : public QObject {
    Q_OBJECT
public:
    explicit Engine(QObject *parent = nullptr);
    ~Engine() override;

    void loadFile(const QString &path);     // async analyse + load
    bool isReady() const { return ready_; }

    // playback / game control surfaced to the GUI
    void startFromThumbnail();               // opens difficulty then starts
    void setMode(tb_gamemode m);
    void chooseDifficulty(const QString &pendingAction);
    void togglePause();
    void restart();
    void stopToGallery();                    // stop playback + free media (back to gallery)
    void onAnswer(int n);
    void onPickup();

    bool pauseLocked() const;
    const tb_ui *ui() const { return game_ ? tb_game_ui(game_) : nullptr; }
    QImage currentFrame();                   // latest decoded frame as QImage
    double lastFramePts() const { return lastFramePts_; }  // pts of the last currentFrame()

    // Test/diagnostics hook: called on every currentFrame() with the presented pts
    // (-1 if none) and how long tb_player_acquire_frame blocked (ms) — the latter is
    // the lock-contention signal. No-op overhead when unset.
    void setFrameProbe(std::function<void(double pts, double acquireMs)> fn) { frameProbe_ = std::move(fn); }
    QString currentSubtitle() const;
    double duration() const { return duration_; }
    double currentTime() const;
    bool isPaused() const { return player_ ? tb_player_is_paused(player_) : true; }

    QString fileName() const { return fileName_; }
    QString coverArt() const { return coverArt_; }
    QString subtitleLabel() const { return subtitleLabel_; }
    QString gameName() const { return gameName_; }
    int     questionCount() const;
    long    highScore(tb_gamemode m) const;     // saved best for this game+mode

    // settings
    tb_input *input() { return input_; }
    void setChannelPreference(const QString &pref);
    QString channelPreference() const { return channelPref_; }
    void reanalyzeChannel();                 // re-pick channel without re-decoding audio

    // hardware decode
    void       setHwaccel(tb_hwaccel h);     // persists + reloads current file
    tb_hwaccel hwaccel() const { return hwaccel_; }
    QString    activeHwaccel() const { return player_ ? QString::fromUtf8(tb_player_active_hwaccel(player_)) : QStringLiteral("software"); }

    // save states
    bool saveState();
    bool loadState();
    bool hasSavedState() const;
    bool saveAllowedNow() const;

#ifdef TB_DEBUG_TOOLS
    // Compile-time debug tools (qmake CONFIG+=debugtools). Not present in release builds.
    QVector<QPair<double, QString>> debugEvents() const;  // (time, label) for the jump dropdown
    void debugSeek(double seconds);                       // jump the footage to a point
#endif

signals:
    void progress(double pct, const QString &text);
    void analysisFinished(bool ok, const QString &message);
    void frameTick();                         // every loop frame -> refresh GUI
    void requestFullscreenToggle();

private slots:
    void onTimer();

private:
    // host callbacks (C signatures, user == this)
    static void hSeek(void *u, double s);
    static void hPlay(void *u);
    static void hPause(void *u);
    static void hRate(void *u, double r);
    static void hMuted(void *u, bool m);
    static double hTime(void *u);
    static double hDuration(void *u);
    static bool hIsPaused(void *u);
    static void hSfx(void *u, tb_sfx s);
    static double hNow(void *u);
    static double hRand(void *u);
    static void hManiaMusic(void *u, bool on);
    static void hInputAction(tb_action a, void *user);

    void buildGame();                         // (re)create the game machine
    void freeMedia();
    QString stateFilePath() const;

    // analysis worker output (delivered to main thread)
    void deliverAnalysis(bool ok, QString msg);

    tb_player    *player_ = nullptr;
    tb_game      *game_   = nullptr;
    tb_event_list events_{};
    tb_cue_list   cues_{};
    tb_input     *input_  = nullptr;
    tb_sfx_engine *sfx_   = nullptr;

    QTimer        timer_;
    QElapsedTimer clock_;

    QString fileName_, filePath_, coverArt_, subtitleLabel_, channelPref_ = "auto", gameName_;
    int     gameIdx_ = -1;
    float  *musicPcm_ = nullptr; size_t musicFrames_ = 0; double musicSr_ = 0;  // mania music
    qint64  fileSize_ = 0;
    double  duration_ = 0, carrier_ = 8000;
    bool    ready_ = false;
    std::atomic<bool> loading_{false};
    std::thread analyzeThread_;     // joined before reuse / on destruction
    bool    maniaMusic_ = false;
    bool    resultsShown_ = false;     // edge-detect the results screen to record high scores
    QString highScoreKey(tb_gamemode m) const;
    void    recordHighScore();
    tb_gamemode mode_ = TB_GM_EASY;
    tb_hwaccel  hwaccel_ = TB_HW_NONE;
    double  lastFramePts_ = -1;
    std::function<void(double, double)> frameProbe_;
};
