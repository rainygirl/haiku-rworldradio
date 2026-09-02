#include "HttpAudioIO.h"

#include <Autolock.h>
#include <DataIO.h>
#include <HttpResult.h>
#include <Url.h>
#include <UrlProtocolRoster.h>
#include <UrlRequest.h>

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
	// Same ambiguous-overload situation as NetworkFetch.cpp and
	// RadioPlayer.cpp - see their matching comments.
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
