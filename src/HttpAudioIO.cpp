#include "HttpAudioIO.h"

#include <Autolock.h>
#include <DataIO.h>
#include <HttpResult.h>
#include <Url.h>
#include <UrlProtocolRoster.h>
#include <UrlRequest.h>

#ifdef HAIKU_HAS_OPENSSL
#include <NetworkAddress.h>
#include <Socket.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

// See the matching comment in NetworkFetch.cpp: these classes live in
// BPrivate::Network on Haiku builds where the classic Url Kit was moved to
// a private header.
using namespace BPrivate::Network;

namespace {
// Same rationale as HlsAdapterIO's timeout - how long Open() waits for the
// first bytes before giving up.
const bigtime_t kHttpAudioTimeout = 20000000;
}

class HttpAudioIO::Sink : public BDataIO {
public:
	explicit Sink(HttpAudioIO* owner) : fOwner(owner) {}

	ssize_t Write(const void* buffer, size_t size)
	{
		return fOwner->WriteChunk(buffer, size);
	}

	ssize_t Read(void*, size_t) { return B_NOT_ALLOWED; }

private:
	HttpAudioIO* fOwner;
};

HttpAudioIO::HttpAudioIO(const std::string& url)
	:
	BAdapterIO(B_MEDIA_STREAMING | B_MEDIA_SEEKABLE, kHttpAudioTimeout),
	fUrl(url),
	fInputAdapter(NULL),
	fWorkerThread(-1),
	fInitSem(create_sem(0, "http-audio-init")),
	fInitReleased(false),
	fInitSucceeded(false),
	fRequestLock("http-audio-request"),
	fRequest(NULL),
	fSocket(NULL),
	fSsl(NULL),
	fStopRequested(0)
{
}

HttpAudioIO::~HttpAudioIO()
{
	atomic_set(&fStopRequested, 1);
	{
		BAutolock lock(fRequestLock);
		if (fRequest != NULL)
			fRequest->Stop();
#ifdef HAIKU_HAS_OPENSSL
		// Closing the socket out from under the worker thread is what
		// actually unblocks a pending SSL_read() - OpenSSL has no separate
		// cancellation call, and there's no clean way to interrupt just
		// the read without tearing down the connection it's reading from.
		if (fSocket != NULL)
			fSocket->Disconnect();
#endif
	}
	if (fWorkerThread >= 0) {
		status_t exitValue;
		wait_for_thread(fWorkerThread, &exitValue);
	}
	delete_sem(fInitSem);
}

void
HttpAudioIO::GetFlags(int32* flags) const
{
	*flags = B_MEDIA_STREAMING | B_MEDIA_SEEK_BACKWARD;
}

status_t
HttpAudioIO::Open()
{
	fInputAdapter = BuildInputAdapter();

	fWorkerThread = spawn_thread(&HttpAudioIO::WorkerThreadEntry,
		"http-audio-worker", B_NORMAL_PRIORITY, this);
	if (fWorkerThread < 0)
		return fWorkerThread;
	resume_thread(fWorkerThread);

	status_t err = acquire_sem_etc(fInitSem, 1, B_RELATIVE_TIMEOUT, kHttpAudioTimeout);
	if (err != B_OK)
		return err;
	if (!fInitSucceeded)
		return B_ERROR;

	return BAdapterIO::Open();
}

status_t
HttpAudioIO::WorkerThreadEntry(void* cookie)
{
	static_cast<HttpAudioIO*>(cookie)->RunWorker();
	return B_OK;
}

void
HttpAudioIO::ReleaseInitOnce(bool success, const std::string& error)
{
	if (fInitReleased)
		return;
	fInitReleased = true;
	fInitSucceeded = success;
	if (!success)
		fInitError = error;
	release_sem(fInitSem);
}

ssize_t
HttpAudioIO::WriteChunk(const void* buffer, size_t size)
{
	if (atomic_get(&fStopRequested) != 0)
		return B_INTERRUPTED;
	ssize_t written = fInputAdapter->Write(buffer, size);
	if (written > 0)
		ReleaseInitOnce(true);
	return written;
}

void
HttpAudioIO::RunWorker()
{
#ifdef HAIKU_HAS_OPENSSL
	RunWorkerOpenSsl();
#else
	RunWorkerUrlRequest();
#endif
}

#ifdef HAIKU_HAS_OPENSSL

namespace {

struct ParsedHttpsUrl {
	std::string host;
	int port;
	std::string path;
};

// Deliberately not BUrl::Host()/Port()/Path() - this only ever sees a
// scheme we already checked is "https://" (see IsHttpsUrl in
// RadioPlayer.cpp), so plain substring slicing covers every station URL
// in the dataset without depending on exactly which BUrl accessors this
// SDK happens to expose.
ParsedHttpsUrl ParseHttpsUrl(const std::string& url)
{
	ParsedHttpsUrl result;
	result.port = 443;
	result.path = "/";

	std::string rest = url.substr(8); // strlen("https://")
	size_t slash = rest.find('/');
	std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
	if (slash != std::string::npos)
		result.path = rest.substr(slash);

	size_t colon = authority.find(':');
	if (colon == std::string::npos) {
		result.host = authority;
	} else {
		result.host = authority.substr(0, colon);
		result.port = atoi(authority.substr(colon + 1).c_str());
	}
	return result;
}

} // namespace

