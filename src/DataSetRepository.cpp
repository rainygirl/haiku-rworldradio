#include "DataSetRepository.h"

#include "JsonValue.h"

namespace DataSetRepository {

std::vector<CountryEntry>
ParseCountryIndex(const std::string& json)
{
	std::vector<CountryEntry> entries;
	JsonValue root = JsonValue::Parse(json);
	if (!root.IsArray())
		return entries;

	for (const JsonValue& item : root.arrayValue) {
		if (!item.IsObject())
			continue;
		CountryEntry entry;
		if (const JsonValue* v = item.Find("name"))
			entry.name = v->AsString();
		if (const JsonValue* v = item.Find("file"))
			entry.file = v->AsString();
		if (const JsonValue* v = item.Find("count"))
			entry.count = v->AsInt();
		if (entry.name.empty() || entry.file.empty())
			continue;
		entries.push_back(std::move(entry));
	}
	return entries;
}

std::vector<Station>
ParseCountryStations(const std::string& json, const std::string& countryName)
{
	std::vector<Station> stations;
	JsonValue root = JsonValue::Parse(json);
	if (!root.IsArray())
		return stations;

	stations.reserve(root.arrayValue.size());
	for (const JsonValue& item : root.arrayValue) {
		if (!item.IsObject())
			continue;

		Station station;
		station.country = countryName;
		if (const JsonValue* v = item.Find("name"))
			station.name = v->AsString();
		if (const JsonValue* v = item.Find("url"))
			station.url = v->AsString();
		if (const JsonValue* v = item.Find("codec"))
			station.codec = v->AsString();
		if (const JsonValue* v = item.Find("bitrate"))
			station.bitrate = v->AsInt();
		if (const JsonValue* v = item.Find("language"))
			station.language = v->AsString();
		if (const JsonValue* v = item.Find("needsResolve"))
			station.needsTuneInResolve = v->type == JsonValue::Type::Boolean && v->boolValue;

		const JsonValue* latValue = item.Find("lat");
		const JsonValue* lonValue = item.Find("lon");
		if (latValue != nullptr && lonValue != nullptr
				&& latValue->type == JsonValue::Type::Number
				&& lonValue->type == JsonValue::Type::Number) {
			station.lat = latValue->numberValue;
			station.lon = lonValue->numberValue;
			station.hasLocation = true;
		}

		if (station.name.empty() || station.url.empty())
			continue;
		stations.push_back(std::move(station));
	}
	return stations;
}

} // namespace DataSetRepository
