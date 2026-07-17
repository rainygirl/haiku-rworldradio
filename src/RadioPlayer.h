#ifndef HAIKU_RADIO_RADIO_PLAYER_H
#define HAIKU_RADIO_RADIO_PLAYER_H

#include <MediaDefs.h>
#include <Messenger.h>
#include <SupportDefs.h>

#include <memory>
#include <mutex>
#include <string>

#include "Station.h"

// Streams and decodes one internet radio station at a time entirely
// in-process: NetworkStreamIO buffers bytes from a BHttpRequest, BMediaFile
// picks a decoder for them, and BSoundPlayer pulls decoded frames for
// playback. All setup and teardown happens off the caller's thread; status
// changes are reported asynchronously via BMessage so this is safe to drive
// from a BWindow's MessageReceived().
class RadioPlayer {
public:
	static const uint32 kStatusMessage = 'RPst';

	enum State {
		kConnecting,
		kPlaying,
		kStopped,
		kError
	};

	// statusTarget receives kStatusMessage BMessages with:
	//   int32  "state"   - one of the State values above
	//   string "station" - station name (empty when kStopped)
	//   string "detail"  - human-readable error detail (kError only)
	explicit RadioPlayer(const BMessenger& statusTarget);
	~RadioPlayer();

	// Stops whatever is currently playing (if anything) and starts
	// connecting to the given station. Returns immediately.
	void Play(const Station& station);

	// Stops playback. Safe to call when nothing is playing.
	void Stop();

	// Peak amplitude (0.0-1.0) of the most recently played audio buffer, or
	// 0 when nothing is playing. Cheap and thread-safe to poll from the UI
	// thread for a simple level meter - it's just an atomic read.
	float CurrentLevel() const;

	// Opaque; defined in RadioPlayer.cpp. Public only so the free-standing
	// SetupArgs/TeardownArgs helper structs in that .cpp file (not members
	// of RadioPlayer, so private access wouldn't extend to them) can name
	// std::shared_ptr<Session>. The definition itself stays out of this
	// header either way.
	struct Session;

private:
	bool IsCurrent(uint64 generation);
	void EmitStatus(State state, const std::string& stationName,
		const std::string& detail);
	void DetachTeardown(std::shared_ptr<Session> session);

	static status_t SetupThreadEntry(void* cookie);
	static status_t TeardownThreadEntry(void* cookie);
	void RunSetup(std::shared_ptr<Session> session, Station station,
		uint64 generation);
	static void PlayBufferProc(void* cookie, void* buffer, size_t size,
		const media_raw_audio_format& format);

	BMessenger fStatusTarget;
	mutable std::mutex fMutex;
	std::shared_ptr<Session> fSession;
	uint64 fGeneration;
};

#endif
