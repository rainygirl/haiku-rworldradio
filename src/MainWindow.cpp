#include "MainWindow.h"

#include <cctype>
#include <cstdio>
#include <exception>

#include <Application.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Message.h>
#include <MessageRunner.h>
#include <OS.h>
#include <ScrollView.h>
#include <StringItem.h>
#include <StringView.h>
#include <TextControl.h>

#include "LevelMeterView.h"
#include "StationCache.h"

namespace {

// Carries the full Station struct alongside the BListView row that
// represents it, so invoking a row can hand the station straight to
// RadioPlayer without a separate lookup table.
class StationItem : public BStringItem {
public:
	explicit StationItem(const Station& s)
		:
		BStringItem(s.name.c_str()),
		station(s)
	{
	}

	Station station;
};

struct LoadArgs {
	MainWindow* window;

	explicit LoadArgs(MainWindow* w) : window(w) {}
};

// e.g. "MP3 128kbps", "AAC", "128kbps", or "" if neither is known.
// radio-browser uses the literal string "UNKNOWN" for stations whose codec
// it couldn't determine - treat that the same as an empty/unknown codec
// rather than showing the placeholder text to the user.
std::string
FormatCodecBitrate(const Station& station)
{
	std::string text = station.codec == "UNKNOWN" ? "" : station.codec;
	if (station.bitrate > 0) {
		if (!text.empty())
			text += " ";
		char bitrate[16];
		snprintf(bitrate, sizeof(bitrate), "%d", station.bitrate);
		text += std::string(bitrate) + "kbps";
	}
	return text;
}

std::string
ToLower(const std::string& s)
{
	std::string out = s;
	for (size_t i = 0; i < out.size(); i++)
		out[i] = static_cast<char>(tolower(static_cast<unsigned char>(out[i])));
	return out;
}

// Case-insensitive substring match; an empty needle matches everything (no
// filter typed yet).
bool
MatchesFilter(const std::string& haystack, const std::string& lowerNeedle)
{
	return lowerNeedle.empty()
		|| ToLower(haystack).find(lowerNeedle) != std::string::npos;
}

} // namespace

MainWindow::MainWindow()
	:
	BWindow(BRect(80, 80, 780, 580), "R World Radio", B_TITLED_WINDOW,
		B_ASYNCHRONOUS_CONTROLS),
	fCountryListView(NULL),
	fStationListView(NULL),
	fCountryFilterView(NULL),
	fStationFilterView(NULL),
	fStatusView(NULL),
	fStopButton(NULL),
	fNowPlayingView(NULL),
	fFormatView(NULL),
	fLevelMeter(NULL),
	fLevelRunner(NULL),
	fPlayer(BMessenger(this))
{
	fCountryListView = new BListView("countries", B_SINGLE_SELECTION_LIST);
	fCountryListView->SetSelectionMessage(new BMessage(kMsgCountrySelected));
	BScrollView* countryScroll = new BScrollView("countryScroll",
		fCountryListView, 0, false, true);

	fStationListView = new BListView("stations", B_SINGLE_SELECTION_LIST);
	fStationListView->SetInvocationMessage(new BMessage(kMsgStationInvoked));
	BScrollView* stationScroll = new BScrollView("stationScroll",
		fStationListView, 0, false, true);

	// Live, case-insensitive substring filters over the two lists - fire on
	// every keystroke via the modification message (not the invocation
	// message, which would only fire on Enter). No label: the list right
	// below each field already makes clear what typing here narrows down.
	fCountryFilterView = new BTextControl("countryFilter", "", "", NULL);
	fCountryFilterView->SetModificationMessage(new BMessage(kMsgCountryFilterChanged));
	fStationFilterView = new BTextControl("stationFilter", "", "", NULL);
	fStationFilterView->SetModificationMessage(new BMessage(kMsgStationFilterChanged));

	fStopButton = new BButton("stop", "\xE2\x96\xA0" /* U+25A0 BLACK SQUARE */,
		new BMessage(kMsgStopPlayback));
	fNowPlayingView = new BStringView("nowPlaying", "Stopped");
	fFormatView = new BStringView("format", "");
	fLevelMeter = new LevelMeterView("level");
	fStatusView = new BStringView("status", "Loading stations...");

	BView* countryPanel = BLayoutBuilder::Group<>(B_VERTICAL, B_USE_SMALL_SPACING)
		.Add(fCountryFilterView)
		.Add(countryScroll)
		.View();
	BView* stationPanel = BLayoutBuilder::Group<>(B_VERTICAL, B_USE_SMALL_SPACING)
		.Add(fStationFilterView)
		.Add(stationScroll)
		.View();

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
			.Add(countryPanel, 1)
			.Add(stationPanel, 2)
			.SetInsets(B_USE_WINDOW_INSETS)
		.End()
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.Add(fStopButton)
			.Add(fNowPlayingView)
			.Add(fFormatView)
			.Add(fLevelMeter)
			.AddGlue()
			.Add(fStatusView)
			.SetInsets(B_USE_WINDOW_INSETS, 0, B_USE_WINDOW_INSETS,
				B_USE_WINDOW_INSETS)
		.End();

	fStopButton->Hide(); // only shown while actually kPlaying

	// Polls RadioPlayer::CurrentLevel() 10x/sec to drive the level meter -
	// simpler and cheaper than plumbing a message from the audio thread on
	// every single buffer callback.
	fLevelRunner = new BMessageRunner(BMessenger(this),
		new BMessage(kMsgLevelTick), 100000);

	StartLoad();
}

