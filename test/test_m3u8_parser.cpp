// Standalone test for M3u8Parser using real playlists captured from BBC
// Radio 1's live HLS feed. Build with any C++11 compiler:
//   c++ -std=c++11 test_m3u8_parser.cpp ../src/M3u8Parser.cpp -o /tmp/test_m3u8
#include <cassert>
#include <iostream>

#include "../src/M3u8Parser.h"

static const char* kMasterUrl =
	"http://as-hls-ww-live.akamaized.net/pool_01505109/live/ww/"
	"bbc_radio_one/bbc_radio_one.isml/bbc_radio_one.m3u8";

static const char* kMasterPlaylist =
"#EXTM3U\n"
"#EXT-X-VERSION:3\n"
"## Created with Unified Streaming Platform  (version=1.13.5-30103)\n"
"\n"
"# variants\n"
"#EXT-X-STREAM-INF:BANDWIDTH=56000,AVERAGE-BANDWIDTH=51000,CODECS=\"mp4a.40.5\"\n"
"bbc_radio_one-audio=48000.m3u8\n"
"#EXT-X-STREAM-INF:BANDWIDTH=112000,AVERAGE-BANDWIDTH=102000,CODECS=\"mp4a.40.5\"\n"
"bbc_radio_one-audio=96000.m3u8\n"
"#EXT-X-STREAM-INF:BANDWIDTH=150000,AVERAGE-BANDWIDTH=136000,CODECS=\"mp4a.40.2\"\n"
"bbc_radio_one-audio=128000.m3u8\n"
"#EXT-X-STREAM-INF:BANDWIDTH=374000,AVERAGE-BANDWIDTH=340000,CODECS=\"mp4a.40.2\"\n"
"bbc_radio_one-audio=320000.m3u8\n";

static const char* kMediaUrl =
	"http://as-hls-ww-live.akamaized.net/pool_01505109/live/ww/"
	"bbc_radio_one/bbc_radio_one.isml/bbc_radio_one-audio=320000.m3u8";

static const char* kMediaPlaylist =
"#EXTM3U\n"
"#EXT-X-VERSION:3\n"
"## Created with Unified Streaming Platform  (version=1.13.5-30103)\n"
"#EXT-X-MEDIA-SEQUENCE:278793294\n"
"#EXT-X-INDEPENDENT-SEGMENTS\n"
"#EXT-X-TARGETDURATION:6\n"
"#USP-X-TIMESTAMP-MAP:MPEGTS=4700405152,LOCAL=2026-07-17T08:31:15.200000Z\n"
"#EXT-X-PROGRAM-DATE-TIME:2026-07-17T08:31:15.200000Z\n"
"#EXTINF:6.4, no desc\n"
"bbc_radio_one-audio=320000-278793294.ts\n"
"#EXTINF:6.4, no desc\n"
"bbc_radio_one-audio=320000-278793295.ts\n"
"#EXTINF:6.4, no desc\n"
"bbc_radio_one-audio=320000-278793296.ts\n";

int main()
{
	assert(M3u8Parser::IsMasterPlaylist(kMasterPlaylist));
	assert(!M3u8Parser::IsMasterPlaylist(kMediaPlaylist));

	auto variants = M3u8Parser::ParseMasterPlaylist(kMasterPlaylist, kMasterUrl);
	assert(variants.size() == 4);
	assert(variants[3].bandwidth == 374000);
	assert(variants[3].uri ==
		"http://as-hls-ww-live.akamaized.net/pool_01505109/live/ww/"
		"bbc_radio_one/bbc_radio_one.isml/bbc_radio_one-audio=320000.m3u8");

	auto playlist = M3u8Parser::ParseMediaPlaylist(kMediaPlaylist, kMediaUrl);
	assert(playlist.mediaSequence == 278793294);
	assert(playlist.targetDuration == 6);
	assert(!playlist.isEndList);
	assert(playlist.segments.size() == 3);
	assert(playlist.segments[0].duration == 6.4);
	assert(playlist.segments[0].uri ==
		"http://as-hls-ww-live.akamaized.net/pool_01505109/live/ww/"
		"bbc_radio_one/bbc_radio_one.isml/bbc_radio_one-audio=320000-278793294.ts");
	assert(playlist.segments[2].uri ==
		"http://as-hls-ww-live.akamaized.net/pool_01505109/live/ww/"
		"bbc_radio_one/bbc_radio_one.isml/bbc_radio_one-audio=320000-278793296.ts");

	// Relative resolution against a host-root base, and a host-absolute URI.
	assert(M3u8Parser::ResolveUrl("http://host.example/a/b/c.m3u8", "seg.ts")
		== "http://host.example/a/b/seg.ts");
	assert(M3u8Parser::ResolveUrl("http://host.example/a/b/c.m3u8", "/x/seg.ts")
		== "http://host.example/x/seg.ts");
	assert(M3u8Parser::ResolveUrl("http://host.example/a/b/c.m3u8",
		"http://other.example/seg.ts") == "http://other.example/seg.ts");

	std::cout << "OK: " << variants.size() << " variants, "
		<< playlist.segments.size() << " segments" << std::endl;
	return 0;
}
