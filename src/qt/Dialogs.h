/*
 * Dialogs.h - About (TerebikkoEmu) and Settings (remappable controls, audio
 * channel preference) dialogs.
 */
#pragma once
#include <QDialog>

class Engine;
class QPushButton;

class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget *parent = nullptr);
};

class QTimer;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(Engine *engine, QWidget *parent = nullptr);
protected:
    void keyPressEvent(QKeyEvent *) override;
private:
    void refreshKeyLabels();
    Engine *engine_;
    int awaiting_ = -1;            // tb_action awaiting a keyboard key, -1 none
    int awaitingPad_ = -1;         // tb_action awaiting a gamepad button, -1 none
    QPushButton *keyBtns_[7];
    QPushButton *padBtns_[7];
    QTimer *capTimer_ = nullptr;   // polls for a captured gamepad button
};
