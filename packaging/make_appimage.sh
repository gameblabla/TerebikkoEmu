#!/usr/bin/env bash
#
# make_appimage.sh - build a self-contained Linux AppImage of TerebikkoEmu (Qt6).
#
# It does an out-of-source release build, stages an AppDir, then uses linuxdeploy +
# its Qt plugin to bundle Qt6, SDL3 and the ffmpeg/libav* libraries and emit a single
# relocatable TerebikkoEmu-<arch>.AppImage.
#
# Usage:
#   packaging/make_appimage.sh            # build + package
#   QMAKE=/path/to/qmake6 packaging/make_appimage.sh
#   VERSION=1.0 packaging/make_appimage.sh
#   DEBUGTOOLS=1 packaging/make_appimage.sh   # bundle the debug-jump build
#
# Requirements: qmake6, a C/C++ toolchain, Qt6 + SDL3 + ffmpeg dev packages, and
# network access on first run (to fetch linuxdeploy; cached afterwards under tools/).
set -euo pipefail

# ----------------------------------------------------------------------------------
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
ARCH="$(uname -m)"
VERSION="${VERSION:-$(date +%Y.%m.%d)}"
BUILD="$ROOT/build-appimage"
APPDIR="$BUILD/AppDir"
TOOLS="$ROOT/tools/appimage"
APP=TerebikkoEmu

# AppImages-of-tools sometimes can't mount FUSE (containers/CI); extract instead.
export APPIMAGE_EXTRACT_AND_RUN=1
export NO_STRIP="${NO_STRIP:-}"            # set NO_STRIP=1 to keep symbols

say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# ----------------------------------------------------------------------------------
# 1. Locate qmake6
# ----------------------------------------------------------------------------------
find_qmake() {
    if [ -n "${QMAKE:-}" ] && command -v "$QMAKE" >/dev/null 2>&1; then echo "$QMAKE"; return; fi
    for q in qmake6 /usr/lib/qt6/bin/qmake6 /usr/lib64/qt6/bin/qmake6 /usr/lib64/qt6/bin/qmake qmake; do
        if command -v "$q" >/dev/null 2>&1; then
            ver="$("$q" -query QT_VERSION 2>/dev/null || true)"
            case "$ver" in 6.*) echo "$q"; return;; esac
        fi
    done
    die "no Qt6 qmake found (set QMAKE=/path/to/qmake6)"
}
QMAKE="$(find_qmake)"
QT_BINS="$("$QMAKE" -query QT_INSTALL_BINS)"
say "Qt $("$QMAKE" -query QT_VERSION) via $QMAKE"

# ----------------------------------------------------------------------------------
# 2. Out-of-source release build
# ----------------------------------------------------------------------------------
say "Building $APP (out-of-source: $BUILD)"
rm -rf "$BUILD"; mkdir -p "$BUILD"
QMAKE_CFG=()
[ -n "${DEBUGTOOLS:-}" ] && QMAKE_CFG+=("CONFIG+=debugtools")
( cd "$BUILD" && "$QMAKE" "$ROOT/$APP.pro" "${QMAKE_CFG[@]}" && make -j"$(nproc)" )
[ -x "$BUILD/$APP" ] || die "build did not produce $BUILD/$APP"

# ----------------------------------------------------------------------------------
# 3. Stage the AppDir
# ----------------------------------------------------------------------------------
say "Staging AppDir"
rm -rf "$APPDIR"
install -Dm755 "$BUILD/$APP"                     "$APPDIR/usr/bin/$APP"
install -Dm644 "$HERE/terebikkoemu.desktop"      "$APPDIR/usr/share/applications/terebikkoemu.desktop"
install -Dm644 "$HERE/terebikkoemu.png"          "$APPDIR/usr/share/icons/hicolor/256x256/apps/terebikkoemu.png"
# linuxdeploy also wants the icon + desktop at the AppDir root; it places them itself,
# but stage a top-level icon so the .desktop's Icon= resolves during validation.
cp "$HERE/terebikkoemu.png" "$APPDIR/terebikkoemu.png"

# ----------------------------------------------------------------------------------
# 4. Fetch linuxdeploy + Qt plugin (cached under tools/appimage)
# ----------------------------------------------------------------------------------
mkdir -p "$TOOLS"
fetch() {  # fetch <url> <dest>
    local url="$1" dest="$2"
    [ -x "$dest" ] && return 0
    say "Downloading $(basename "$dest")"
    if command -v curl >/dev/null 2>&1; then curl -fL# "$url" -o "$dest"
    elif command -v wget >/dev/null 2>&1; then wget -q --show-progress "$url" -O "$dest"
    else die "need curl or wget to download $url"; fi
    chmod +x "$dest"
}
BASE=https://github.com/linuxdeploy
fetch "$BASE/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage"            "$TOOLS/linuxdeploy-$ARCH.AppImage"
fetch "$BASE/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage" "$TOOLS/linuxdeploy-plugin-qt-$ARCH.AppImage"

# ----------------------------------------------------------------------------------
# 5. Bundle dependencies + build the AppImage
# ----------------------------------------------------------------------------------
say "Bundling Qt6 / SDL3 / ffmpeg and packing the AppImage"
export PATH="$TOOLS:$PATH"           # so the qt plugin is discoverable
export QMAKE                         # linuxdeploy-plugin-qt uses this to find Qt6
export OUTPUT="$APP-$VERSION-$ARCH.AppImage"
export VERSION                       # embedded into the AppImage filename/metadata
export QML_SOURCES_PATHS="$ROOT/src"

"$TOOLS/linuxdeploy-$ARCH.AppImage" \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/$APP" \
    --desktop-file "$APPDIR/usr/share/applications/terebikkoemu.desktop" \
    --icon-file "$HERE/terebikkoemu.png" \
    --plugin qt \
    --output appimage

[ -f "$ROOT/$OUTPUT" ] || mv -f "$OUTPUT" "$ROOT/$OUTPUT"
chmod +x "$ROOT/$OUTPUT"
say "Done: $ROOT/$OUTPUT  ($(du -h "$ROOT/$OUTPUT" | cut -f1))"
