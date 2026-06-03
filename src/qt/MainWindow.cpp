#include "MainWindow.h"
#include "Engine.h"
#include "VideoWidget.h"
#include "Gallery.h"
#include "Dialogs.h"

#include <QMenuBar>
#include <QMenu>
#include <QStackedWidget>
#include <QPushButton>
#include <QLabel>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QKeyEvent>
#include <QStatusBar>
#include <QTimer>
#include <QScreen>
#include <QApplication>
#include <QSettings>
#include <QComboBox>

static QString fmtTime(double t) {
    if (t < 0 || t != t) return "--:--";
    int m = int(t / 60); double s = t - m * 60;
    return QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 4, 'f', 1, QChar('0'));
}

static const char *kDark =
    "QMainWindow,QWidget{background:#0c0d1a;color:#eef1ff;}"
    "QLabel{color:#eef1ff;}"
    "QPushButton{background:#16182b;color:#eef1ff;border:1px solid #2c3050;border-radius:10px;padding:8px 12px;}"
    "QPushButton:hover{background:#1d2038;}"
    "QPushButton:disabled{color:#666;}";

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setWindowTitle("TerebikkoEmu");
    setStyleSheet(kDark);
    engine_ = new Engine(this);

    stack_ = new QStackedWidget(this);
    gallery_ = new Gallery(this);
    stack_->addWidget(gallery_);            // page 0
    stack_->addWidget(buildPlayerPage());   // page 1
    setCentralWidget(stack_);

    buildMenus();
    statusBar()->showMessage("No video loaded");

    connect(engine_, &Engine::progress, this, &MainWindow::onProgress);
    connect(engine_, &Engine::analysisFinished, this, &MainWindow::onAnalysisFinished);
    connect(engine_, &Engine::frameTick, this, &MainWindow::onTick);
    connect(engine_, &Engine::requestFullscreenToggle, this, &MainWindow::onToggleFullscreen);
    connect(gallery_, &Gallery::activated, this, &MainWindow::onGalleryActivated);

    QString savedDir = QSettings().value("gallery/path").toString();
    if (!savedDir.isEmpty()) gallery_->setDirectory(savedDir);   // restore last gallery folder

    // Bounded repaint timer paced near the display refresh (NOT a frameSwapped busy
    // loop, which spins under Wayland). Only repaints when the player page is shown.
    double hz = screen() ? screen()->refreshRate() : 60.0;
    if (hz < 24.0 || hz > 480.0) hz = 60.0;
    auto *videoTimer = new QTimer(this);
    videoTimer->setTimerType(Qt::PreciseTimer);
    videoTimer->setInterval(qMax(2, int(1000.0 / hz)));
    connect(videoTimer, &QTimer::timeout, this, [this] {
        // Repaint whenever the player page is shown - including while a modal dialog
        // (Settings / About) is open, so the video keeps playing instead of freezing.
        // The video is a separate native GL window, so this doesn't slow the dialog.
        if (stack_->currentIndex() == 1) video_->update();
    });
    videoTimer->start();

    resize(1100, 720);
}
MainWindow::~MainWindow() = default;

// portable helper: works on both Qt5 and Qt6
template <typename R, typename F>
static QAction *addAct(QMenu *m, const QString &text, const QKeySequence &sc, R *recv, F slot) {
    QAction *a = m->addAction(text);
    if (!sc.isEmpty()) a->setShortcut(sc);
    QObject::connect(a, &QAction::triggered, recv, slot);
    return a;
}

void MainWindow::buildMenus() {
    QMenu *file = menuBar()->addMenu("&File");
    addAct(file, "&Load Video...", QKeySequence::Open, this, &MainWindow::onLoadVideo);
    addAct(file, "Open &Gallery Folder...", QKeySequence(), this, &MainWindow::onOpenGalleryFolder);
    file->addSeparator();
    addAct(file, "&Quit", QKeySequence::Quit, this, [this]{ close(); });

    QMenu *state = menuBar()->addMenu("&Save States");
    addAct(state, "&Save State", QKeySequence("Ctrl+S"), this, &MainWindow::onSaveState);
    addAct(state, "&Load State", QKeySequence("Ctrl+L"), this, &MainWindow::onLoadState);

    QMenu *view = menuBar()->addMenu("&View");
    addAct(view, "&Fullscreen", QKeySequence(Qt::Key_F11), this, &MainWindow::onToggleFullscreen);
    addAct(view, "Se&ttings / Controls...", QKeySequence(), this, &MainWindow::onSettings);
    addAct(view, "Show &Gallery", QKeySequence(), this, [this]{ stack_->setCurrentIndex(0); });

    QMenu *help = menuBar()->addMenu("&Help");
    addAct(help, "&About TerebikkoEmu", QKeySequence(), this, &MainWindow::onAbout);
}

