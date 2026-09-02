#ifndef HAIKU_RADIO_HTTP_AUDIO_IO_H
#define HAIKU_RADIO_HTTP_AUDIO_IO_H

#include <AdapterIO.h>
#include <Locker.h>
#include <OS.h>
#include <SupportDefs.h>

#include <string>

namespace BPrivate { namespace Network { class BUrlRequest; } }
class BSocket;
typedef struct ssl_st SSL;

// Bridges a plain (non-HLS) HTTP/HTTPS audio stream into BMediaFile.
//
// BMediaFile(BUrl) hands the URL to Haiku's own http_streamer media add-on,
// which only speaks plain http:// - an https:// stream comes back with
// InitCheck() == B_MEDIA_NO_HANDLER (confirmed by hand against several
// live icecast/AIS stations: the same station's http:// mirror opens fine,
// the https:// one fails every time).
//
// Where HAIKU_HAS_OPENSSL is set (Makefile detects the openssl devel
// headers at build time - see its comment), https:// goes over a
// hand-rolled TLS/HTTP client built on BSocket + OpenSSL directly, because
// on some Haiku builds even the Network Kit's own BUrlRequest can't do
// https at all: confirmed by hand that its SSL_connect()-equivalent path
// returns B_UNSUPPORTED for every https host tried, on a build whose
// Network Services Kit (libnetservices.a) has zero SSL symbols linked in -
// while curl and openssl(1) on that same machine, and this class's own
// direct OpenSSL calls, complete a real TLS 1.3 handshake against the
// exact same stations. So this doesn't route through BUrlRequest at all -
// it does its own DNS/connect via BSocket, wraps the fd with OpenSSL,
// writes a minimal HTTP/1.1 GET by hand, and streams whatever comes back
// after the header block straight into the BAdapterIO (these AIS/Icecast
// stations hold the connection open and push raw audio bytes forever,
// no Content-Length or chunked encoding to worry about).
//
// Where HAIKU_HAS_OPENSSL is NOT set (e.g. the arm64 bootstrap SDK, which
// has no openssl devel package at all yet), this falls back to the
// BUrlRequest-based implementation - same mechanism NetworkFetch uses,
// writing each chunk straight into a BAdapterIO instead of buffering the
// whole (unbounded, live) body. That fallback still hits the same
// B_UNSUPPORTED wall on a build without kit-level TLS support, but it's
// the best available without OpenSSL headers to build against, and costs
// nothing on a build where the kit's TLS does work.
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
#ifdef HAIKU_HAS_OPENSSL
	void RunWorkerOpenSsl();
#else
	void RunWorkerUrlRequest();
#endif
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

	// Guards whichever of these the current backend uses, so the
	// destructor can force a blocked worker thread to wake up: closing
	// the request/socket out from under it makes its blocking read fail
	// instead of hanging until the timeout.
	BLocker fRequestLock;
	BPrivate::Network::BUrlRequest* fRequest;
	BSocket* fSocket;
	SSL* fSsl;

	int32 fStopRequested;
};

#endif
