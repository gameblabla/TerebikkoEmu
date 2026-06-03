# Top-level build entry points for TerebikkoEmu.
#
# Linux Qt build:
#   make linux
#
# Windows cross builds from Linux:
#   make win64      # builds FFmpeg + SDL3 (from source) for x86_64, then app
#   make win32      # builds bundled FFmpeg for i686-w64-mingw32, then static app
#   make nvcodec    # installs bundled nv-codec-headers for FFmpeg/NVDEC
#   make sdl-win64  # imports SDL3 master and cross-builds it statically

QMAKE ?= qmake6
JOBS ?= $(shell nproc 2>/dev/null || printf 4)

all: linux

linux:
	@mkdir -p build/linux-qt
	cd build/linux-qt && $(QMAKE) ../../TerebikkoEmu.pro
	$(MAKE) -C build/linux-qt -j$(JOBS)

nvcodec:
	$(MAKE) -f src/Makefile.ffmpeg nvcodec

ffmpeg-win64:
	$(MAKE) -f src/Makefile.ffmpeg win64

ffmpeg-win32:
	$(MAKE) -f src/Makefile.ffmpeg win32

ffmpeg-windows:
	$(MAKE) -f src/Makefile.ffmpeg all

# SDL3 is imported from the upstream master branch and cross-built statically.
sdl-win64:
	$(MAKE) -f src/Makefile.sdl win64

sdl-win32:
	$(MAKE) -f src/Makefile.sdl win32

sdl-update:
	$(MAKE) -f src/Makefile.sdl update

win64:
	$(MAKE) -f src/Makefile.win64

win32:
	$(MAKE) -f src/Makefile.win32

windows: win64 win32

clean:
	rm -rf build/linux-qt build/win32 build/win64 terebikko-win32.exe terebikko-win64.exe

clean-ffmpeg:
	$(MAKE) -f src/Makefile.ffmpeg clean

clean-sdl:
	$(MAKE) -f src/Makefile.sdl clean

.PHONY: all linux nvcodec ffmpeg-win64 ffmpeg-win32 ffmpeg-windows \
	sdl-win64 sdl-win32 sdl-update win64 win32 windows \
	clean clean-ffmpeg clean-sdl
