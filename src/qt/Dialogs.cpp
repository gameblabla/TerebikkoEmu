#include "Dialogs.h"
#include "Engine.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QKeyEvent>
#include <QKeySequence>
#include <QSettings>
#include <QTimer>

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent) {
    setWindowTitle("About TerebikkoEmu");
    auto *v = new QVBoxLayout(this);
    auto *title = new QLabel("<h2>TerebikkoEmu</h2>");
    auto *body = new QLabel(
        "A native C11 / Qt6 emulator for Bandai Terebikko and Mattel See 'N Say "
        "Video Phone interactive tapes, by gameblabla.<br><br>"
        "Drop in a video and play along with the four-button phone. The hidden "
        "~8 kHz control tones are decoded locally to recover questions, calls and "
        "answers. Hard mode turns it into a Dragon's-Lair quick-time challenge; "
        "Minigame is an endless remix.<br><br>"
        "Core logic is pure C11; video via ffmpeg, audio + input via SDL3.");
    body->setWordWrap(true);
    auto *ok = new QPushButton("Close");
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    v->addWidget(title); v->addWidget(body); v->addWidget(ok);
    setMinimumWidth(420);
}

static const char *kActionLabels[7] = {
    "Answer 1", "Answer 2", "Answer 3", "Answer 4", "Pick up phone", "Play / pause", "Fullscreen"
};

