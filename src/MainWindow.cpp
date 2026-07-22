#include "MainWindow.h"

#include <cstdio>

#include <Application.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <Message.h>
#include <MessageRunner.h>
#include <OS.h>
#include <ScrollView.h>
#include <StringItem.h>
#include <StringView.h>

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
std::string
FormatCodecBitrate(const Station& station)
{
	std::string text = station.codec;
	if (station.bitrate > 0) {
		if (!text.empty())
			text += " ";
		char bitrate[16];
		snprintf(bitrate, sizeof(bitrate), "%d", station.bitrate);
		text += std::string(bitrate) + "kbps";
	}
	return text;
}

} // namespace

MainWindow::MainWindow()
	:
	BWindow(BRect(80, 80, 780, 580), "R World Radio", B_TITLED_WINDOW,
		B_ASYNCHRONOUS_CONTROLS),
	fCountryListView(NULL),
	fStationListView(NULL),
	fStatusView(NULL),
	fStopButton(NULL),
	fNowPlayingView(NULL),
	fFormatView(NULL),
	fLevelMeter(NULL),
	fLevelRunner(NULL),
	fPlayer(BMessenger(this))
{
	BMenuBar* menuBar = new BMenuBar("menubar");
	BMenu* fileMenu = new BMenu("File");
	fileMenu->AddItem(new BMenuItem("Quit", new BMessage(B_QUIT_REQUESTED), 'Q'));
	menuBar->AddItem(fileMenu);

	fCountryListView = new BListView("countries", B_SINGLE_SELECTION_LIST);
	fCountryListView->SetSelectionMessage(new BMessage(kMsgCountrySelected));
	BScrollView* countryScroll = new BScrollView("countryScroll",
		fCountryListView, 0, false, true);

	fStationListView = new BListView("stations", B_SINGLE_SELECTION_LIST);
	fStationListView->SetInvocationMessage(new BMessage(kMsgStationInvoked));
	BScrollView* stationScroll = new BScrollView("stationScroll",
		fStationListView, 0, false, true);

	fStopButton = new BButton("stop", "\xE2\x96\xA0" /* U+25A0 BLACK SQUARE */,
		new BMessage(kMsgStopPlayback));
	fNowPlayingView = new BStringView("nowPlaying", "Stopped");
	fFormatView = new BStringView("format", "");
	fLevelMeter = new LevelMeterView("level");
	fStatusView = new BStringView("status", "Loading stations...");

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(menuBar)
		.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
			.Add(countryScroll, 1)
			.Add(stationScroll, 2)
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

	StationCache::LoadResult result = StationCache::Load();

	BMessage msg(kMsgLoadDone);
	msg.AddPointer("result", new StationCache::LoadResult(result));
	BMessenger(window).SendMessage(&msg);
	return B_OK;
}

void
MainWindow::PopulateCountries()
{
	fCountryListView->MakeEmpty();
	fStationListView->MakeEmpty();
	for (std::map<std::string, std::vector<Station> >::const_iterator it
			= fStationsByCountry.begin(); it != fStationsByCountry.end(); ++it)
		fCountryListView->AddItem(new BStringItem(it->first.c_str()));

	if (fCountryListView->CountItems() > 0) {
		fCountryListView->Select(0);
		PopulateStationsForSelectedCountry();
	}
}

void
MainWindow::PopulateStationsForSelectedCountry()
{
	fStationListView->MakeEmpty();

	int32 index = fCountryListView->CurrentSelection();
	if (index < 0)
		return;
	BStringItem* countryItem
		= static_cast<BStringItem*>(fCountryListView->ItemAt(index));
	if (countryItem == NULL)
		return;

	std::map<std::string, std::vector<Station> >::const_iterator it
		= fStationsByCountry.find(countryItem->Text());
	if (it == fStationsByCountry.end())
		return;

	for (size_t i = 0; i < it->second.size(); i++)
		fStationListView->AddItem(new StationItem(it->second[i]));
}

void
MainWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgCountrySelected:
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