// Terebikko phone-button stylesheet: bright/"lit" when enabled, greyed out when
// disabled - mirroring the web version where only the buttons you need light up
// (1-4 while a question is up, Pick up while the phone is ringing).
static QString phoneBtnCss(const QString &bg, const QString &fg, int font) {
    return QString(
        "QPushButton{background:%1;color:%2;font-size:%3px;font-weight:bold;border:none;border-radius:16px;}"
        "QPushButton:disabled{background:#191b29;color:#41465f;}"
    ).arg(bg).arg(fg).arg(font);
}

static QPushButton *bigBtn(const QString &text, const QString &css) {
    auto *b = new QPushButton(text);
    b->setMinimumHeight(72);
    b->setStyleSheet(css);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

QWidget *MainWindow::buildPlayerPage() {
    auto *page = new QWidget;
    auto *h = new QHBoxLayout(page);

    // left: video + transport
    auto *left = new QVBoxLayout;
    video_ = new VideoView(engine_);
    // Embed the native GL window directly (no QOpenGLWidget FBO compositing).
    QWidget *videoContainer = QWidget::createWindowContainer(video_, this);
    videoContainer->setMinimumSize(320, 240);
    videoContainer->setFocusPolicy(Qt::NoFocus);
    left->addWidget(videoContainer, 1);

    auto *bar = new QHBoxLayout;
    quitBtn_      = new QPushButton("← Gallery"); quitBtn_->setObjectName("quitBtn");
    playPauseBtn_ = new QPushButton("Play");      playPauseBtn_->setObjectName("playPauseBtn");
    restartBtn_   = new QPushButton("Restart");   restartBtn_->setObjectName("restartBtn");
    saveStateBtn_ = new QPushButton("Save State"); saveStateBtn_->setObjectName("saveStateBtn");
    loadStateBtn_ = new QPushButton("Load State"); loadStateBtn_->setObjectName("loadStateBtn");
    fullscreenBtn_= new QPushButton("Fullscreen"); fullscreenBtn_->setObjectName("fullscreenBtn");
    timeL_        = new QLabel("00:00.0 / --:--");
    for (auto *b : { quitBtn_, playPauseBtn_, restartBtn_, saveStateBtn_, loadStateBtn_, fullscreenBtn_ }) b->setFocusPolicy(Qt::NoFocus);
    playPauseBtn_->setStyleSheet(
        "QPushButton{background:#2f6df0;color:#fff;border:1px solid #4a82ff;border-radius:10px;padding:8px 12px;font-weight:bold;}"
        "QPushButton:hover{background:#3d7bff;}"
        "QPushButton:disabled{background:#1e2740;color:#5a6486;border-color:#28324f;}");
    playPauseBtn_->setEnabled(false);
    bar->addWidget(quitBtn_); bar->addWidget(playPauseBtn_); bar->addWidget(restartBtn_);
    bar->addWidget(saveStateBtn_); bar->addWidget(loadStateBtn_); bar->addWidget(fullscreenBtn_);
#ifdef TB_DEBUG_TOOLS
    // Debug-only: jump to any decoded question/sound. (qmake CONFIG+=debugtools)
    debugJump_ = new QComboBox; debugJump_->setObjectName("debugJump");
    debugJump_->setMinimumWidth(160); debugJump_->setFocusPolicy(Qt::NoFocus);
    auto *debugGo = new QPushButton("Go to"); debugGo->setObjectName("debugGoBtn");
    debugGo->setFocusPolicy(Qt::NoFocus);
    connect(debugGo, &QPushButton::clicked, this, [this]{
        int i = debugJump_->currentIndex();
        if (i >= 0) engine_->debugSeek(debugJump_->itemData(i).toDouble());
    });
    bar->addSpacing(12);
    bar->addWidget(new QLabel("DBG:")); bar->addWidget(debugJump_); bar->addWidget(debugGo);
#endif
    bar->addStretch(); bar->addWidget(timeL_);
    transportW_ = new QWidget; transportW_->setLayout(bar);
    left->addWidget(transportW_);
    h->addLayout(left, 1);

    connect(quitBtn_,      &QPushButton::clicked, this, &MainWindow::onQuitToGallery);
    connect(playPauseBtn_, &QPushButton::clicked, this, [this]{ engine_->togglePause(); });
    connect(restartBtn_,   &QPushButton::clicked, this, [this]{ engine_->restart(); });
    connect(saveStateBtn_, &QPushButton::clicked, this, &MainWindow::onSaveState);
    connect(loadStateBtn_, &QPushButton::clicked, this, &MainWindow::onLoadState);
    connect(fullscreenBtn_,&QPushButton::clicked, this, &MainWindow::onToggleFullscreen);

    // right: HUD + phone
    auto *side = new QVBoxLayout;
    auto *hud = new QHBoxLayout;
    scoreL_ = new QLabel("0"); streakL_ = new QLabel("0x"); livesL_ = new QLabel("∞"); correctL_ = new QLabel("0/0");
    auto hudCard = [](const QString &cap, QLabel *val) {
        auto *w = new QWidget; auto *v = new QVBoxLayout(w);
        auto *c = new QLabel(cap); c->setStyleSheet("color:#a6acd0;font-size:10px;");
        val->setStyleSheet("font-size:22px;font-weight:bold;");
        v->addWidget(c); v->addWidget(val); return w;
    };
    hud->addWidget(hudCard("SCORE", scoreL_)); hud->addWidget(hudCard("STREAK", streakL_));
    hud->addWidget(hudCard("LIVES", livesL_)); hud->addWidget(hudCard("CORRECT", correctL_));
    hudW_ = new QWidget; hudW_->setLayout(hud);
    side->addWidget(hudW_);

    modeBadge_ = new QLabel("EASY");
    modeBadge_->setStyleSheet("padding:4px 10px;border-radius:8px;background:#1b2740;color:#7c9cff;font-weight:bold;");
    side->addWidget(modeBadge_);
    // (Difficulty is chosen on the video screen itself, web-style — see VideoView.)

    // 4 phone buttons
    auto *phoneBox = new QVBoxLayout;
    auto *grid = new QGridLayout;
    const char *colors[4] = { "#db3545", "#40b96d", "#4184d9", "#e4cc42" };
    const char *fg[4]     = { "#fff", "#fff", "#fff", "#2d2d18" };
    for (int i = 0; i < 4; i++) {
        answerBtns_[i] = bigBtn(QString::number(i + 1), phoneBtnCss(colors[i], fg[i], 34));
        grid->addWidget(answerBtns_[i], i / 2, i % 2);
        connect(answerBtns_[i], &QPushButton::clicked, this, [this, i]{ engine_->onAnswer(i + 1); });
    }
    phoneBox->addLayout(grid);

    pickupBtn_ = bigBtn("☎ Pick up", phoneBtnCss("#37d99a", "#06210f", 22));
    pickupBtn_->setMinimumHeight(92);   // a bit taller than the answer buttons
    connect(pickupBtn_, &QPushButton::clicked, this, [this]{ engine_->onPickup(); });
    phoneBox->addWidget(pickupBtn_);
    phoneW_ = new QWidget; phoneW_->setLayout(phoneBox);

    statusPill_ = new QLabel("Pick a difficulty to start. Mania is an endless remix.");
    statusPill_->setWordWrap(true); statusPill_->setStyleSheet("color:#a6acd0;font-size:11px;");

    // Layout: HUD + badge on top, phone buttons vertically centered (stretches above and
    // below), status note at the bottom. In fullscreen only the phone panel is visible,
    // so the stretches keep it centered.
    side->addStretch(1);
    side->addWidget(phoneW_);
    side->addStretch(1);
    side->addWidget(statusPill_);

    sideW_ = new QWidget; sideW_->setLayout(side); sideW_->setFixedWidth(330);
    h->addWidget(sideW_);
    return page;
}

// ---- menu actions ----
void MainWindow::onLoadVideo() {
    QString p = QFileDialog::getOpenFileName(this, "Load Terebikko / See 'N Say video", QString(),
        "Media (*.mp4 *.m4v *.mkv *.webm *.mov *.avi *.mp3 *.m4a *.wav *.flac);;All files (*)");
    if (!p.isEmpty()) onGalleryActivated(p);
}
void MainWindow::onOpenGalleryFolder() {
    QString d = QFileDialog::getExistingDirectory(this, "Choose a folder of videos");
    if (!d.isEmpty()) { QSettings().setValue("gallery/path", d); gallery_->setDirectory(d); stack_->setCurrentIndex(0); }
}
void MainWindow::openPath(const QString &path) { onGalleryActivated(path); }
void MainWindow::onGalleryActivated(const QString &path) {
    stack_->setCurrentIndex(1);
    engine_->loadFile(path);
    statusBar()->showMessage("Loading: " + QFileInfo(path).fileName());
}
void MainWindow::onToggleFullscreen() {
    if (isFullScreen()) showNormal(); else showFullScreen();
    applyFullscreenLayout();
}
// In fullscreen: hide the menu bar / transport / HUD / difficulty, leaving just the
// screen and the Terebikko phone buttons (which can be disabled in Settings).
void MainWindow::applyFullscreenLayout() {
    bool fs = isFullScreen();
    bool phoneInFs = QSettings().value("ui/fullscreenButtons", true).toBool();
    menuBar()->setVisible(!fs);
    statusBar()->setVisible(!fs);
    transportW_->setVisible(!fs);
    hudW_->setVisible(!fs);
    modeBadge_->setVisible(!fs);
    statusPill_->setVisible(!fs);
    phoneW_->setVisible(!fs || phoneInFs);
    sideW_->setVisible(!fs || phoneInFs);

    // In fullscreen, scale the Terebikko buttons up proportionally; restore otherwise.
    int answerFont = fs ? 64 : 34, answerH = fs ? 150 : 72;
    int pickupFont = fs ? 44 : 22, pickupH = fs ? 170 : 92;   /* Pick Up a bit taller */
    const char *colors[4] = { "#db3545", "#40b96d", "#4184d9", "#e4cc42" };
    const char *fg[4] = { "#fff", "#fff", "#fff", "#2d2d18" };
    for (int i = 0; i < 4; i++) {
        answerBtns_[i]->setMinimumHeight(answerH);
        answerBtns_[i]->setStyleSheet(phoneBtnCss(colors[i], fg[i], answerFont));
    }
    pickupBtn_->setMinimumHeight(pickupH);
    pickupBtn_->setStyleSheet(phoneBtnCss("#37d99a", "#06210f", pickupFont));
    sideW_->setFixedWidth(fs ? (phoneInFs ? 520 : 0) : 330);
}
void MainWindow::onQuitToGallery() {
    engine_->stopToGallery();
    stack_->setCurrentIndex(0);
    setWindowTitle("TerebikkoEmu");
    statusBar()->showMessage("Gallery");
}
void MainWindow::onAbout() { AboutDialog(this).exec(); }
void MainWindow::onSettings() { SettingsDialog(engine_, this).exec(); applyFullscreenLayout(); }

void MainWindow::onSaveState() {
    if (engine_->saveState()) statusBar()->showMessage("Save state stored.", 3000);
    else QMessageBox::information(this, "Save State", "Save states are available in Easy and Hard once a video is decoded.");
}
void MainWindow::onLoadState() {
    if (engine_->loadState()) statusBar()->showMessage("Save state loaded.", 3000);
    else QMessageBox::information(this, "Load State", "No matching save state for this file in Easy/Hard.");
}

void MainWindow::onProgress(double pct, const QString &text) {
    statusBar()->showMessage(QString("%1%  %2").arg(pct, 0, 'f', 0).arg(text));
}
void MainWindow::onAnalysisFinished(bool ok, const QString &msg) {
    if (!ok) { QMessageBox::warning(this, "Load failed", msg); statusBar()->showMessage("Load failed."); return; }
    statusBar()->showMessage(msg + "  ·  Decode: " + engine_->activeHwaccel());
    setWindowTitle("TerebikkoEmu - " + engine_->fileName());
    // Ask for difficulty first (web-style): the video stays paused/blanked until the
    // player picks a difficulty, which then starts the run from the beginning.
    engine_->chooseDifficulty("start");
#ifdef TB_DEBUG_TOOLS
    populateDebugJump();
#endif
    if (qEnvironmentVariableIsSet("TB_AUTOSTART")) engine_->setMode(TB_GM_EASY);  // diagnostics
}

#ifdef TB_DEBUG_TOOLS
void MainWindow::populateDebugJump() {
    if (!debugJump_) return;
    debugJump_->clear();
    for (const auto &e : engine_->debugEvents())
        debugJump_->addItem(e.second, e.first);   // text, time(data)
}
#endif

// Only touch a widget when its value actually changed: setText/setEnabled trigger
// relayout + a backing-store repaint, and doing that every frame contends with the
// native video window's compositing (especially on Wayland) -> choppiness.
template <typename W> static void setTextIf(W *w, const QString &s) { if (w->text() != s) w->setText(s); }
static void setEnabledIf(QWidget *w, bool on) { if (w->isEnabled() != on) w->setEnabled(on); }

// ---- per-frame refresh ----
void MainWindow::onTick() {
    const tb_ui *u = engine_->ui();
    if (u) {
        setTextIf(scoreL_, QString::number(u->score));
        setTextIf(streakL_, QString::number(u->streak) + "x");
        if (u->mode == TB_GM_MANIA) setTextIf(livesL_, "HI " + QString::number(u->mania_high));
        else if (u->lives >= 1000000) setTextIf(livesL_, "∞");
        else { QString hearts; for (int i = 0; i < u->lives; i++) hearts += "♥"; setTextIf(livesL_, hearts.isEmpty() ? "-" : hearts); }
        setTextIf(correctL_, u->mode == TB_GM_MANIA ? ("R " + QString::number(u->mania_round))
                                                    : QString("%1/%2").arg(u->correct).arg(u->n_questions));
        const char *labels[] = { "EASY", "HARD", "HARD+", "MINIGAME" };
        setTextIf(modeBadge_, labels[u->mode]);
        for (int i = 0; i < 4; i++) setEnabledIf(answerBtns_[i], u->buttons_enabled);
        setEnabledIf(pickupBtn_, u->pickup_enabled);
        setTextIf(playPauseBtn_, engine_->isReady() && !engine_->isPaused() ? "Pause" : "Play");
        setEnabledIf(playPauseBtn_, engine_->isReady() && !engine_->pauseLocked());
    }
    setTextIf(timeL_, QString("%1 / %2").arg(fmtTime(engine_->currentTime())).arg(fmtTime(engine_->duration())));
    setEnabledIf(saveStateBtn_, engine_->saveAllowedNow());
    setEnabledIf(loadStateBtn_, engine_->saveAllowedNow() && engine_->hasSavedState());
    // Video repaints are driven by the dedicated refresh-rate timer, not here.
}

void MainWindow::keyPressEvent(QKeyEvent *ev) {
    if (ev->isAutoRepeat()) { QMainWindow::keyPressEvent(ev); return; }
    if (ev->key() == Qt::Key_Escape && isFullScreen()) { onToggleFullscreen(); return; }
    tb_action a = tb_input_action_for_key(engine_->input(), ev->key());
    switch (a) {
        case TB_ACT_ANS1: engine_->onAnswer(1); return;
        case TB_ACT_ANS2: engine_->onAnswer(2); return;
        case TB_ACT_ANS3: engine_->onAnswer(3); return;
        case TB_ACT_ANS4: engine_->onAnswer(4); return;
        case TB_ACT_PICKUP: engine_->onPickup(); return;
        case TB_ACT_PAUSE: engine_->togglePause(); return;
        case TB_ACT_FULLSCREEN: onToggleFullscreen(); return;
        default: break;
    }
    QMainWindow::keyPressEvent(ev);
}
