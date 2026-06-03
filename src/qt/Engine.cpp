#include "Engine.h"
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QRandomGenerator>
#include <QMetaObject>
#include <QFile>
#include <QDataStream>
#include <QRegularExpression>
#include <QSettings>
#include <thread>
#include <cstring>

extern "C" {
#include "tb_media.h"
#include "tb_decode.h"
#include "tb_gamedb.h"
#include "tb_mania_music.h"
}
#include <cstdlib>

Engine::Engine(QObject *parent) : QObject(parent) {
    clock_.start();
    input_ = tb_input_create();
    sfx_   = tb_sfx_create();
    // default keyboard bindings use Qt::Key codes
    const int defaults[TB_ACT_COUNT] = {
        Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4, Qt::Key_Return, Qt::Key_P, Qt::Key_F
    };
    tb_input_reset_defaults(input_, defaults);
    QString cfg = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(cfg);
    tb_input_load(input_, (cfg + "/controls.cfg").toUtf8().constData());

    QSettings st;
    hwaccel_ = (tb_hwaccel)st.value("video/hwaccel", (int)TB_HW_NONE).toInt();
    channelPref_ = st.value("audio/channel", "auto").toString();

    connect(&timer_, &QTimer::timeout, this, &Engine::onTimer);
    timer_.start(16);
}

Engine::~Engine() {
    timer_.stop();
    if (analyzeThread_.joinable()) analyzeThread_.join();   // don't tear down state under a running worker
    if (input_) {
        QString cfg = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        tb_input_save(input_, (cfg + "/controls.cfg").toUtf8().constData());
    }
    freeMedia();
    tb_input_destroy(input_);
    tb_sfx_destroy(sfx_);
}

void Engine::freeMedia() {
    if (game_) { tb_game_destroy(game_); game_ = nullptr; }
    if (player_) { tb_player_close(player_); player_ = nullptr; }
    tb_event_list_free(&events_);
    tb_cue_list_free(&cues_);
    memset(&events_, 0, sizeof events_);
    memset(&cues_, 0, sizeof cues_);
    if (sfx_) tb_sfx_set_music(sfx_, nullptr, 0, 0);
    free(musicPcm_); musicPcm_ = nullptr; musicFrames_ = 0; musicSr_ = 0;
    gameIdx_ = -1; gameName_.clear();
    ready_ = false;
}

// ---- host callbacks ----
void Engine::hSeek(void *u, double s)  { auto *e=(Engine*)u; if (e->player_) tb_player_seek(e->player_, s); }
void Engine::hPlay(void *u)            { auto *e=(Engine*)u; if (e->player_) tb_player_play(e->player_); }
void Engine::hPause(void *u)           { auto *e=(Engine*)u; if (e->player_) tb_player_pause(e->player_); }
void Engine::hRate(void *u, double r)  { auto *e=(Engine*)u; if (e->player_) tb_player_set_rate(e->player_, r); }
void Engine::hMuted(void *u, bool m)   { auto *e=(Engine*)u; if (e->player_) tb_player_set_muted(e->player_, m); }
double Engine::hTime(void *u)          { auto *e=(Engine*)u; return e->player_ ? tb_player_time(e->player_) : 0; }
double Engine::hDuration(void *u)      { auto *e=(Engine*)u; return e->player_ ? tb_player_duration(e->player_) : 0; }
bool Engine::hIsPaused(void *u)        { auto *e=(Engine*)u; return e->player_ ? tb_player_is_paused(e->player_) : true; }
void Engine::hSfx(void *u, tb_sfx s)   { auto *e=(Engine*)u; tb_sfx_play(e->sfx_, s); }
double Engine::hNow(void *u)           { auto *e=(Engine*)u; return (double)e->clock_.elapsed(); }
double Engine::hRand(void *u)          { (void)u; return QRandomGenerator::global()->generateDouble(); }
void Engine::hManiaMusic(void *u, bool on) { auto *e=(Engine*)u; e->maniaMusic_ = on; tb_sfx_music(e->sfx_, on, 0); }

