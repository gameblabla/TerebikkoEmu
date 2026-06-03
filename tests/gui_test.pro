######################################################################
# gui_test.pro - automated GUI test: drives the real MainWindow with
# simulated button clicks, records the presented-frame timeline through
# the actual paint path, and reports lockups / smoothness / lock
# contention / memory growth.
#
# Build:  qmake6 gui_test.pro && make
# Run:    QT_QPA_PLATFORM=xcb DISPLAY=:99 ./tb_test_gui <video>   (under Xvfb)
######################################################################
QT += core gui widgets opengl
CONFIG += c++17 link_pkgconfig
TARGET = tb_test_gui
TEMPLATE = app

QMAKE_CFLAGS += -std=c11
PKGCONFIG += sdl3 libavformat libavcodec libavutil libswscale libswresample
INCLUDEPATH += ../src/core ../src/platform ../src/qt

SOURCES += \
    ../src/core/tb_decode.c ../src/core/tb_events.c ../src/core/tb_game.c \
    ../src/core/tb_subs.c ../src/core/tb_media.c ../src/core/tb_player.c ../src/core/tb_sfx.c \
    ../src/core/tb_gamedb.c ../src/core/tb_gamedb_data.c ../src/core/tb_mania_music.c \
    ../src/platform/tb_input.c \
    ../src/qt/Engine.cpp ../src/qt/MainWindow.cpp ../src/qt/VideoWidget.cpp \
    ../src/qt/Gallery.cpp ../src/qt/Dialogs.cpp \
    tb_test_gui.cpp

HEADERS += \
    ../src/qt/Engine.h ../src/qt/MainWindow.h ../src/qt/VideoWidget.h \
    ../src/qt/Gallery.h ../src/qt/Dialogs.h
