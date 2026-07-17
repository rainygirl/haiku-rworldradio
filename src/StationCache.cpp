#include "StationCache.h"

#include <Application.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Roster.h>

#include "DataSetRepository.h"

namespace {

// The app is normally run as objects.<arch>-<compiler>-release/<Binary>
// from the project root, so "data" sits one level up from the executable's
// own directory; a plain "data" (relative to the current working directory)
// covers running it via `./Binary` from that same project root, and
// "<execDir>/data" covers a layout where data/ is shipped right next to
// the binary. A properly "installed" copy (binary in
// ~/config/non-packaged/apps/ so it shows up in Deskbar) keeps data/
// *out* of that directory instead - Deskbar's Applications menu lists
// that directory's contents directly, so a "data" subfolder sitting next
// to the binary there would show up as a spurious extra menu entry. Haiku
// has an established convention for exactly this split
// (non-packaged/apps for the binary, non-packaged/data/<AppName> for its
// data), so that's checked too via B_USER_NONPACKAGED_DATA_DIRECTORY.
std::vector<std::string>
CandidateDataDirs()
{
	std::vector<std::string> candidates;

	app_info info;
	if (be_app != nullptr && be_app->GetAppInfo(&info) == B_OK) {
		BEntry entry(&info.ref);
		BPath execPath;
		if (entry.GetPath(&execPath) == B_OK) {
			BPath execDir;
			execPath.GetParent(&execDir);
			candidates.push_back(std::string(execDir.Path()) + "/data");

			BPath projectDir;
			execDir.GetParent(&projectDir);
			candidates.push_back(std::string(projectDir.Path()) + "/data");
		}
	}

	BPath nonPackagedData;
	if (find_directory(B_USER_NONPACKAGED_DATA_DIRECTORY, &nonPackagedData) == B_OK)
		candidates.push_back(std::string(nonPackagedData.Path()) + "/RWorldRadio");

	candidates.push_back("data");
	return candidates;
}

} // namespace

std::string
StationCache::FindDataDir()
{
	for (const std::string& candidate : CandidateDataDirs()) {
		BEntry probe((candidate + "/countries.json").c_str());
		if (probe.Exists())
			return candidate;
	}
	return std::string();
}

bool
StationCache::ReadFile(const std::string& path, std::string& outContent)
{
	BFile file(path.c_str(), B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return false;

	off_t size = 0;
	file.GetSize(&size);
	if (size <= 0)
		return false;

	outContent.resize(static_cast<size_t>(size));
	ssize_t read = file.Read(&outContent[0], static_cast<size_t>(size));
	return read == size;
}

StationCache::LoadResult
StationCache::Load()
{
	LoadResult result;

	std::string dataDir = FindDataDir();
	if (dataDir.empty()) {
		result.error = "data/countries.json not found next to the app";
		return result;
	}

	std::string indexJson;
	if (!ReadFile(dataDir + "/countries.json", indexJson)) {
		result.error = "could not read " + dataDir + "/countries.json";
		return result;
	}

	std::vector<DataSetRepository::CountryEntry> countries
		= DataSetRepository::ParseCountryIndex(indexJson);
	if (countries.empty()) {
		result.error = "countries.json parsed but is empty";
		return result;
	}

	for (const auto& entry : countries) {
		std::string stationsJson;
		if (!ReadFile(dataDir + "/countries/" + entry.file, stationsJson))
			continue; // skip a missing/unreadable country file, keep going
		std::vector<Station> stations
			= DataSetRepository::ParseCountryStations(stationsJson, entry.name);
		if (!stations.empty())
			result.byCountry[entry.name] = std::move(stations);
	}

	result.ok = !result.byCountry.empty();
	if (!result.ok)
		result.error = "no country files could be read";
	return result;
}
