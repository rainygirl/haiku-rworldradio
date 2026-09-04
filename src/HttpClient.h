#ifndef HAIKU_RADIO_HTTP_CLIENT_H
#define HAIKU_RADIO_HTTP_CLIENT_H

#include <string>

// Small in-process HTTP/HTTPS helpers shared by HttpAudioIO (which streams an
// endless audio body) and NetworkFetch (which fetches a finite resource -
// playlist, JSON). Both need the same URL parsing and redirect handling, and
// on builds where Haiku's Network Kit has no TLS (the arm64 image), both must
// speak https themselves over OpenSSL rather than through BUrlRequest.
namespace HttpClient {

struct ParsedUrl {
	bool tls;			// https -> true (TLS), http -> false (plain socket)
	std::string host;
	int port;
	std::string path;
	bool valid;
};

// Parses http:// and https:// URLs by plain slicing (no dependency on which
// BUrl accessors a given SDK exposes). valid is false for any other scheme.
ParsedUrl ParseUrl(const std::string& url);

// Resolves a Location header against the URL it came from: absolute URLs pass
// through, an absolute path reuses the base scheme+host, a bare reference is
// taken relative to the base directory.
std::string ResolveRedirect(const std::string& base, const std::string& location);

// Case-insensitive lookup of one header value in a raw header block (the bytes
// before the blank line). Empty if absent. nameLower must be lowercase.
std::string ExtractHeader(const std::string& headers, const char* nameLower);

#ifdef HAIKU_HAS_OPENSSL
// Blocking GET of a complete resource over http or https, following up to a
// few redirects, honouring Content-Length / chunked / read-until-close body
// framing. Returns true on a final 2xx with a non-empty body; on false,
// outError describes why. Only available where OpenSSL is (https needs it).
bool GetBody(const std::string& url, std::string& outBody, int& outStatus,
	std::string& outError);
#endif

} // namespace HttpClient

#endif // HAIKU_RADIO_HTTP_CLIENT_H
