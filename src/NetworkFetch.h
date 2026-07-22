#ifndef HAIKU_RADIO_NETWORK_FETCH_H
#define HAIKU_RADIO_NETWORK_FETCH_H

#include <string>

namespace NetworkFetch {

struct Result {
	bool ok;
	int httpStatus;
	std::string body;
	std::string error;

	Result() : ok(false), httpStatus(0) {}
};

// Blocking GET. Spawns the request on Haiku's Network Kit thread and joins
// it via wait_for_thread(), so this must not be called from the UI thread.
Result Get(const std::string& url);

} // namespace NetworkFetch

#endif
