#include "NetworkFetch.h"

#include <cstdio>

#include <DataIO.h>
#include <Errors.h>
#include <HttpResult.h>
#include <OS.h>
#include <Url.h>
#include <UrlProtocolListener.h>
#include <UrlProtocolRoster.h>
#include <UrlRequest.h>

// See the matching comment in RadioPlayer.cpp: these classes live in
// BPrivate::Network on Haiku builds where the classic Url Kit was moved to
// a private header.
using namespace BPrivate::Network;

namespace {

// BUrlRequest writes the response body directly into whatever BDataIO is
// passed as its `output` - there is no per-chunk listener callback on this
// API - so this just accumulates it in memory.
class BufferSink : public BDataIO {
public:
	std::string body;

	ssize_t Write(const void* buffer, size_t size)
	{
		body.append(static_cast<const char*>(buffer), size);
		return static_cast<ssize_t>(size);
	}

	ssize_t Read(void*, size_t)
	{
		return B_NOT_ALLOWED;
	}
};

} // namespace

namespace NetworkFetch {

Result Get(const std::string& url)
{
	Result result;

	// Explicit second argument: this Haiku SDK's Url.h declares both
	// BUrl(const char*, bool = true) and a legacy BUrl(const char*)
	// overload, which is ambiguous with a single argument.
	BUrl parsedUrl(url.c_str(), true);
	if (!parsedUrl.IsValid()) {
		result.error = "invalid URL";
		return result;
	}

	BufferSink sink;
	// BHttpRequest's constructor is private - BUrlProtocolRoster::MakeRequest
	// is the only way to create a request, returned as the base BUrlRequest.
	BUrlRequest* request = BUrlProtocolRoster::MakeRequest(parsedUrl, &sink);
	if (request == NULL) {
		result.error = "failed to create request";
		return result;
	}

	thread_id requestThread = request->Run();
	if (requestThread < 0) {
		result.error = "failed to start request";
		delete request;
		return result;
	}

	status_t exitValue = B_OK;
	wait_for_thread(requestThread, &exitValue);

	const BHttpResult& httpResult
		= static_cast<const BHttpResult&>(request->Result());
	result.httpStatus = httpResult.StatusCode();
	result.body = sink.body;
	result.ok = (result.httpStatus >= 200 && result.httpStatus < 300)
		&& !result.body.empty();

	if (!result.ok && result.error.empty()) {
		char status[16];
		snprintf(status, sizeof(status), "%d", result.httpStatus);
		result.error = "HTTP status " + std::string(status);
	}

	delete request;
	return result;
}

} // namespace NetworkFetch
