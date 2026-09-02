#ifndef HAIKU_RADIO_HTTP_AUDIO_IO_H
#define HAIKU_RADIO_HTTP_AUDIO_IO_H

#include <AdapterIO.h>
#include <Locker.h>
#include <OS.h>
#include <SupportDefs.h>

#include <string>

namespace BPrivate { namespace Network { class BUrlRequest; } }

// Bridges a plain (non-HLS) HTTP/HTTPS audio stream into BMediaFile.
//
// BMediaFile(BUrl) hands the URL to Haiku's own http_streamer media add-on,
// which only speaks plain http:// - an https:// stream comes back with
// InitCheck() == B_MEDIA_NO_HANDLER (confirmed by hand against several
// live icecast/AIS stations: the same station's http:// mirror opens fine,
// the https:// one fails every time). The Network Kit itself has no such
// restriction (NetworkFetch already does https fine for the JSON API and
// TuneIn resolves), so this fetches the stream ourselves via BUrlRequest -
// same mechanism as NetworkFetch::Get(), but writing each chunk straight
// into a BAdapterIO instead of buffering the whole (unbounded, live) body -
// and hands BMediaFile a plain elementary stream to sniff, same as
// HlsAdapterIO does for HLS.
class HttpAudioIO : public BAdapterIO {
public:
	explicit HttpAudioIO(const std::string& url);
	~HttpAudioIO();

	void GetFlags(int32* flags) const;
	status_t Open();

	const std::string& InitError() const { return fInitError; }

private:
	static status_t WorkerThreadEntry(void* cookie);
	void RunWorker();
	void ReleaseInitOnce(bool success, const std::string& error = std::string());
	ssize_t WriteChunk(const void* buffer, size_t size);

	class Sink;
	friend class Sink;

	std::string fUrl;
	std::string fInitError;
	BInputAdapter* fInputAdapter;
	thread_id fWorkerThread;
	sem_id fInitSem;
	bool fInitReleased;
	bool fInitSucceeded;

	BLocker fRequestLock;
	BPrivate::Network::BUrlRequest* fRequest;

	int32 fStopRequested;
};

#endif
