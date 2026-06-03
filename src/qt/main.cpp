/* main.cpp - TerebikkoEmu entry point. */
#include <QApplication>
#include <QSurfaceFormat>
#include "MainWindow.h"

int main(int argc, char **argv) {
    // vsync-locked presentation for tear-free, smoothly paced video
    QSurfaceFormat fmt;
    fmt.setSwapInterval(1);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    // config + state live in ~/.config/terebikkoemu (controls.cfg, gallery path, high scores)
    QApplication::setApplicationName("terebikkoemu");
    QApplication::setOrganizationName("terebikkoemu");
    MainWindow w;
    w.show();
    if (argc > 1) w.openPath(QString::fromLocal8Bit(argv[1]));   // TerebikkoEmu <file>
    return app.exec();
}
