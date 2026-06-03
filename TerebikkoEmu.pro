######################################################################
# TerebikkoEmu - Qt6 + qmake project
#
# Pure-C11 core (src/core, src/platform) + a thin C++/Qt frontend (src/qt).
# Links SDL3 (audio output + input) and ffmpeg/libav* (demux + decode) via
# pkg-config. The core has no Qt dependency so it can back a future Win32 frontend.
#
# Build:   qmake6 && make        (or: qmake CONFIG+=qt6 && make)
# Requires Qt6 Widgets, SDL3, and ffmpeg (libavformat/avcodec/avutil/swscale/swresample) dev packages.
######################################################################

QT += core gui widgets opengl
CONFIG += c++17 link_pkgconfig
TARGET = TerebikkoEmu
TEMPLATE = app

# C11 for the core
QMAKE_CFLAGS += -std=c11
QMAKE_CFLAGS_WARN_ON += -Wall -Wextra

PKGCONFIG += sdl3 libavformat libavcodec libavutil libswscale libswresample

INCLUDEPATH += src/core src/platform src/qt

# ---- optional compile-time debug tools (event jump dropdown + "Go to") ----
# Enable with:  qmake6 CONFIG+=debugtools && make
# Never defined in a normal/release build.
debugtools {
    DEFINES += TB_DEBUG_TOOLS
    message("TerebikkoEmu: debug tools ENABLED (event jump dropdown)")
}

# ---- compile-time game database: regenerate src/core/tb_gamedb_data.c from gamedb/*.txt ----
system(gcc -std=c11 -O2 -o $$PWD/tools/gen_gamedb $$PWD/tools/gen_gamedb.c)
system($$PWD/tools/gen_gamedb $$PWD/src/core/tb_gamedb_data.c $$files($$PWD/gamedb/*.txt))

# ---- pure C core (no Qt) ----
SOURCES += \
    src/core/tb_decode.c \
    src/core/tb_events.c \
    src/core/tb_game.c \
    src/core/tb_subs.c \
    src/core/tb_media.c \
    src/core/tb_player.c \
    src/core/tb_sfx.c \
    src/core/tb_gamedb.c \
    src/core/tb_gamedb_data.c \
    src/core/tb_mania_music.c \
    src/platform/tb_input.c

HEADERS += \
    src/core/tb_decode.h \
    src/core/tb_events.h \
    src/core/tb_game.h \
    src/core/tb_subs.h \
    src/core/tb_media.h \
    src/core/tb_player.h \
    src/core/tb_sfx.h \
    src/core/tb_gamedb.h \
    src/core/tb_mania_music.h \
    src/platform/tb_input.h

# ---- C++/Qt frontend ----
SOURCES += \
    src/qt/main.cpp \
    src/qt/Engine.cpp \
    src/qt/MainWindow.cpp \
    src/qt/VideoWidget.cpp \
    src/qt/Gallery.cpp \
    src/qt/Dialogs.cpp

HEADERS += \
    src/qt/Engine.h \
    src/qt/MainWindow.h \
    src/qt/VideoWidget.h \
    src/qt/Gallery.h \
    src/qt/Dialogs.h

# ---- convenience: `make appimage` builds a relocatable Linux AppImage ----
# (Does its own out-of-source release build + dependency bundling.)
appimage.target = appimage
appimage.commands = QMAKE=$$QMAKE_QMAKE $$PWD/packaging/make_appimage.sh
QMAKE_EXTRA_TARGETS += appimage
QMAKE_DISTCLEAN += -r $$PWD/build-appimage
