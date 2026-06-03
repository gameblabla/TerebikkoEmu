/*
 * MainWindow.h - top-level window: menu bar (Load Video / Save States / Fullscreen
 * / About), the gallery<->player stack, the 1/2/3/4 + Pick-up phone buttons, the
 * HUD and the transport bar. Keyboard input is mapped through the Engine's
 * remappable bindings.
 */
#pragma once
#include <QMainWindow>

class Engine;
class VideoView;
class Gallery;
class QStackedWidget;
class QPushButton;
class QLabel;
class QComboBox;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    void openPath(const QString &path);        // load a file directly (CLI / drag-in)
    Engine *engine() const { return engine_; } // for the GUI test harness

protected:
    void keyPressEvent(QKeyEvent *) override;

private slots:
    void onLoadVideo();
    void onOpenGalleryFolder();
    void onToggleFullscreen();
    void onAbout();
    void onSettings();
    void onSaveState();
    void onLoadState();
    void onQuitToGallery();
    void onProgress(double pct, const QString &text);
    void onAnalysisFinished(bool ok, const QString &msg);
    void onTick();
    void onGalleryActivated(const QString &path);

private:
    void buildMenus();
    QWidget *buildPlayerPage();
    void applyFullscreenLayout();              // show/hide chrome for fullscreen

    Engine *engine_;
    QStackedWidget *stack_;
    Gallery *gallery_;
    VideoView *video_;

    QPushButton *answerBtns_[4];
    QPushButton *pickupBtn_;
    QPushButton *playPauseBtn_, *restartBtn_, *saveStateBtn_, *loadStateBtn_, *fullscreenBtn_, *quitBtn_;
    QLabel *scoreL_, *streakL_, *livesL_, *correctL_, *modeBadge_, *timeL_, *statusPill_;
    QWidget *transportW_, *hudW_, *phoneW_, *sideW_;   // panels toggled in fullscreen

#ifdef TB_DEBUG_TOOLS
    QComboBox *debugJump_ = nullptr;     // dropdown of questions/sounds to skip to
    void populateDebugJump();            // fill it after analysis
#endif
};
