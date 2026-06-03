/*
 * VideoWidget.h - the video surface.
 *
 * This is a QOpenGLWindow (not a QOpenGLWidget): a QOpenGLWidget renders to an
 * offscreen FBO that Qt then composites through its backing store, which adds a
 * frame of latency and jitter and makes video visibly stutter. A QOpenGLWindow
 * presents directly to a native, vsync-locked surface with no Qt compositing in
 * the path. It is embedded in the widget layout via QWidget::createWindowContainer.
 *
 * Video + all overlays (subtitles, INCOMING CALL, big status, GAME OVER, results)
 * are drawn with QPainter on the window's GL paint engine.
 */
#pragma once
#include <QOpenGLWindow>
#include <QImage>
#include <QPixmap>
#include <QElapsedTimer>
#include <QString>

class Engine;

class VideoView : public QOpenGLWindow {
    Q_OBJECT
public:
    explicit VideoView(Engine *engine);

protected:
    void paintGL() override;
    void mousePressEvent(QMouseEvent *) override;

private:
    Engine *engine_;
    QPixmap frameCache_;     // GPU-backed; rebuilt only when the frame pts changes
    QPixmap prevCache_;      // outgoing frame, kept for the Mania push transition
    double  cachePts_ = -2;
    QRect   diffRects_[4];   // on-screen difficulty buttons (web-style), hit-tested on click
    double  callAlpha_ = 0;  // eased fade for the INCOMING CALL / PICK UP overlay

    // Subtitle transitions are rendered here, not in the core timing code: the
    // engine reports the currently-active cue and the view cross-fades visual
    // changes so adjacent cues do not pop or momentarily stack on screen.
    QString subtitleCurrent_;
    QString subtitlePrevious_;
    QElapsedTimer subtitleClock_;
    int     lastTransSeq_ = 0;       // Mania push-transition tracking
    int     transDir_ = 0;
    QElapsedTimer transClock_, frameClock_;
};