void Engine::hInputAction(tb_action a, void *user) {
    auto *e = (Engine*)user;
    switch (a) {
        case TB_ACT_ANS1: e->onAnswer(1); break;
        case TB_ACT_ANS2: e->onAnswer(2); break;
        case TB_ACT_ANS3: e->onAnswer(3); break;
        case TB_ACT_ANS4: e->onAnswer(4); break;
        case TB_ACT_PICKUP: e->onPickup(); break;
        case TB_ACT_PAUSE: e->togglePause(); break;
        case TB_ACT_FULLSCREEN: emit e->requestFullscreenToggle(); break;
        default: break;
    }
}

// ---- loading / analysis ----
void Engine::loadFile(const QString &path) {
    if (loading_.load()) return;
    loading_.store(true);
    freeMedia();
    filePath_ = path;
    QFileInfo fi(path);
    fileName_ = fi.fileName();
    fileSize_ = fi.size();

    // cover art + sidecar subtitle discovery (cheap, on this thread)
    char buf[1024];
    if (tb_find_cover_art(path.toUtf8().constData(), buf, sizeof buf)) coverArt_ = QString::fromUtf8(buf);
    else coverArt_.clear();

    emit progress(2, "Decoding audio... Play is locked until decoding completes.");

    if (analyzeThread_.joinable()) analyzeThread_.join();   // previous load is done (loading_ gated)
    QString p = path;
    analyzeThread_ = std::thread([this, p]() {
        char err[256] = {0};
        double dur = 0; int w = 0, h = 0;
        tb_media_probe(p.toUtf8().constData(), &dur, &w, &h, err, sizeof err);

        tb_audio_channels au;
        bool ok = tb_media_decode_audio(p.toUtf8().constData(), &au, err, sizeof err,
            [](double pct, const char *t, void *u) {
                auto *e = (Engine*)u;
                QMetaObject::invokeMethod(e, [e, pct, t]() { emit e->progress(pct, QString::fromUtf8(t)); }, Qt::QueuedConnection);
            }, this);

        QString msg;
        if (!ok) { msg = QString::fromUtf8(err); }
        else {
            QMetaObject::invokeMethod(this, [this]() { emit progress(56, "Scanning channels for the control tone..."); }, Qt::QueuedConnection);
            // analyzeChannels: decode each channel, pick the one with most answers
            int order[8], no = 0;
            if (channelPref_ == "left") order[no++] = 0;
            else if (channelPref_ == "right" && au.nch > 1) order[no++] = 1;
            else for (int c = 0; c < au.nch && c < 8; c++) order[no++] = c;

            int bestAns = -1; double bestCar = 8000; tb_packet_list best; tb_packet_list_init(&best);
            for (int i = 0; i < no; i++) {
                int c = order[i];
                tb_packet_list pk; tb_packet_list_init(&pk);
                double car = tb_decode_channel(au.chan[c], au.nsamples, au.sample_rate, 0, &pk, nullptr, nullptr);
                int ans = 0; for (size_t k = 0; k < pk.count; k++) if (pk.data[k].answer >= 1 && pk.data[k].answer <= 4) ans++;
                if (ans > bestAns) { bestAns = ans; bestCar = car; tb_packet_list_free(&best); best = pk; }
                else tb_packet_list_free(&pk);
            }
            carrier_ = bestCar; duration_ = dur;
            tb_event_list_init(&events_);
            tb_build_events(&best, bestCar, &events_);
            tb_packet_list_free(&best);

            // ---- Mania music: sibling music.mp3, else the game's in-video song (Sailor Moon) ----
            gameIdx_ = tb_gamedb_detect(&events_);
            gameName_ = gameIdx_ >= 0 ? QString::fromUtf8(tb_gamedb_name(gameIdx_)) : QString();
            free(musicPcm_); musicPcm_ = nullptr; musicFrames_ = 0; musicSr_ = 0;
            QString musicPath = QFileInfo(p).absolutePath() + "/music.mp3";
            if (QFile::exists(musicPath)) {
                tb_mania_music_from_file(musicPath.toUtf8().constData(), &musicPcm_, &musicFrames_, &musicSr_);
            } else if (gameIdx_ >= 0 && tb_gamedb_mania_anchor(gameIdx_)) {
                int ch = au.nch > 1 ? 1 : 0;            // detector uses one channel; right tends to carry tones
                tb_mania_music_detect_clip(au.chan[ch], au.nsamples, au.sample_rate, &events_,
                                           tb_gamedb_mania_anchor(gameIdx_), &musicPcm_, &musicFrames_, &musicSr_);
            }
            tb_audio_channels_free(&au);

            // subtitles: sidecar first, else embedded
            tb_cue_list_init(&cues_);
            char sub[1024];
            if (tb_find_sidecar_subtitle(p.toUtf8().constData(), sub, sizeof sub) && tb_load_sidecar_subtitles(sub, &cues_)) {
                subtitleLabel_ = QString("Sidecar: %1").arg(QFileInfo(QString::fromUtf8(sub)).fileName());
            } else if (tb_media_extract_subtitles(p.toUtf8().constData(), &cues_)) {
                subtitleLabel_ = "Embedded subtitles";
            } else subtitleLabel_.clear();

            int nQ = 0, nR = 0;
            for (size_t i = 0; i < events_.count; i++) { if (events_.data[i].type == TB_EV_ANSWER) nQ++; else if (events_.data[i].type == TB_EV_PICKUP) nR++; }
            msg = QString("Scan complete. %1 questions, %2 calls, %3 kHz carrier.%4")
                    .arg(nQ).arg(nR).arg(qRound(bestCar / 1000.0))
                    .arg(cues_.count ? QString(" Subtitles: %1 (%2 cues).").arg(subtitleLabel_).arg(cues_.count) : QString());
        }
        QMetaObject::invokeMethod(this, [this, ok, msg]() { deliverAnalysis(ok, msg); }, Qt::QueuedConnection);
    });
}

