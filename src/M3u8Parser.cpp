#include "M3u8Parser.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace {

bool
StartsWith(const std::string& s, const char* prefix)
{
	size_t len = strlen(prefix);
	return s.compare(0, len, prefix) == 0;
}

std::vector<std::string>
SplitLines(const std::string& text)
{
	std::vector<std::string> lines;
	size_t start = 0;
	while (start <= text.size()) {
		size_t newline = text.find('\n', start);
		std::string line = (newline == std::string::npos)
			? text.substr(start) : text.substr(start, newline - start);
		while (!line.empty() && (line[line.size() - 1] == '\r'
				|| isspace(static_cast<unsigned char>(line[line.size() - 1]))))
			line.resize(line.size() - 1);
		lines.push_back(line);
		if (newline == std::string::npos)
			break;
		start = newline + 1;
	}
	return lines;
}

int
ExtractIntAttribute(const std::string& line, const std::string& key)
{
	std::string afterColon = ":" + key + "=";
	std::string afterComma = "," + key + "=";
	size_t pos = line.find(afterColon);
	size_t offset = afterColon.size();
	if (pos == std::string::npos) {
		pos = line.find(afterComma);
		offset = afterComma.size();
	}
	if (pos == std::string::npos)
		return 0;
	return atoi(line.c_str() + pos + offset);
}

} // namespace

namespace M3u8Parser {

std::string
ResolveUrl(const std::string& baseUrl, const std::string& uri)
{
	if (uri.find("://") != std::string::npos)
		return uri;

	size_t schemeEnd = baseUrl.find("://");
	if (schemeEnd == std::string::npos)
		return uri; // base isn't a URL we can resolve against; best effort

	size_t hostStart = schemeEnd + 3;
	size_t pathStart = baseUrl.find('/', hostStart);
	std::string origin = (pathStart == std::string::npos)
		? baseUrl : baseUrl.substr(0, pathStart);

	if (!uri.empty() && uri[0] == '/')
		return origin + uri;

	if (pathStart == std::string::npos)
		return origin + "/" + uri;

	size_t lastSlash = baseUrl.find_last_of('/');
	std::string baseDir = baseUrl.substr(0, lastSlash + 1);
	return baseDir + uri;
}

bool
IsMasterPlaylist(const std::string& text)
{
	return text.find("#EXT-X-STREAM-INF") != std::string::npos;
}

std::vector<Variant>
ParseMasterPlaylist(const std::string& text, const std::string& baseUrl)
{
	std::vector<Variant> variants;
	std::vector<std::string> lines = SplitLines(text);

	for (size_t i = 0; i < lines.size(); i++) {
		if (!StartsWith(lines[i], "#EXT-X-STREAM-INF:"))
			continue;

		int bandwidth = ExtractIntAttribute(lines[i], "BANDWIDTH");
		size_t j = i + 1;
		while (j < lines.size() && (lines[j].empty() || lines[j][0] == '#'))
			j++;
		if (j >= lines.size())
			break;

		Variant variant;
		variant.bandwidth = bandwidth;
		variant.uri = ResolveUrl(baseUrl, lines[j]);
		variants.push_back(variant);
		i = j;
	}
	return variants;
}

MediaPlaylist
ParseMediaPlaylist(const std::string& text, const std::string& baseUrl)
{
	MediaPlaylist playlist;
	std::vector<std::string> lines = SplitLines(text);
	double nextDuration = 0;

	for (size_t i = 0; i < lines.size(); i++) {
		const std::string& line = lines[i];
		if (StartsWith(line, "#EXT-X-MEDIA-SEQUENCE:")) {
			playlist.mediaSequence = atol(line.c_str() + strlen("#EXT-X-MEDIA-SEQUENCE:"));
		} else if (StartsWith(line, "#EXT-X-TARGETDURATION:")) {
			playlist.targetDuration = atof(line.c_str() + strlen("#EXT-X-TARGETDURATION:"));
		} else if (StartsWith(line, "#EXT-X-ENDLIST")) {
			playlist.isEndList = true;
		} else if (StartsWith(line, "#EXTINF:")) {
			nextDuration = atof(line.c_str() + strlen("#EXTINF:"));
		} else if (!line.empty() && line[0] != '#') {
			Segment segment;
			segment.uri = ResolveUrl(baseUrl, line);
			segment.duration = nextDuration;
			playlist.segments.push_back(segment);
			nextDuration = 0;
		}
	}
	return playlist;
}

} // namespace M3u8Parser
