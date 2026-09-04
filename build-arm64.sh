#!/bin/sh
# Native build of rworldradio on Haiku arm64 (RENKU), where there is no
# /boot/system/develop/etc/makefile-engine (minimum profile) - just raw g++
# calls against this system's own gcc 13.3.0, headers and libs. Run this
# from inside the guest, from the project directory (e.g.
# /boot/home/rworldradio).
set -e

OBJDIR=objects.arm64-release
mkdir -p "$OBJDIR"

SRCS="main App MainWindow LevelMeterView RadioPlayer StationCache NetworkFetch DataSetRepository JsonValue M3u8Parser TsDemuxer HlsAdapterIO HttpAudioIO HttpClient Mp3StreamDecoder AacStreamDecoder StreamSniffer"

INC="-Isrc -I/boot/system/develop/headers/private/netservices -I/boot/system/develop/headers/private/media/experimental -I/boot/system/develop/headers/private/shared"

# FDK-AAC (vendored, decode subset only) needs each of its libraries' include
# and src directories on the path - it includes headers by bare name across
# library boundaries rather than by relative path.
FDK=src/thirdparty/fdk-aac
FDK_LIBS="libAACdec libSBRdec libFDK libMpegTPDec libPCMutils libSYS libArithCoding libDRCdec libSACdec"
FDK_INC=""
for d in $FDK_LIBS; do FDK_INC="$FDK_INC -I$FDK/$d/include -I$FDK/$d/src"; done

# This SDK's Url.h declares both BUrl(const char*) and BUrl(const char*,
# bool = true), which makes a single-argument BUrl(str) call ambiguous -
# confirmed the same issue as the legacy x86/gcc2 SDK (see the matching
# comment/#ifdef in src/RadioPlayer.cpp and src/NetworkFetch.cpp).
DEFS="-DHAIKU_BURL_HAS_BOOL_CTOR"

# This image's /boot/system/add-ons/media/plugins is empty - no reader/decoder
# add-ons at all - so BMediaFile returns B_MEDIA_NO_HANDLER for every stream.
# Decode MP3 in-process instead (see Mp3StreamDecoder / RadioPlayer's
# RWORLDRADIO_EMBEDDED_MP3 path). Harmless on an image that does have plugins;
# only arm64's build turns it on because that's where they're missing.
DEFS="$DEFS -DRWORLDRADIO_EMBEDDED_MP3"

# HttpAudioIO needs to talk TLS directly - see its header comment. The
# arm64 bootstrap SDK has neither the openssl devel package nor the
# openssl runtime package yet (as of this writing, confirmed: no
# libssl.so anywhere in the image), so detect both rather than assume
# either: headers alone would build fine and then fail to *load* for
# want of libssl.so.3 at runtime. Without both, HttpAudioIO.cpp falls
# back to its BUrlRequest path, which still builds but won't actually
# get https streams playing until openssl lands on this SDK.
OPENSSL_LIBS=""
if [ -f /boot/system/develop/headers/openssl/ssl.h ] && ls /boot/system/lib/libssl.so* >/dev/null 2>&1; then
	DEFS="$DEFS -DHAIKU_HAS_OPENSSL"
	OPENSSL_LIBS="-lssl -lcrypto"
fi

# FDK-AAC is ~100 files of third-party C++; build it once into its own object
# directory and skip files already built, so an incremental app rebuild
# doesn't pay for it again. Its own warnings are silenced - it is vendored
# unmodified and we don't want them drowning ours.
FDKOBJ="$OBJDIR/fdk"
mkdir -p "$FDKOBJ"
fdk_built=0
for f in $FDK/*/src/*.cpp; do
	o="$FDKOBJ/$(echo "$f" | tr '/' '_' | sed 's/\.cpp$/.o/')"
	if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
		g++ -c "$f" $FDK_INC -O2 -w -std=c++11 -D_GLIBCXX_USE_CXX11_ABI=1 -o "$o"
		fdk_built=$((fdk_built + 1))
	fi
done
echo "fdk-aac: $fdk_built file(s) compiled"

for s in $SRCS; do
	echo "cc $s"
	g++ -c "src/$s.cpp" $INC $FDK_INC $DEFS -O2 -Wall -Wno-multichar -Wno-ctor-dtor-privacy \
		-std=c++11 -D_GLIBCXX_USE_CXX11_ABI=1 -o "$OBJDIR/$s.o"
done

echo "link"
g++ -o "$OBJDIR/rworldradio" "$OBJDIR"/*.o "$FDKOBJ"/*.o \
	/boot/system/develop/lib/libnetservices.a \
	/boot/system/develop/lib/libshared.a \
	-lbe -ltracker -lnetwork -lbnetapi -lmedia -lstdc++ -lsupc++ $OPENSSL_LIBS

echo "resources"
rc -o "$OBJDIR/app.rsrc" src/app.rdef
xres -o "$OBJDIR/rworldradio" "$OBJDIR/app.rsrc"
mimeset -f "$OBJDIR/rworldradio"

echo "done:"
ls -la "$OBJDIR/rworldradio"
