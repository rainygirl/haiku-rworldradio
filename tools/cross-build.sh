#!/bin/sh
# Cross-compile rworldradio for x86_64 or arm64 Haiku on a workstation with
# Docker, producing dist/<arch>/rworldradio without resources (the pkgman
# recipe attaches them on Haiku). x86_gcc2 is still built natively with make.
#
#   tools/cross-build.sh x86_64    # haiku/cross-compiler:x86_64-r1beta4 image
#   tools/cross-build.sh arm64     # container with a Haiku arm64 build tree
#
#   ARM64_CONTAINER   default haiku-builder
#   ARM64_GENERATED   default /root/renku-arm64-work/generated.arm64
#
# Both builds talk TLS directly (HAIKU_HAS_OPENSSL), so the package must
# require lib:libssl and lib:libcrypto.
set -e

ARCH="$1"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HERE/dist/$ARCH"
mkdir -p "$OUT"

# Same list as the makefile's SRCS. One line: it is expanded inside the
# container's sh -c script, where a newline would split the for loop.
SRCS="main App MainWindow LevelMeterView RadioPlayer StationCache NetworkFetch DataSetRepository JsonValue M3u8Parser TsDemuxer HlsAdapterIO HttpAudioIO HttpClient"
CXXFLAGS="-O2 -Wall -Wno-multichar -std=c++11 -D_GLIBCXX_USE_CXX11_ABI=1 -DHAIKU_HAS_OPENSSL"

case "$ARCH" in
x86_64)
	# The image has Haiku's private headers and static kits but no Haiku
	# OpenSSL, so the HaikuPorts openssl3 packages are fetched and extracted.
	SSL_URL=https://eu.hpkg.haiku-os.org/haikuports/master/x86_64/current/packages
	SSL_VERSION="${SSL_VERSION:-3.5.8-2}"
	( cd "$HERE" && tar cf - src ) \
		| docker run --rm -i --platform linux/amd64 haiku/cross-compiler:x86_64-r1beta4 sh -c "
			set -e
			mkdir -p /work/ssl && cd /work && tar xf -
			for p in openssl3 openssl3_devel; do
				wget -q -O \$p.hpkg $SSL_URL/\$p-$SSL_VERSION-x86_64.hpkg
				package extract -C /work/ssl \$p.hpkg
			done
			D=/system/develop
			for s in $SRCS; do
				x86_64-unknown-haiku-g++ -c src/\$s.cpp $CXXFLAGS -Isrc \
					-I\$D/headers/private/netservices \
					-I\$D/headers/private/media/experimental \
					-I\$D/headers/private/shared \
					-I/work/ssl/develop/headers -o \$s.o >&2
			done
			x86_64-unknown-haiku-g++ -o rworldradio *.o -Xlinker -soname=_APP_ \
				\$D/lib/libnetservices.a \$D/lib/libshared.a \
				-L/work/ssl/develop/lib \
				-lbe -ltracker -lnetwork -lbnetapi -lmedia -lssl -lcrypto >&2
			tar cf - rworldradio" \
		| tar xf - -C "$OUT"
	;;
arm64)
	# arm64 images have no media decoder add-ons, so MP3 (minimp3) and AAC
	# (FDK-AAC) are decoded in-process - see RWORLDRADIO_EMBEDDED_MP3.
	CONTAINER="${ARM64_CONTAINER:-haiku-builder}"
	GEN="${ARM64_GENERATED:-/root/renku-arm64-work/generated.arm64}"
	P="$GEN/objects/haiku/arm64/packaging/packages_build/minimum"
	WORK="/tmp/rworldradio-cross-$$"
	( cd "$HERE" && tar cf - src ) \
		| docker exec -i "$CONTAINER" sh -c "
			set -e
			mkdir -p $WORK/sysroot/boot/system $WORK/lib && cd $WORK && tar xf -
			ln -sfn $P/hpkg_-haiku_devel.hpkg/contents/develop $WORK/sysroot/boot/system/develop
			SYSLIBS=\$(ls -d $GEN/build_packages/gcc_syslibs-*-arm64/lib | head -1)
			ln -sfn \$SYSLIBS/libstdc++.so.6 $WORK/lib/libstdc++.so
			ln -sfn \$SYSLIBS/libgcc_s.so.1 $WORK/lib/libgcc_s.so
			SSL=\$(ls -d $GEN/build_packages/openssl3-*-arm64 | head -1)
			CXX=\"$GEN/cross-tools-arm64/bin/aarch64-unknown-haiku-g++ --sysroot=$WORK/sysroot\"
			D=$WORK/sysroot/boot/system/develop
			FDK=src/thirdparty/fdk-aac
			FDK_INC=
			for d in libAACdec libSBRdec libFDK libMpegTPDec libPCMutils libSYS libArithCoding libDRCdec libSACdec; do
				FDK_INC=\"\$FDK_INC -I\$FDK/\$d/include -I\$FDK/\$d/src\"
			done
			for f in \$FDK/*/src/*.cpp; do
				\$CXX -c \$f \$FDK_INC -O2 -w -std=c++11 -D_GLIBCXX_USE_CXX11_ABI=1 \
					-o fdk_\$(basename \$f .cpp).o >&2
			done
			for s in $SRCS Mp3StreamDecoder AacStreamDecoder StreamSniffer; do
				\$CXX -c src/\$s.cpp $CXXFLAGS -DHAIKU_BURL_HAS_BOOL_CTOR -DRWORLDRADIO_EMBEDDED_MP3 \
					-Isrc \$FDK_INC \
					-I\$D/headers/private/netservices \
					-I\$D/headers/private/media/experimental \
					-I\$D/headers/private/shared \
					-I\$SSL/develop/headers -o \$s.o >&2
			done
			\$CXX -o rworldradio *.o -Xlinker -soname=_APP_ \
				\$D/lib/libnetservices.a \$D/lib/libshared.a \
				-L$P/hpkg_-haiku.hpkg/contents/lib \
				-L$GEN/objects/haiku/arm64/release/kits/media \
				-L\$SSL/develop/lib -L$WORK/lib \
				-lbe -ltracker -lnetwork -lbnetapi -lmedia -lssl -lcrypto >&2
			tar cf - rworldradio
			rm -rf $WORK" \
		| tar xf - -C "$OUT"
	;;
*)
	echo "usage: $0 x86_64|arm64" >&2
	exit 1
	;;
esac

file "$OUT/rworldradio"
