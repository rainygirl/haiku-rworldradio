#ifndef HAIKU_RADIO_DATA_SET_REPOSITORY_H
#define HAIKU_RADIO_DATA_SET_REPOSITORY_H

#include <string>
#include <vector>

#include "Station.h"

// Parses the pre-built dataset produced by tools/update_stations_db.py:
// data/countries.json (an index) plus one data/countries/<file>.json per
// country. Pure JSON parsing, no filesystem/network access, so it's
// testable the same way JsonValue is.
namespace DataSetRepository {

struct CountryEntry {
	std::string name;
	std::string file;
	int count;

	CountryEntry() : count(0) {}
};

// Parses data/countries.json.
std::vector<CountryEntry> ParseCountryIndex(const std::string& json);

// Parses one data/countries/<file>.json (a flat array of station records).
std::vector<Station> ParseCountryStations(const std::string& json,
	const std::string& countryName);

} // namespace DataSetRepository

#endif