void Engine::deliverAnalysis(bool ok, QString msg) {
    loading_.store(false);
    if (!ok) { emit analysisFinished(false, msg); return; }
    char err[256] = {0};
    player_ = tb_player_open(filePath_.toUtf8().constData(), hwaccel_, err, sizeof err);
    if (!player_) { emit analysisFinished(false, QString("Playback open failed: %1").arg(QString::fromUtf8(err))); return; }
    buildGame();
    if (sfx_) tb_sfx_set_music(sfx_, musicPcm_, musicFrames_, musicSr_);
    ready_ = true;
    // preview frame at ~4s like the JS thumbnail
    tb_player_seek(player_, qMin(4.0, qMax(0.0, duration_ - 0.1)));
    QString extra = !gameName_.isEmpty() ? ("  ·  " + gameName_) : QString();
    if (sfx_ && tb_sfx_has_music(sfx_)) extra += "  ·  Mania music ready";
    emit progress(100, msg + extra);
    emit analysisFinished(true, msg + extra);
}

void Engine::buildGame() {
    if (game_) { tb_game_destroy(game_); game_ = nullptr; }
    tb_host host = { hSeek, hPlay, hPause, hRate, hMuted, hTime, hDuration, hIsPaused, hSfx, hNow, hRand, hManiaMusic };
    game_ = tb_game_create(&events_, &host, this);
    tb_game_set_mode(game_, mode_);
}

// ---- loop ----
void Engine::onTimer() {
    if (input_) tb_input_poll(input_, hInputAction, this);
    if (sfx_ && maniaMusic_) tb_sfx_tick(sfx_, (double)clock_.elapsed());
    if (game_ && player_) {
        if (tb_player_ended(player_) && tb_game_running(game_)) tb_game_on_media_ended(game_);
        const tb_ui *u = tb_game_ui(game_);
        tb_sfx_set_round(sfx_, u->mania_round);
        tb_game_tick(game_);
        // record a high score when the results screen first appears
        if (u->results && !resultsShown_) { recordHighScore(); resultsShown_ = true; }
        else if (!u->results) resultsShown_ = false;
    }
    emit frameTick();
}

QString Engine::highScoreKey(tb_gamemode m) const {
    QString game = !gameName_.isEmpty() ? gameName_ : fileName_;
    static const char *mn[] = { "easy", "hard", "veryhard", "mania" };
    return QString("highscores/%1/%2").arg(game).arg(mn[m]);
}
long Engine::highScore(tb_gamemode m) const { return QSettings().value(highScoreKey(m), 0).toLongLong(); }
void Engine::recordHighScore() {
    const tb_ui *u = ui(); if (!u) return;
    long score = (mode_ == TB_GM_MANIA) ? u->mania_high : u->score;   // mania tracks its own high
    QString key = highScoreKey(mode_);
    if (score > QSettings().value(key, 0).toLongLong()) QSettings().setValue(key, (qlonglong)score);
}