void
HttpAudioIO::RunWorkerOpenSsl()
{
	ParsedHttpsUrl parsed = ParseHttpsUrl(fUrl);
	if (parsed.host.empty()) {
		ReleaseInitOnce(false, "invalid stream URL");
		return;
	}

	BNetworkAddress address;
	status_t err = address.SetTo(parsed.host.c_str(), parsed.port);
	if (err != B_OK) {
		ReleaseInitOnce(false, "could not resolve host: " + std::string(strerror(err)));
		return;
	}

	BSocket* socket = new BSocket();
	err = socket->Connect(address, kHttpAudioTimeout);
	if (err != B_OK) {
		delete socket;
		ReleaseInitOnce(false, "could not connect: " + std::string(strerror(err)));
		return;
	}

	SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
	if (ctx == NULL) {
		delete socket;
		ReleaseInitOnce(false, "SSL context init failed");
		return;
	}
	SSL_CTX_set_default_verify_paths(ctx);

	SSL* ssl = SSL_new(ctx);
	SSL_set_fd(ssl, socket->Socket());
	SSL_set_tlsext_host_name(ssl, parsed.host.c_str());

	bool stopped = false;
	{
		BAutolock lock(fRequestLock);
		if (atomic_get(&fStopRequested) != 0)
			stopped = true;
		else {
			fSocket = socket;
			fSsl = ssl;
		}
	}
	if (stopped) {
		SSL_free(ssl);
		SSL_CTX_free(ctx);
		delete socket;
		ReleaseInitOnce(false, "stopped");
		return;
	}

	int connectResult = SSL_connect(ssl);
	if (connectResult != 1) {
		char detail[64];
		snprintf(detail, sizeof(detail), "TLS handshake failed (SSL error %d)",
			SSL_get_error(ssl, connectResult));
		ReleaseInitOnce(false, detail);
		goto cleanup;
	}

	{
		std::string request = "GET " + parsed.path + " HTTP/1.1\r\n"
			"Host: " + parsed.host + "\r\n"
			"User-Agent: rworldradio\r\n"
			"Accept: */*\r\n"
			"Connection: close\r\n"
			"\r\n";
		if (SSL_write(ssl, request.data(), (int)request.size()) <= 0) {
			ReleaseInitOnce(false, "could not send request");
			goto cleanup;
		}
	}

	{
		// Read until the header block ends, then everything from here on
		// (including whatever arrived past the blank line in the same
		// read) is raw audio - these AIS/Icecast stations hold the
		// connection open and push bytes forever, no Content-Length or
		// chunked encoding on the audio itself.
		std::string headerBuf;
		char chunk[4096];
		for (;;) {
			int n = SSL_read(ssl, chunk, sizeof(chunk));
			if (n <= 0) {
				ReleaseInitOnce(false, "connection closed before headers completed");
				goto cleanup;
			}
			headerBuf.append(chunk, n);
			size_t end = headerBuf.find("\r\n\r\n");
			if (end == std::string::npos) {
				if (headerBuf.size() > 16384) {
					ReleaseInitOnce(false, "response headers too large");
					goto cleanup;
				}
				continue;
			}
			size_t statusEnd = headerBuf.find("\r\n");
			std::string statusLine = headerBuf.substr(0, statusEnd);
			if (statusLine.find(" 200") == std::string::npos) {
				ReleaseInitOnce(false, "HTTP error: " + statusLine);
				goto cleanup;
			}
			std::string leftover = headerBuf.substr(end + 4);
			if (!leftover.empty())
				WriteChunk(leftover.data(), leftover.size());
			break;
		}

		while (atomic_get(&fStopRequested) == 0) {
			int n = SSL_read(ssl, chunk, sizeof(chunk));
			if (n <= 0)
				break;
			WriteChunk(chunk, n);
		}
	}

cleanup:
	{
		BAutolock lock(fRequestLock);
		fSsl = NULL;
		fSocket = NULL;
	}
	SSL_free(ssl);
	SSL_CTX_free(ctx);
	delete socket;
}

#else // !HAIKU_HAS_OPENSSL

void
HttpAudioIO::RunWorkerUrlRequest()
{
	// Some Haiku SDKs (the legacy x86/gcc2 secondary arch) declare BOTH
	// BUrl(const char*, bool = true) and BUrl(const char*), making a
	// single-argument call ambiguous - others (the primary x86_64/gcc13
	// SDK) only have the single-argument form, where a second bool
	// argument is a hard error. HAIKU_BURL_HAS_BOOL_CTOR is set by the
	// Makefile per-SDK - see its comment.
#ifdef HAIKU_BURL_HAS_BOOL_CTOR
	BUrl parsedUrl(fUrl.c_str(), true);
#else
	BUrl parsedUrl(fUrl.c_str());
#endif
	if (!parsedUrl.IsValid()) {
		ReleaseInitOnce(false, "invalid stream URL");
		return;
	}

	Sink sink(this);
	BUrlRequest* request = BUrlProtocolRoster::MakeRequest(parsedUrl, &sink);
	if (request == NULL) {
		ReleaseInitOnce(false, "failed to create request");
		return;
	}

	{
		BAutolock lock(fRequestLock);
		fRequest = request;
	}

	thread_id requestThread = request->Run();
	if (requestThread < 0) {
		ReleaseInitOnce(false, "failed to start request");
		BAutolock lock(fRequestLock);
		fRequest = NULL;
		delete request;
		return;
	}

	status_t exitValue = B_OK;
	wait_for_thread(requestThread, &exitValue);

	if (!fInitSucceeded) {
		const BHttpResult& httpResult
			= static_cast<const BHttpResult&>(request->Result());
		char detail[64];
		snprintf(detail, sizeof(detail), "HTTP status %ld", (long)httpResult.StatusCode());
		ReleaseInitOnce(false, detail);
	}

	{
		BAutolock lock(fRequestLock);
		fRequest = NULL;
	}
	delete request;
}

#endif // HAIKU_HAS_OPENSSL
