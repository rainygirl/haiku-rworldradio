## Haiku Generic Build Makefile ##
NAME = rworldradio
TYPE = APP
APP_MIME_SIG = application/x-vnd.RWorldRadio-Radio

SRCS = \
	src/main.cpp \
	src/App.cpp \
	src/MainWindow.cpp \
	src/LevelMeterView.cpp \
	src/RadioPlayer.cpp \
	src/StationCache.cpp \
	src/NetworkFetch.cpp \
	src/DataSetRepository.cpp \
	src/JsonValue.cpp \
	src/M3u8Parser.cpp \
	src/TsDemuxer.cpp \
	src/HlsAdapterIO.cpp

# app_signature/app_version.short_info (so Deskbar shows "R World Radio"
# instead of the raw binary name) and the app icon - see src/app.rdef.
RDEFS = src/app.rdef
RSRCS =

# network: low-level socket API (BNetworkInterface etc, os/net headers) -
#          NOT where BUrlRequest/BHttpRequest live, kept in case anything
#          else needs it.
# libnetservices.a: BUrlRequest/BHttpRequest/BUrlProtocolRoster/... - the
#          classic Url Kit implementation, a static archive, not part of
#          any shared lib (matches how HaikuDepot links against it). Only
#          NetworkFetch/RadioPlayer use this now (resolving a TuneIn
#          station's Tune.ashx link at play time) - StationCache reads the
#          bundled data/ dataset and touches the network for nothing.
# media:   BMediaFile / BMediaTrack / BSoundPlayer (Media Kit)
# be:      BApplication / BWindow / interface kit, and app_info/be_app for
#          StationCache to locate the data/ directory next to the binary
# tracker: BEntry / BPath support used by StationCache
# stdc++/supc++: makefile-engine does NOT link these automatically for
# TYPE=APP (see its STDCPPLIBS comment) - without them, anything needing an
# out-of-line libstdc++ symbol (e.g. vector reallocation, __throw_length_error)
# fails at link time even though most std::string/vector usage is inlined.
LIBS = be tracker network bnetapi media stdc++ supc++ \
	/boot/system/develop/lib/libnetservices.a \
	/boot/system/develop/lib/libshared.a

LIBPATHS =
# BUrlRequest/BHttpRequest/BUrlProtocolListener live under the private
# "netservices" (classic, listener-callback based) headers on this Haiku
# version, not under os/net - that folder only has the low-level socket API.
# netservices2 is a newer, differently-shaped API and is NOT what this code
# uses.
# private/media/experimental has BAdapterIO/BMediaIO (HlsAdapterIO's base -
# same experimental Media Kit class Haiku's own http_streamer add-on uses).
# private/shared has RWLocker.h, which AdapterIO.h itself includes.
SYSTEM_INCLUDE_PATHS = /boot/system/develop/headers/private/netservices \
	/boot/system/develop/headers/private/media/experimental \
	/boot/system/develop/headers/private/shared
LOCAL_INCLUDE_PATHS =
OPTIMIZE := FULL
LOCALES =
DEFINES =
WARNINGS = ALL
SYMBOLS =
DEBUGGER =
# Force the modern (post-C++11, non-COW) libstdc++ std::string ABI: some
# vector<pair<string,...>> reallocation paths were otherwise emitting old
# std::string::_Rep-based helper calls that this Haiku gcc13 toolchain's
# libstdc++ doesn't provide, causing "undefined reference" at link time.
COMPILER_FLAGS = -std=c++11 -D_GLIBCXX_USE_CXX11_ABI=1
LINKER_FLAGS =
APP_VERSION =
DRIVER_PATH =

## Include the Makefile-Engine that ships with the Haiku SDK.
include /boot/system/develop/etc/makefile-engine