MainWindow::~MainWindow()
{
	delete fLevelRunner;
}

bool
MainWindow::QuitRequested()
{
	fPlayer.Stop();
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}

void
MainWindow::SetStatusText(const std::string& text)
{
	fStatusView->SetText(text.c_str());
}

void
MainWindow::StartLoad()
{
	SetStatusText("Loading stations...");
	LoadArgs* args = new LoadArgs(this);
	thread_id t = spawn_thread(&MainWindow::LoadThreadEntry, "station-load",
		B_NORMAL_PRIORITY, args);
	if (t < 0) {
		delete args;
		SetStatusText("Failed to start loader thread");
		return;
	}
	resume_thread(t);
}

status_t
MainWindow::LoadThreadEntry(void* cookie)
{
	LoadArgs* args = static_cast<LoadArgs*>(cookie);
	MainWindow* window = args->window;
	delete args;

	// JsonValue::Parse() throws std::runtime_error on malformed input - an
	// exception escaping a spawned thread's entry function is fatal (Haiku,
	// like any C++ program, calls std::terminate()/abort() for that), so a
	// single bad/truncated country file must not be allowed to crash the
	// whole app. Report it as a normal failed LoadResult instead.
	StationCache::LoadResult result;
	try {
		result = StationCache::Load();
	} catch (const std::exception& e) {
		result.ok = false;
		result.error = std::string("station data parse error: ") + e.what();
	} catch (...) {
		result.ok = false;
		result.error = "station data parse error: unknown exception";
	}

	BMessage msg(kMsgLoadDone);
	msg.AddPointer("result", new StationCache::LoadResult(result));
	BMessenger(window).SendMessage(&msg);
	return B_OK;
}

// Rebuilds the country list from fStationsByCountry, showing only entries
// matching fCountryFilterView's text (case-insensitive substring, live as
// the user types). Filtering only changes what's visible here - the
// authoritative selection (fSelectedCountryName) and the station panel are
// untouched even if the selected country's row is filtered out of view, the
// same as the other ports (see ui.rs) - only clicking a row changes it (see
// the kMsgCountrySelected handler in MessageReceived).
void
MainWindow::PopulateCountries()
{
	fCountryListView->MakeEmpty();

	std::string needle = ToLower(fCountryFilterView->Text());
	int32 selectIndex = -1;

	for (std::map<std::string, std::vector<Station> >::const_iterator it
			= fStationsByCountry.begin(); it != fStationsByCountry.end(); ++it) {
		const std::string& name = it->first;
		if (!MatchesFilter(name, needle))
			continue;
		fCountryListView->AddItem(new BStringItem(name.c_str()));
		if (name == fSelectedCountryName)
			selectIndex = fCountryListView->CountItems() - 1;
	}

	if (selectIndex >= 0) {
		fCountryListView->Select(selectIndex);
	} else if (fSelectedCountryName.empty() && fCountryListView->CountItems() > 0) {
		// Nothing selected yet (first population after a successful load) -
		// default to the first country, same as before filtering existed.
		fCountryListView->Select(0);
		BStringItem* item = static_cast<BStringItem*>(fCountryListView->ItemAt(0));
		if (item != NULL) {
			fSelectedCountryName = item->Text();
			PopulateStationsForSelectedCountry();
		}
	}
}

