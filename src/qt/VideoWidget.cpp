#include "VideoWidget.h"
#include "Engine.h"
#include <QPainter>
#include <QMouseEvent>
#include <QFontMetrics>
#include <QGuiApplication>

VideoView::VideoView(Engine *engine) : engine_(engine) {
    setMinimumSize(QSize(320, 240));
    transClock_.start(); frameClock_.start(); subtitleClock_.start();
    // NB: repaints are driven by a bounded refresh-rate timer in MainWindow, NOT a
    // frameSwapped->update self-loop. That loop only throttles if swapBuffers blocks
    // on vsync; under compositors that present via frame callbacks (Wayland), the swap
    // doesn't block, so the loop spins and starves the whole event loop.
}

static QRect fitRect(const QSize &dst, const QSize &src) {
    if (src.isEmpty()) return QRect(QPoint(0, 0), dst);
    double s = qMin(dst.width() / (double)src.width(), dst.height() / (double)src.height());
    int w = int(src.width() * s), h = int(src.height() * s);
    return QRect((dst.width() - w) / 2, (dst.height() - h) / 2, w, h);
}

static double easeSubtitle(double x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    return x * x * (3.0 - 2.0 * x);   // smoothstep
}

static void drawSubtitle(QPainter &p, const QRect &vid, int viewHeight,
                         const QFont &baseFont, const QString &sub, double opacity) {
    if (sub.isEmpty() || opacity <= 0.001) return;

    QFont f = baseFont;
    f.setBold(true);
    f.setPointSizeF(qMax(12.0, viewHeight * 0.045));

    QRect box = vid.adjusted(vid.width() * 0.06, 0,
                             -vid.width() * 0.06, -vid.height() * 0.05);
    QRect tr = QFontMetrics(f).boundingRect(
        box, Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap, sub);
    tr.moveBottom(box.bottom());

    p.save();
    p.setFont(f);
    p.setOpacity(opacity);
    p.setPen(Qt::black);
    for (int dx = -2; dx <= 2; dx++) {
        for (int dy = -2; dy <= 2; dy++) {
            p.drawText(tr.translated(dx, dy),
                       Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap, sub);
        }
    }
    p.setPen(Qt::white);
    p.drawText(tr, Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap, sub);
    p.restore();
}

