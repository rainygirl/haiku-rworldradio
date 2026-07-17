#ifndef HAIKU_RADIO_M3U8_PARSER_H
#define HAIKU_RADIO_M3U8_PARSER_H

#include <string>
#include <vector>

// Minimal HLS playlist parser: enough to follow a master playlist to one
// media playlist, and to read a live media playlist's segment list. Not a
// general-purpose implementation - no byte-range segments, no encryption,
// no discontinuities.
namespace M3u8Parser {

struct Variant {
	std::string uri;
	int bandwidth = 0;
};

struct Segment {
	std::string uri;
	double duration = 0;
};

struct MediaPlaylist {
	std::vector<Segment> segments;
	long mediaSequence = 0;
	double targetDuration = 6;
	bool isEndList = false;
};

// Resolves a possibly-relative URI against a base URL. Handles the cases
// HLS playlists actually produce: absolute http(s) URLs pass through,
// host-absolute ("/path") and playlist-relative ("segment.ts") URIs
// resolve against baseUrl's origin/directory respectively.
std::string ResolveUrl(const std::string& baseUrl, const std::string& uri);

bool IsMasterPlaylist(const std::string& text);

// baseUrl is the playlist's own URL, used to resolve relative variant/
// segment URIs.
std::vector<Variant> ParseMasterPlaylist(const std::string& text,
	const std::string& baseUrl);
MediaPlaylist ParseMediaPlaylist(const std::string& text,
	const std::string& baseUrl);

} // namespace M3u8Parser

#endif