// Rebuilds the station list for fSelectedCountryName, showing only entries
// matching fStationFilterView's text (case-insensitive substring, live as
// the user types).
void
MainWindow::PopulateStationsForSelectedCountry()
{
	fStationListView->MakeEmpty();

	std::map<std::string, std::vector<Station> >::const_iterator it
		= fStationsByCountry.find(fSelectedCountryName);
	if (it == fStationsByCountry.end())
		return;

	std::string needle = ToLower(fStationFilterView->Text());
	for (size_t i = 0; i < it->second.size(); i++) {
		const Station& station = it->second[i];
		if (MatchesFilter(station.name, needle))
			fStationListView->AddItem(new StationItem(station));
	}
}

void
MainWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgCountrySelected:
		{
			int32 index = fCountryListView->CurrentSelection();
			if (index < 0)
				break;
			BStringItem* item
				= static_cast<BStringItem*>(fCountryListView->ItemAt(index));
			if (item == NULL)
				break;
			std::string name = item->Text();
			if (name != fSelectedCountryName) {
				fSelectedCountryName = name;
				fStationFilterView->SetText("");
				PopulateStationsForSelectedCountry();
			}
			break;
		}

		case kMsgCountryFilterChanged:
			PopulateCountries();
			break;

		case kMsgStationFilterChanged:
			PopulateStationsForSelectedCountry();
			break;

		case kMsgStationInvoked:
		{
			int32 index = fStationListView->CurrentSelection();
			if (index < 0)
				break;
			StationItem* item
				= static_cast<StationItem*>(fStationListView->ItemAt(index));
			if (item == NULL)
				break;
			fCurrentStation = item->station;
			fPlayer.Play(item->station);
			break;
		}

		case kMsgStopPlayback:
			fPlayer.Stop();
			break;

		case kMsgLoadDone:
		{
			StationCache::LoadResult* result = NULL;
			if (message->FindPointer("result", reinterpret_cast<void**>(&result))
					== B_OK && result != NULL) {
				if (result->ok) {
					fStationsByCountry = result->byCountry;
					PopulateCountries();
					char countText[16];
					snprintf(countText, sizeof(countText), "%lu",
						static_cast<unsigned long>(fStationsByCountry.size()));
					std::string status = "Loaded "
						+ std::string(countText) + " countries";
					if (!result->error.empty())
						status += " (" + result->error + ")";
					SetStatusText(status);
				} else {
					SetStatusText("Failed to load stations: " + result->error);
				}
				delete result;
			}
			break;
		}

		case kMsgLevelTick:
			fLevelMeter->SetLevel(fPlayer.CurrentLevel());
			break;

		case RadioPlayer::kStatusMessage:
		{
			int32 state = RadioPlayer::kStopped;
			message->FindInt32("state", &state);
			const char* stationName = "";
			message->FindString("station", &stationName);
			const char* detail = "";
			message->FindString("detail", &detail);

			switch (state) {
				case RadioPlayer::kConnecting:
					fNowPlayingView->SetText(
						(std::string("Connecting: ") + stationName).c_str());
					fFormatView->SetText("");
					if (!fStopButton->IsHidden())
						fStopButton->Hide();
					break;
				case RadioPlayer::kPlaying:
					fNowPlayingView->SetText("Now Playing");
					fFormatView->SetText(FormatCodecBitrate(fCurrentStation).c_str());
					if (fStopButton->IsHidden())
						fStopButton->Show();
					break;
				case RadioPlayer::kStopped:
					fNowPlayingView->SetText("Stopped");
					fFormatView->SetText("");
					if (!fStopButton->IsHidden())
						fStopButton->Hide();
					break;
				case RadioPlayer::kError:
					fNowPlayingView->SetText((std::string("Error (")
						+ stationName + "): " + detail).c_str());
					fFormatView->SetText("");
					if (!fStopButton->IsHidden())
						fStopButton->Hide();
					break;
			}
			break;
		}

		default:
			BWindow::MessageReceived(message);
	}
}
