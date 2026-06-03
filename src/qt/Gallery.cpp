#include "Gallery.h"
extern "C" {
#include "tb_subs.h"
}
#include <QListWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QDir>
#include <QFileInfo>
#include <QPixmap>
#include <QIcon>
#include <QList>
#include <QStringList>

Gallery::Gallery(QWidget *parent) : QWidget(parent) {
    auto *v = new QVBoxLayout(this);
    header_ = new QLabel("Open a gallery folder from the File menu to browse videos.");
    header_->setStyleSheet("color:#a6acd0;padding:6px;");
    v->addWidget(header_);

    list_ = new QListWidget;
    list_->setViewMode(QListView::IconMode);
    list_->setIconSize(QSize(220, 165));
    list_->setGridSize(QSize(250, 220));
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setSpacing(10);
    list_->setWordWrap(true);
    list_->setStyleSheet("QListWidget{background:#0c0d1a;border:none;}"
                         "QListWidget::item{color:#eef1ff;}"
                         "QListWidget::item:selected{background:#1d2038;border:1px solid #7c9cff;border-radius:8px;}");
    v->addWidget(list_, 1);

    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) {
        if (it) emit activated(it->data(Qt::UserRole).toString());
    });
}

static const QStringList kVideoExt = {
    "*.mp4", "*.m4v", "*.mkv", "*.webm", "*.mov", "*.avi", "*.mpg", "*.mpeg", "*.ogv"
};

static QFileInfoList videoFilesInDirectory(const QDir &dir) {
    return dir.entryInfoList(kVideoExt, QDir::Files | QDir::Readable, QDir::Name | QDir::IgnoreCase);
}

static QFileInfoList collectGalleryFilesOneLevelDeep(const QDir &root) {
    QFileInfoList files = videoFilesInDirectory(root);

    const QFileInfoList children = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
                                                      QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &child : children) {
        QDir childDir(child.absoluteFilePath());
        files.append(videoFilesInDirectory(childDir));
    }
    return files;
}

void Gallery::setDirectory(const QString &dir) { dir_ = dir; rescan(); }

void Gallery::rescan() {
    list_->clear();
    QDir d(dir_);
    if (!d.exists()) { header_->setText("Folder not found."); return; }

    QFileInfoList files = collectGalleryFilesOneLevelDeep(d);
    header_->setText(QString("Gallery: %1  (%2 videos, including one folder deep) - double-click to play")
                         .arg(dir_)
                         .arg(files.size()));

    for (const QFileInfo &fi : files) {
        const QString rel = d.relativeFilePath(fi.absoluteFilePath());
        const QString label = rel.contains('/') ? rel.section('/', 0, -2) + " / " + fi.completeBaseName()
                                                : fi.completeBaseName();
        auto *it = new QListWidgetItem(label);
        it->setData(Qt::UserRole, fi.absoluteFilePath());
        it->setToolTip(fi.absoluteFilePath());
        it->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);

        char cover[1024];
        if (tb_find_cover_art(fi.absoluteFilePath().toUtf8().constData(), cover, sizeof cover)) {
            QPixmap pm(QString::fromUtf8(cover));
            if (!pm.isNull()) it->setIcon(QIcon(pm.scaled(220, 165, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        }
        if (it->icon().isNull()) {
            QPixmap pm(220, 165); pm.fill(QColor(22, 24, 43));
            it->setIcon(QIcon(pm));
        }
        list_->addItem(it);
    }
}