// ---- control surface ----
void Engine::startFromThumbnail() { if (game_) chooseDifficulty("start"); }
void Engine::setMode(tb_gamemode m) { mode_ = m; if (game_) tb_game_set_mode(game_, m); }
void Engine::chooseDifficulty(const QString &pendingAction) {
    if (game_) tb_game_open_difficulty(game_, pendingAction.isEmpty() ? nullptr : pendingAction.toUtf8().constData());
}
void Engine::togglePause() {
    if (!game_ || !player_) return;
    if (pauseLocked()) return;
    // Difficulty-first: while the difficulty prompt is up, Play must not bypass it;
    // the run starts only when a difficulty is chosen.
    const tb_ui *u = tb_game_ui(game_);
    if (u && u->difficulty_overlay) return;
    if (tb_player_is_paused(player_)) {
        if (!tb_game_running(game_) && !tb_game_ended(game_)) tb_game_start(game_, currentTime() <= 0.2);
        else tb_player_play(player_);
    } else tb_player_pause(player_);
}
void Engine::restart() { if (game_) chooseDifficulty("restart"); }

#ifdef TB_DEBUG_TOOLS
QVector<QPair<double, QString>> Engine::debugEvents() const {
    QVector<QPair<double, QString>> v;
    int qn = 0;
    for (size_t i = 0; i < events_.count; i++) {
        const tb_event *e = &events_.data[i];
        QString k;
        switch (e->type) {
            case TB_EV_ANSWER: k = QString("Q#%1  answer=%2").arg(++qn).arg(e->answer); break;
            case TB_EV_PICKUP: k = "PICK UP"; break;
            case TB_EV_INTRO:  k = "intro"; break;
            default:           k = "sound"; break;
        }
        int mm = int(e->start) / 60, ss = int(e->start) % 60;
        v.append({ e->start, QString("%1:%2  %3").arg(mm, 2, 10, QChar('0')).arg(ss, 2, 10, QChar('0')).arg(k) });
    }
    return v;
}
void Engine::debugSeek(double seconds) {
    if (!player_) return;
    tb_player_seek(player_, seconds);
    tb_player_play(player_);   // resume so the jump is observable
}
#endif
void Engine::stopToGallery() {
    if (analyzeThread_.joinable()) analyzeThread_.join();
    freeMedia();
    lastFramePts_ = -1;
}
void Engine::onAnswer(int n) { if (game_) tb_game_on_answer(game_, n); }
void Engine::onPickup() { if (game_) tb_game_on_pickup(game_); }
bool Engine::pauseLocked() const { return game_ ? tb_game_pause_locked(game_) : false; }

double Engine::currentTime() const { return player_ ? tb_player_time(player_) : 0; }
int Engine::questionCount() const {
    int n = 0; for (size_t i = 0; i < events_.count; i++) if (events_.data[i].type == TB_EV_ANSWER) n++;
    return n;
}

QImage Engine::currentFrame() {
    if (!player_) { lastFramePts_ = -1; if (frameProbe_) frameProbe_(-1, 0); return QImage(); }
    qint64 t0 = clock_.nsecsElapsed();
    tb_frame f = tb_player_acquire_frame(player_);
    double acquireMs = (clock_.nsecsElapsed() - t0) / 1.0e6;
    if (frameProbe_) frameProbe_(f.valid ? f.pts : -1, acquireMs);
    if (!f.valid) { lastFramePts_ = -1; return QImage(); }
    lastFramePts_ = f.pts;
    // Wrap the player's display buffer without copying. It is only rewritten by the
    // next acquire (same GUI thread), and the returned image is drawn before then.
    return QImage(f.rgba, f.width, f.height, f.stride, QImage::Format_RGBA8888);
}

