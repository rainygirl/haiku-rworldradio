#ifndef HAIKU_RADIO_HLS_ADAPTER_IO_H
#define HAIKU_RADIO_HLS_ADAPTER_IO_H

#include <AdapterIO.h>
#include <OS.h>
#include <SupportDefs.h>

#include <string>

// Bridges a live HLS audio stream (radio-browser's BBC-style .m3u8 entries)
// into BMediaFile: fetches/refreshes the playlist, downloads new segments,
// demuxes each via TsDemuxer, and writes the resulting elementary stream
// through BAdapterIO's BInputAdapter. This mirrors Haiku's own HTTPMediaIO
// (src/add-ons/media/plugins/http_streamer) - same pattern, just producing
// bytes via our own HLS+TS pipeline instead of a raw HTTP response, so
// BMediaFile's format sniffer sees a plain elementary ADTS AAC/MPEG-audio
// stream and picks a decoder the same way it already does for icecast MP3.
//
// Ownership note: BMediaFile(BDataIO*) does NOT delete its source (checked
// in Haiku's MediaFile.cpp - fDeleteSource is only set to true by the
// entry_ref/BUrl constructors, which build their own internal source and
// so own it; the raw-BDataIO* constructor leaves it false since the caller
// supplied that object). The caller must Open() this before handing it to
// BMediaFile (BMediaFile never calls Open() itself - only its own internal
// BUrl-based streamer setup does) and must delete it separately once done -
// see RadioPlayer::Session, which destroys its BMediaFile before this.
class HlsAdapterIO : public BAdapterIO {
public:
	explicit HlsAdapterIO(const std::string& playlistUrl);
	~HlsAdapterIO();

	void GetFlags(int32* flags) const;
	status_t Open();
	bool IsRunning() const;

private:
	static status_t WorkerThreadEntry(void* cookie);
	void RunWorker();
	void ReleaseInitOnce(bool success);

	std::string fPlaylistUrl;
	BInputAdapter* fInputAdapter;
	thread_id fWorkerThread;
	sem_id fInitSem;
	bool fInitReleased;
	bool fInitSucceeded;

	// Plain 0/1 flags read/written across the worker thread and the caller,
	// via Haiku's atomic_set()/atomic_get() (no std::atomic pre-C++11).
	int32 fStopRequested;
	mutable int32 fRunning;
};

#endif
