// Standalone test for DataSetRepository, run against the real generated
// dataset so it doubles as a sanity check on tools/update_stations_db.py's
// output shape. Build with any C++11 compiler:
//   c++ -std=c++11 test_dataset_repository.cpp \
//       ../src/JsonValue.cpp ../src/DataSetRepository.cpp -o /tmp/test_dataset
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>

#include "../src/DataSetRepository.h"

static std::string
ReadFile(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

int main()
{
	std::string indexJson = ReadFile("../data/countries.json");
	assert(!indexJson.empty() && "run from test/, and generate data/ first");

	auto countries = DataSetRepository::ParseCountryIndex(indexJson);
	assert(!countries.empty());
	std::cout << countries.size() << " countries in index" << std::endl;

	bool foundGermany = false;
	for (const auto& entry : countries) {
		if (entry.name != "Germany")
			continue;
		foundGermany = true;
		std::string stationsJson = ReadFile("../data/countries/" + entry.file);
		auto stations = DataSetRepository::ParseCountryStations(stationsJson, entry.name);
		assert(static_cast<int>(stations.size()) == entry.count);
		assert(!stations.empty());

		bool sawSourceMarker = false;
		for (const auto& station : stations) {
			assert(!station.name.empty());
			assert(!station.url.empty());
			assert(station.country == "Germany");
			if (station.needsTuneInResolve || !station.language.empty()
					|| station.hasLocation)
				sawSourceMarker = true;
		}
		assert(sawSourceMarker);
		std::cout << "Germany: " << stations.size() << " stations parsed" << std::endl;
	}
	assert(foundGermany);

	std::cout << "OK" << std::endl;
	return 0;
}