QString Engine::currentSubtitle() const {
    if (!cues_.count || !game_) return QString();
    const tb_ui *u = tb_game_ui(game_);
    if (!u) return QString();
    if (!tb_game_running(game_) && !tb_game_ended(game_)) return QString(); // keep preview clean

    const double t = currentTime();
    QString out;

    // Treat subtitle intervals as half-open: [start, end). The former symmetric
    // +/-20 ms tolerance made adjacent cues overlap briefly, so the view would draw
    // both the outgoing and incoming caption for a frame or two. The Qt view now
    // handles visual smoothing, so the timing query should return only cues that
    // are actually active at the current playhead.
    for (size_t i = 0; i < cues_.count; i++) {
        const tb_cue &cue = cues_.data[i];
        if (cue.start > t) break;
        if (t >= cue.start && t < cue.end) {
            if (!out.isEmpty()) out += "\n";
            out += QString::fromUtf8(cue.text);
        }
    }
    return out;
}

void Engine::setChannelPreference(const QString &pref) {
    channelPref_ = pref;
    QSettings().setValue("audio/channel", pref);
}
void Engine::reanalyzeChannel() { if (!filePath_.isEmpty()) loadFile(filePath_); }

void Engine::setHwaccel(tb_hwaccel h) {
    if (h == hwaccel_) return;
    hwaccel_ = h;
    QSettings().setValue("video/hwaccel", (int)h);
    if (!filePath_.isEmpty()) loadFile(filePath_);   // reopen with the new decoder
}

// ---- save states (binary file keyed by name+size) ----
QString Engine::stateFilePath() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/states";
    QDir().mkpath(dir);
    QString key = fileName_.toLower() + "_" + QString::number(fileSize_);
    key.replace(QRegularExpression("[^a-z0-9_]"), "_");
    return dir + "/" + key + ".tbs";
}

bool Engine::saveAllowedNow() const {
    return ready_ && game_ && tb_game_mode_allows_save(tb_game_mode(game_));
}
bool Engine::hasSavedState() const { return QFile::exists(stateFilePath()); }

bool Engine::saveState() {
    if (!saveAllowedNow()) return false;
    tb_savestate st;
    if (!tb_game_snapshot(game_, &st)) return false;
    QFile f(stateFilePath());
    if (!f.open(QIODevice::WriteOnly)) { tb_savestate_free(&st); return false; }
    QDataStream s(&f); s.setByteOrder(QDataStream::LittleEndian);
    s << (qint32)st.version << (qint32)st.mode << st.time << (qint32)st.cur_idx
      << (qint64)st.score << (qint64)st.streak << (qint64)st.best
      << (qint32)st.lives << (qint32)st.correct << (qint32)st.wrong
      << st.pause_lock_until << (qint32)st.n_play;
    for (int i = 0; i < st.n_play; i++) { s << (quint8)st.done[i] << (qint32)st.got[i]; }
    tb_savestate_free(&st);
    return true;
}

bool Engine::loadState() {
    if (!saveAllowedNow() || !hasSavedState()) return false;
    QFile f(stateFilePath());
    if (!f.open(QIODevice::ReadOnly)) return false;
    QDataStream s(&f); s.setByteOrder(QDataStream::LittleEndian);
    tb_savestate st; memset(&st, 0, sizeof st);
    qint32 version, mode, curIdx, lives, correct, wrong, nplay;
    qint64 score, streak, best;
    s >> version >> mode >> st.time >> curIdx >> score >> streak >> best
      >> lives >> correct >> wrong >> st.pause_lock_until >> nplay;
    st.version = version; st.mode = (tb_gamemode)mode; st.cur_idx = curIdx;
    st.score = score; st.streak = streak; st.best = best; st.lives = lives;
    st.correct = correct; st.wrong = wrong; st.n_play = nplay;
    st.done = (unsigned char*)malloc((size_t)nplay + 1);
    st.got  = (int*)malloc(((size_t)nplay + 1) * sizeof(int));
    for (int i = 0; i < nplay; i++) { quint8 d; qint32 g; s >> d >> g; st.done[i] = d; st.got[i] = g; }
    mode_ = st.mode;
    bool ok = tb_game_restore(game_, &st);
    tb_savestate_free(&st);
    return ok;
}
