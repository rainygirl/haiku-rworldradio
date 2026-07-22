#ifndef HAIKU_RADIO_STATION_H
#define HAIKU_RADIO_STATION_H

#include <string>

struct Station {
	std::string uuid;
	std::string name;
	std::string url;
	std::string urlResolved;
	std::string country;
	std::string countryCode;
	std::string tags;
	std::string codec;
	int bitrate;
	std::string language;
	double lat;
	double lon;
	bool hasLocation;

	// True for stations sourced from TuneIn: PlaybackUrl() then returns a
	// Tune.ashx resolver link (a tiny text response containing the real
	// stream URL) rather than a directly playable stream, since TuneIn
	// doesn't hand out raw stream URLs in its browse listings.
	bool needsTuneInResolve;

	Station()
		:
		bitrate(0),
		lat(0),
		lon(0),
		hasLocation(false),
		needsTuneInResolve(false)
	{
	}

	// Prefer the pre-resolved stream URL (radio-browser follows playlist
	// redirects server-side) and fall back to the raw one.
	const std::string& PlaybackUrl() const
	{
		return urlResolved.empty() ? url : urlResolved;
	}
};

#endif
