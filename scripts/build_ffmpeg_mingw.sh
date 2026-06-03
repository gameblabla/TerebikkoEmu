#!/usr/bin/env sh
set -eu
arch="${1:-}"
case "$arch" in
  nvcodec|ffnvcodec) exec make -f src/Makefile.ffmpeg nvcodec ;;
  win64|x86_64) exec make -f src/Makefile.ffmpeg win64 ;;
  win32|i686|x86) exec make -f src/Makefile.ffmpeg win32 ;;
  both|all) exec make -f src/Makefile.ffmpeg all ;;
  *) echo "usage: $0 {nvcodec|win64|win32|both}" >&2; exit 2 ;;
esac
