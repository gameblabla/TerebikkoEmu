/*
 * Gallery.h - fancy-emulator style grid: point it at a folder and it lists the
 * video files as icons, using sibling cover art (.png/.jpg/.jpeg) when present.
 */
#pragma once
#include <QWidget>

class QListWidget;
class QLabel;

class Gallery : public QWidget {
    Q_OBJECT
public:
    explicit Gallery(QWidget *parent = nullptr);
    void setDirectory(const QString &dir);

signals:
    void activated(const QString &path);

private:
    void rescan();
    QListWidget *list_;
    QLabel *header_;
    QString dir_;
};
