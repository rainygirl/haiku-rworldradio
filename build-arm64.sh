#!/bin/sh
# Native build of rworldradio on Haiku arm64 (RENKU), where there is no
# /boot/system/develop/etc/makefile-engine (minimum profile) - just raw g++
# calls against this system's own gcc 13.3.0, headers and libs. Run this
# from inside the guest, from the project directory (e.g.
# /boot/home/rworldradio).
set -e

OBJDIR=objects.arm64-release
mkdir -p "$OBJDIR"

SRCS="main App MainWindow LevelMeterView RadioPlayer StationCache NetworkFetch DataSetRepository JsonValue M3u8Parser TsDemuxer HlsAdapterIO HttpAudioIO"

INC="-Isrc -I/boot/system/develop/headers/private/netservices -I/boot/system/develop/headers/private/media/experimental -I/boot/system/develop/headers/private/shared"

# This SDK's Url.h declares both BUrl(const char*) and BUrl(const char*,
# bool = true), which makes a single-argument BUrl(str) call ambiguous -
# confirmed the same issue as the legacy x86/gcc2 SDK (see the matching
# comment/#ifdef in src/RadioPlayer.cpp and src/NetworkFetch.cpp).
DEFS="-DHAIKU_BURL_HAS_BOOL_CTOR"

for s in $SRCS; do
	echo "cc $s"
	g++ -c "src/$s.cpp" $INC $DEFS -O2 -Wall -Wno-multichar -Wno-ctor-dtor-privacy \
		-std=c++11 -D_GLIBCXX_USE_CXX11_ABI=1 -o "$OBJDIR/$s.o"
done

echo "link"
g++ -o "$OBJDIR/rworldradio" "$OBJDIR"/*.o \
	/boot/system/develop/lib/libnetservices.a \
	/boot/system/develop/lib/libshared.a \
	-lbe -ltracker -lnetwork -lbnetapi -lmedia -lstdc++ -lsupc++

echo "resources"
rc -o "$OBJDIR/app.rsrc" src/app.rdef
xres -o "$OBJDIR/rworldradio" "$OBJDIR/app.rsrc"
mimeset -f "$OBJDIR/rworldradio"

echo "done:"
ls -la "$OBJDIR/rworldradio"