void VideoView::paintGL() {
    const QRect full(QPoint(0, 0), size());
    const int H = height();
    const QFont baseFont = QGuiApplication::font();

    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);   // bilinear on the GPU
    p.fillRect(full, Qt::black);

    const tb_ui *uPre = engine_->ui();
    bool blank = uPre && uPre->difficulty_overlay;   // blank the image during difficulty
                                                     // selection / restart (anti-cheat)

    // A Mania prompt change kicks off a push transition. Detect it BEFORE refreshing the
    // frame cache so we keep the outgoing frame to slide it out under the incoming one.
    bool maniaMode = uPre && uPre->mode == TB_GM_MANIA;
    if (uPre && uPre->transition_seq != lastTransSeq_) {
        lastTransSeq_ = uPre->transition_seq;
        transDir_ = uPre->transition_dir;
        prevCache_ = frameCache_;            // outgoing frame
        transClock_.restart();
    }

    QImage frame = blank ? QImage() : engine_->currentFrame();
    QRect vid = full;
    if (!frame.isNull()) {
        // Upload to a GPU-backed pixmap only when the frame actually changes; redraw
        // the cached texture every vsync. Avoids re-uploading 11 MB per refresh.
        double pts = engine_->lastFramePts();
        if (pts != cachePts_ || frameCache_.size() != frame.size()) {
            frameCache_ = QPixmap::fromImage(frame);
            cachePts_ = pts;
        }
        vid = fitRect(size(), frameCache_.size());

        // Mania push transition: the new picture slides in from the prompt's direction
        // and physically pushes the previous one off-screen (not a black wipe).
        double te = transClock_.elapsed() / 300.0;   // 300ms push
        if (maniaMode && te >= 0 && te < 1.0 && !prevCache_.isNull()) {
            double prog = te * te * (3 - 2 * te);     // smoothstep ease
            const int W = width(), Hh = height();
            int dxN = 0, dyN = 0, dxO = 0, dyO = 0;
            switch (transDir_) {
                case 0: dyN = -int(Hh * (1 - prog)); dyO =  int(Hh * prog); break;  // push down (from top)
                case 1: dxN = -int(W  * (1 - prog)); dxO =  int(W  * prog); break;  // push right (from left)
                case 2: dxN =  int(W  * (1 - prog)); dxO = -int(W  * prog); break;  // push left (from right)
                default:dyN =  int(Hh * (1 - prog)); dyO = -int(Hh * prog); break;  // push up (from bottom)
            }
            QRect vidPrev = fitRect(size(), prevCache_.size());
            p.drawPixmap(vidPrev.translated(dxO, dyO), prevCache_);
            p.drawPixmap(vid.translated(dxN, dyN), frameCache_);
        } else {
            p.drawPixmap(vid, frameCache_);
        }
    }

    const tb_ui *u = engine_->ui();

    // subtitle overlay: cue changes are cross-faded to avoid hard pops and the
    // transient old+new stacking that made subtitle switches look glitchy.
    QString sub = engine_->currentSubtitle();
    if (sub != subtitleCurrent_) {
        subtitlePrevious_ = subtitleCurrent_;
        subtitleCurrent_ = sub;
        subtitleClock_.restart();
    }

    const double subFadeMs = 140.0;
    double subProgress = easeSubtitle(qMin(1.0, subtitleClock_.elapsed() / subFadeMs));
    if (!subtitlePrevious_.isEmpty() && subProgress < 1.0)
        drawSubtitle(p, vid, H, baseFont, subtitlePrevious_, 1.0 - subProgress);
    if (!subtitleCurrent_.isEmpty())
        drawSubtitle(p, vid, H, baseFont, subtitleCurrent_, subProgress);
    if (subProgress >= 1.0) subtitlePrevious_.clear();

    if (!u) return;

    auto centerText = [&](const QString &t, const QColor &c, double frac) {
        QFont f = baseFont; f.setBold(true); f.setPointSizeF(qMax(20.0, H * frac));
        p.setFont(f); p.setPen(c);
        p.drawText(full, Qt::AlignCenter, t);
    };

    // INCOMING CALL / PICK UP - smooth fade in/out
    double dt = frameClock_.restart() / 1000.0; if (dt < 0 || dt > 0.1) dt = 0.016;
    double target = u->call_overlay ? 1.0 : 0.0;
    callAlpha_ += (target - callAlpha_) * qMin(1.0, dt / 0.09);   // quick ~90ms ease
    if (callAlpha_ > 0.01) {
        double a = callAlpha_;
        p.fillRect(full, QColor(8, 9, 20, int(150 * a)));
        QFont f = baseFont; f.setBold(true); f.setPointSizeF(qMax(18.0, H * 0.05));
        p.setFont(f); QColor c(255, 209, 102); c.setAlphaF(a); p.setPen(c);
        QRect r = full.adjusted(0, H / 2 - 60, 0, 0);
        p.drawText(r, Qt::AlignHCenter | Qt::AlignTop, QString::fromUtf8(u->call_title));
        QFont f2 = baseFont; f2.setPointSizeF(qMax(12.0, H * 0.03)); p.setFont(f2);
        QColor c2(200, 200, 200); c2.setAlphaF(a); p.setPen(c2);
        p.drawText(r.adjusted(0, int(H * 0.07), 0, 0), Qt::AlignHCenter | Qt::AlignTop, QString::fromUtf8(u->call_hint));
    }

    // big status (CORRECT / WRONG / ANSWER / BREAK ...)
    if (u->status_overlay) {
        QColor c = u->status_kind == TB_STATUS_GOOD ? QColor(55, 217, 154)
                 : u->status_kind == TB_STATUS_BAD ? QColor(255, 93, 116) : QColor(255, 209, 102);
        centerText(QString::fromUtf8(u->status_text), c, 0.10);
    }

    // GAME OVER / MISS
    if (u->game_over) {
        p.fillRect(full, QColor(0, 0, 0, 200));
        centerText(QString::fromUtf8(u->game_over_text), Qt::white, 0.11);
        QFont f = baseFont; f.setPointSizeF(qMax(12.0, H * 0.028)); p.setFont(f); p.setPen(Qt::lightGray);
        p.drawText(full.adjusted(0, H / 2 + 50, 0, 0), Qt::AlignHCenter | Qt::AlignTop, QString::fromUtf8(u->game_over_sub));
    }

    // results card
    if (u->results) {
        p.fillRect(full, QColor(6, 7, 16, 230));
        QFont ft = baseFont; ft.setBold(true); ft.setPointSizeF(qMax(16.0, H * 0.04)); p.setFont(ft); p.setPen(Qt::white);
        QRect r = full.adjusted(0, H / 2 - 90, 0, 0);
        p.drawText(r, Qt::AlignHCenter | Qt::AlignTop, QString::fromUtf8(u->result_title));
        QFont fs = baseFont; fs.setBold(true); fs.setPointSizeF(qMax(30.0, H * 0.08)); p.setFont(fs); p.setPen(QColor(55, 217, 154));
        p.drawText(r.adjusted(0, int(H * 0.05), 0, 0), Qt::AlignHCenter | Qt::AlignTop, QString::number(u->result_score));
        QFont fl = baseFont; fl.setPointSizeF(qMax(12.0, H * 0.028)); p.setFont(fl); p.setPen(Qt::lightGray);
        p.drawText(r.adjusted(0, int(H * 0.16), 0, 0), Qt::AlignHCenter | Qt::AlignTop, QString::fromUtf8(u->result_line));
        QFont fh = baseFont; fh.setPointSizeF(qMax(11.0, H * 0.022)); p.setFont(fh); p.setPen(Qt::gray);
        p.drawText(full.adjusted(0, 0, 0, -10), Qt::AlignHCenter | Qt::AlignBottom,
                   "Use the toolbar / Restart to play again or switch difficulty.");
    }

    // (Mania screen transition is a content push, handled in the frame-draw block above.)

    // difficulty prompt with on-screen buttons (web-style); image already blanked above
    if (u->difficulty_overlay) {
        p.fillRect(full, Qt::black);
        QFont ft = baseFont; ft.setBold(true); ft.setPointSizeF(qMax(18.0, H * 0.05));
        p.setFont(ft); p.setPen(QColor(124, 156, 255));
        QRect title = QRect(0, int(H * 0.30), width(), int(H * 0.12));
        p.drawText(title, Qt::AlignCenter, "Choose a difficulty to start");

        static const char *dn[4] = { "Easy", "Hard", "Hard+", "MiniGame" };
        static const QColor dc[4] = { QColor(0x40,0xb9,0x6d), QColor(0xdb,0x35,0x45),
                                      QColor(0xe4,0xcc,0x42), QColor(0x7c,0x9c,0xff) };
        int bw = qMin(int(width() * 0.20), 220), bh = qMax(44, int(H * 0.10)), gap = int(bw * 0.12);
        int totalW = bw * 4 + gap * 3, x0 = (width() - totalW) / 2, y0 = int(H * 0.50);
        QFont bf = baseFont; bf.setBold(true); bf.setPointSizeF(qMax(13.0, H * 0.03)); p.setFont(bf);
        for (int i = 0; i < 4; i++) {
            diffRects_[i] = QRect(x0 + i * (bw + gap), y0, bw, bh);
            p.setPen(Qt::NoPen); p.setBrush(dc[i]);
            p.drawRoundedRect(diffRects_[i], 12, 12);
            p.setPen(i == 2 ? QColor(0x2d,0x2d,0x18) : Qt::white);
            p.drawText(diffRects_[i], Qt::AlignCenter, dn[i]);
        }
        p.setBrush(Qt::NoBrush);
    } else {
        for (int i = 0; i < 4; i++) diffRects_[i] = QRect();
    }
}

void VideoView::mousePressEvent(QMouseEvent *ev) {
    const tb_ui *u = engine_->ui();
    // Only the on-screen difficulty buttons are clickable; clicking the video itself
    // does nothing (it must NOT trigger difficulty selection).
    if (!u || !u->difficulty_overlay) return;
    QPoint pos = ev->position().toPoint();
    static const tb_gamemode dm[4] = { TB_GM_EASY, TB_GM_HARD, TB_GM_VERYHARD, TB_GM_MANIA };
    for (int i = 0; i < 4; i++)
        if (diffRects_[i].contains(pos)) { engine_->setMode(dm[i]); return; }
}