SettingsDialog::SettingsDialog(Engine *engine, QWidget *parent) : QDialog(parent), engine_(engine) {
    setWindowTitle("Settings / Controls");
    auto *v = new QVBoxLayout(this);

    // audio channel
    auto *chRow = new QHBoxLayout;
    chRow->addWidget(new QLabel("Audio channel:"));
    auto *ch = new QComboBox; ch->addItems({ "Auto-detect", "Left", "Right" });
    QString pref = engine_->channelPreference();
    ch->setCurrentIndex(pref == "left" ? 1 : pref == "right" ? 2 : 0);
    chRow->addWidget(ch); chRow->addStretch();
    v->addLayout(chRow);
    connect(ch, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        engine_->setChannelPreference(i == 1 ? "left" : i == 2 ? "right" : "auto");
        engine_->reanalyzeChannel();
    });

    // hardware video decode
    auto *hwRow = new QHBoxLayout;
    hwRow->addWidget(new QLabel("Video decode:"));
    auto *hw = new QComboBox;
    static const tb_hwaccel kAll[] = { TB_HW_NONE, TB_HW_AUTO, TB_HW_NVDEC, TB_HW_VAAPI,
                                       TB_HW_D3D11VA, TB_HW_MEDIACODEC, TB_HW_VIDEOTOOLBOX };
    QList<tb_hwaccel> avail;
    for (tb_hwaccel a : kAll) if (tb_hwaccel_supported(a)) { avail << a; hw->addItem(tb_hwaccel_name(a)); }
    int cur = avail.indexOf(engine_->hwaccel()); if (cur < 0) cur = 0;
    hw->setCurrentIndex(cur);
    hwRow->addWidget(hw); hwRow->addStretch();
    v->addLayout(hwRow);
    auto *hwNote = new QLabel(QString("Currently decoding with: %1. Changing this reloads the video.")
                              .arg(engine_->activeHwaccel()));
    hwNote->setStyleSheet("color:#a6acd0;font-size:11px;");
    v->addWidget(hwNote);
    connect(hw, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, avail](int i) {
        if (i >= 0 && i < avail.size()) engine_->setHwaccel(avail[i]);
    });

    // fullscreen Terebikko buttons toggle
    auto *fsBtns = new QCheckBox("Show Terebikko buttons in fullscreen");
    fsBtns->setChecked(QSettings().value("ui/fullscreenButtons", true).toBool());
    connect(fsBtns, &QCheckBox::toggled, this, [](bool on){ QSettings().setValue("ui/fullscreenButtons", on); });
    v->addWidget(fsBtns);

    v->addWidget(new QLabel("<b>Controls</b> — click a Key cell then press a key, or a Pad cell then press a gamepad button:"));
    auto *grid = new QGridLayout;
    grid->addWidget(new QLabel("<b>Action</b>"), 0, 0);
    grid->addWidget(new QLabel("<b>Key</b>"), 0, 1);
    grid->addWidget(new QLabel("<b>Gamepad</b>"), 0, 2);
    for (int i = 0; i < 7; i++) {
        grid->addWidget(new QLabel(kActionLabels[i]), i + 1, 0);
        keyBtns_[i] = new QPushButton; keyBtns_[i]->setFocusPolicy(Qt::NoFocus);
        padBtns_[i] = new QPushButton; padBtns_[i]->setFocusPolicy(Qt::NoFocus);
        const int act = i;
        connect(keyBtns_[i], &QPushButton::clicked, this, [this, act]{ awaiting_ = act; awaitingPad_ = -1; refreshKeyLabels(); });
        connect(padBtns_[i], &QPushButton::clicked, this, [this, act]{
            awaitingPad_ = act; awaiting_ = -1;
            tb_input_begin_capture(engine_->input());
            refreshKeyLabels();
        });
        grid->addWidget(keyBtns_[i], i + 1, 1);
        grid->addWidget(padBtns_[i], i + 1, 2);
    }
    v->addLayout(grid);

    auto *gpNote = new QLabel(engine_->input() && tb_input_has_gamepad(engine_->input())
                              ? "Gamepad detected." : "No gamepad detected (plug one in; it is hot-pluggable).");
    gpNote->setStyleSheet("color:#a6acd0;font-size:11px;");
    v->addWidget(gpNote);

    auto *btns = new QHBoxLayout;
    auto *reset = new QPushButton("Reset to defaults");
    auto *done = new QPushButton("Done");
    connect(reset, &QPushButton::clicked, this, [this]{
        const int defaults[TB_ACT_COUNT] = { Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4, Qt::Key_Return, Qt::Key_P, Qt::Key_F };
        tb_input_reset_defaults(engine_->input(), defaults); refreshKeyLabels();
    });
    connect(done, &QPushButton::clicked, this, &QDialog::accept);
    btns->addStretch(); btns->addWidget(reset); btns->addWidget(done);
    v->addLayout(btns);

    // poll for a captured gamepad button (the Engine's timer pumps SDL events even
    // while this modal dialog is open)
    capTimer_ = new QTimer(this);
    connect(capTimer_, &QTimer::timeout, this, [this]{
        if (awaitingPad_ < 0) return;
        int b = tb_input_take_captured(engine_->input());
        if (b >= 0) { tb_input_set_pad(engine_->input(), (tb_action)awaitingPad_, b); awaitingPad_ = -1; refreshKeyLabels(); }
    });
    capTimer_->start(30);

    refreshKeyLabels();
    setMinimumWidth(480);
}

void SettingsDialog::refreshKeyLabels() {
    for (int i = 0; i < 7; i++) {
        if (awaiting_ == i) keyBtns_[i]->setText("press a key…");
        else keyBtns_[i]->setText(QKeySequence(tb_input_get(engine_->input(), (tb_action)i).key).toString());
        if (awaitingPad_ == i) padBtns_[i]->setText("press a button…");
        else padBtns_[i]->setText(tb_input_pad_button_name(tb_input_get(engine_->input(), (tb_action)i).pad));
    }
}

void SettingsDialog::keyPressEvent(QKeyEvent *ev) {
    if (awaiting_ >= 0 && ev->key() != Qt::Key_Escape) {
        tb_input_set_key(engine_->input(), (tb_action)awaiting_, ev->key());
        awaiting_ = -1; refreshKeyLabels(); return;
    }
    QDialog::keyPressEvent(ev);
}
