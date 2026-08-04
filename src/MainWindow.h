#ifndef HAIKU_RADIO_MAIN_WINDOW_H
#define HAIKU_RADIO_MAIN_WINDOW_H

#include <Window.h>

#include <map>
#include <string>
#include <vector>

#include "RadioPlayer.h"
#include "Station.h"

class BButton;
class BListView;
class BMessageRunner;
class BStringView;
class BTextControl;
class LevelMeterView;

class MainWindow : public BWindow {
public:
	MainWindow();
	~MainWindow();

	void MessageReceived(BMessage* message);
	bool QuitRequested();

private:
	static const uint32 kMsgCountrySelected = 'ctSl';
	static const uint32 kMsgStationInvoked = 'stIv';
	static const uint32 kMsgLoadDone = 'ldDn';
	static const uint32 kMsgLevelTick = 'lvlT';
	static const uint32 kMsgStopPlayback = 'stpP';
	static const uint32 kMsgCountryFilterChanged = 'ctFl';
	static const uint32 kMsgStationFilterChanged = 'stFl';

	void StartLoad();
	static status_t LoadThreadEntry(void* cookie);
	void PopulateCountries();
	void PopulateStationsForSelectedCountry();
	void SetStatusText(const std::string& text);

	BListView* fCountryListView;
	BListView* fStationListView;
	BTextControl* fCountryFilterView;
	BTextControl* fStationFilterView;
	BStringView* fStatusView;
	BButton* fStopButton;
	BStringView* fNowPlayingView;
	BStringView* fFormatView;
	LevelMeterView* fLevelMeter;
	BMessageRunner* fLevelRunner;

	// The station the user last invoked, kept only so the format/bitrate
	// label can be filled in from our own dataset once RadioPlayer confirms
	// kPlaying - the stream's actual negotiated media_format doesn't expose
	// a friendly codec name/bitrate the way the dataset record already does.
	Station fCurrentStation;

	// Authoritative selection, independent of which rows the country filter
	// currently shows/hides - mirrors the other ports (see ui.rs), where
	// filtering the country list narrows what's visible without touching
	// which country is actually selected, even if its row is filtered out.
	std::string fSelectedCountryName;

	std::map<std::string, std::vector<Station> > fStationsByCountry;
	RadioPlayer fPlayer;
};

#endif
