#ifndef HAIKU_RADIO_STATION_CACHE_H
#define HAIKU_RADIO_STATION_CACHE_H

#include <map>
#include <string>
#include <vector>

#include "Station.h"

// Loads the station list from the pre-built dataset (data/countries.json +
// data/countries/<file>.json, produced by tools/update_stations_db.py) that
// ships alongside the app - no network access, no TTL, nothing to refresh.
// Meant to be called from a worker thread since it does file I/O, though in
// practice reading these local files is fast.
class StationCache {
public:
	struct LoadResult {
		bool ok = false;
		std::string error;
		std::map<std::string, std::vector<Station>> byCountry;
	};

	static LoadResult Load();

private:
	static std::string FindDataDir();
	static bool ReadFile(const std::string& path, std::string& outContent);
};

#endif
