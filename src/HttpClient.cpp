#include "HttpClient.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#ifdef HAIKU_HAS_OPENSSL
#include <NetworkAddress.h>
#include <Socket.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace HttpClient {

ParsedUrl
ParseUrl(const std::string& url)
{
	ParsedUrl r;
	r.valid = false;
	r.path = "/";

	// strncmp, not std::string::compare(pos, n, const char*): the latter's
	// overload behaves differently under the legacy gcc2 secondary arch and
	// silently rejects otherwise-valid URLs there.
	std::string rest;
	if (strncmp(url.c_str(), "https://", 8) == 0) {
		r.tls = true;
		r.port = 443;
		rest = url.substr(8);
	} else if (strncmp(url.c_str(), "http://", 7) == 0) {
		r.tls = false;
		r.port = 80;
		rest = url.substr(7);
	} else {
		return r; // unsupported scheme
	}

	size_t slash = rest.find('/');
	std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
	if (slash != std::string::npos)
		r.path = rest.substr(slash);

	size_t colon = authority.find(':');
	if (colon == std::string::npos) {
		r.host = authority;
	} else {
		r.host = authority.substr(0, colon);
		r.port = atoi(authority.substr(colon + 1).c_str());
	}
	if (r.host.empty())
		return r;
	r.valid = true;
	return r;
}

std::string
ResolveRedirect(const std::string& base, const std::string& location)
{
	if (strncmp(location.c_str(), "http://", 7) == 0
		|| strncmp(location.c_str(), "https://", 8) == 0) {
		return location;
	}
	size_t schemeEnd = base.find("://");
	if (schemeEnd == std::string::npos)
		return location;
	size_t pathStart = base.find('/', schemeEnd + 3);
	std::string schemeAuthority = (pathStart == std::string::npos)
		? base : base.substr(0, pathStart);
	if (!location.empty() && location[0] == '/')
		return schemeAuthority + location;
	std::string baseDir = (pathStart == std::string::npos)
		? schemeAuthority + "/" : base.substr(0, base.rfind('/') + 1);
	return baseDir + location;
}

std::string
ExtractHeader(const std::string& headers, const char* nameLower)
{
	size_t nameLen = strlen(nameLower);
	size_t pos = 0;
	while (pos < headers.size()) {
		size_t lineEnd = headers.find("\r\n", pos);
		if (lineEnd == std::string::npos)
			lineEnd = headers.size();
		size_t colon = headers.find(':', pos);
		if (colon != std::string::npos && colon < lineEnd
			&& colon - pos == nameLen) {
			bool match = true;
			for (size_t i = 0; i < nameLen; i++) {
				if (tolower((unsigned char)headers[pos + i])
						!= (unsigned char)nameLower[i]) {
					match = false;
					break;
				}
			}
			if (match) {
				size_t v = colon + 1;
				while (v < lineEnd && (headers[v] == ' ' || headers[v] == '\t'))
					v++;
				return headers.substr(v, lineEnd - v);
			}
		}
		if (lineEnd == headers.size())
			break;
		pos = lineEnd + 2;
	}
	return std::string();
}

#ifdef HAIKU_HAS_OPENSSL

namespace {

// Thin cover over "read some bytes", whichever transport is in play, so the
// header/body loops below don't need to branch on tls at every call site.
struct Transport {
	BSocket* socket;
	SSL* ssl;

	int Read(char* buffer, size_t size)
	{
		return ssl != NULL ? SSL_read(ssl, buffer, (int)size)
			: (int)socket->Read(buffer, size);
	}
	int Write(const char* buffer, size_t size)
	{
		return ssl != NULL ? SSL_write(ssl, buffer, (int)size)
			: (int)socket->Write(buffer, size);
	}
};

// Reads exactly count bytes (beyond whatever is already in body) or fails.
bool
ReadExact(Transport& t, std::string& body, size_t count)
{
	char buf[4096];
	while (body.size() < count) {
		size_t want = count - body.size();
		int n = t.Read(buf, want < sizeof(buf) ? want : sizeof(buf));
		if (n <= 0)
			return false;
		body.append(buf, n);
	}
	return true;
}

// Dechunks a "Transfer-Encoding: chunked" body. leftover holds bytes already
// read past the header block; more are pulled from the transport as needed.
bool
ReadChunked(Transport& t, std::string& leftover, std::string& outBody)
{
	for (;;) {
		size_t lineEnd;
		while ((lineEnd = leftover.find("\r\n")) == std::string::npos) {
			char buf[4096];
			int n = t.Read(buf, sizeof(buf));
			if (n <= 0)
				return false;
			leftover.append(buf, n);
		}
		std::string sizeLine = leftover.substr(0, lineEnd);
		size_t semi = sizeLine.find(';'); // chunk extensions, if any
		if (semi != std::string::npos)
			sizeLine.resize(semi);
		long chunkSize = strtol(sizeLine.c_str(), NULL, 16);
		leftover.erase(0, lineEnd + 2);
		if (chunkSize <= 0)
			return true; // terminating 0-length chunk; ignore any trailer
		while (leftover.size() < (size_t)chunkSize + 2) {
			char buf[4096];
			int n = t.Read(buf, sizeof(buf));
			if (n <= 0)
				return false;
			leftover.append(buf, n);
		}
		outBody.append(leftover, 0, chunkSize);
		leftover.erase(0, chunkSize + 2); // + trailing \r\n after the chunk
	}
}

} // namespace

bool
GetBody(const std::string& url, std::string& outBody, int& outStatus,
	std::string& outError)
{
	std::string current = url;
	const int kMaxRedirects = 5;

	for (int redirect = 0; redirect <= kMaxRedirects; redirect++) {
		ParsedUrl parsed = ParseUrl(current);
		if (!parsed.valid) {
			outError = "invalid or unsupported URL";
			return false;
		}

		BNetworkAddress address;
		if (address.SetTo(parsed.host.c_str(), parsed.port) != B_OK) {
			outError = "could not resolve host: " + parsed.host;
			return false;
		}

		BSocket socket;
		if (socket.Connect(address, 20000000) != B_OK) {
			outError = "could not connect to " + parsed.host;
			return false;
		}

		SSL_CTX* ctx = NULL;
		SSL* ssl = NULL;
		if (parsed.tls) {
			ctx = SSL_CTX_new(TLS_client_method());
			if (ctx == NULL) {
				outError = "SSL context init failed";
				return false;
			}
			SSL_CTX_set_default_verify_paths(ctx);
			ssl = SSL_new(ctx);
			SSL_set_fd(ssl, socket.Socket());
			SSL_set_tlsext_host_name(ssl, parsed.host.c_str());
			if (SSL_connect(ssl) != 1) {
				char detail[64];
				snprintf(detail, sizeof(detail), "TLS handshake failed (SSL error %d)",
					SSL_get_error(ssl, 0));
				outError = detail;
				SSL_free(ssl);
				SSL_CTX_free(ctx);
				return false;
			}
		}
		Transport t = { &socket, ssl };

		std::string request = "GET " + parsed.path + " HTTP/1.1\r\n"
			"Host: " + parsed.host + "\r\n"
			"User-Agent: rworldradio\r\n"
			"Accept: */*\r\n"
			"Connection: close\r\n"
			"\r\n";
		if (t.Write(request.data(), request.size()) <= 0) {
			outError = "could not send request";
			if (ssl != NULL) { SSL_free(ssl); SSL_CTX_free(ctx); }
			return false;
		}

		std::string headerBuf;
		size_t headerEnd = std::string::npos;
		char chunk[4096];
		for (;;) {
			int n = t.Read(chunk, sizeof(chunk));
			if (n <= 0) {
				outError = "connection closed before headers completed";
				if (ssl != NULL) { SSL_free(ssl); SSL_CTX_free(ctx); }
				return false;
			}
			headerBuf.append(chunk, n);
			headerEnd = headerBuf.find("\r\n\r\n");
			if (headerEnd != std::string::npos)
				break;
			if (headerBuf.size() > 16384) {
				outError = "response headers too large";
				if (ssl != NULL) { SSL_free(ssl); SSL_CTX_free(ctx); }
				return false;
			}
		}

		std::string headers = headerBuf.substr(0, headerEnd);
		std::string leftover = headerBuf.substr(headerEnd + 4);
		size_t statusEnd = headers.find("\r\n");
		std::string statusLine = headers.substr(0, statusEnd);
		int status = 0;
		size_t sp = statusLine.find(' ');
		if (sp != std::string::npos)
			status = atoi(statusLine.c_str() + sp + 1);

		if (status >= 300 && status < 400 && redirect < kMaxRedirects) {
			std::string location = ExtractHeader(headers, "location");
			if (ssl != NULL) { SSL_free(ssl); SSL_CTX_free(ctx); }
			if (location.empty()) {
				outError = "redirect with no Location header";
				return false;
			}
			current = ResolveRedirect(current, location);
			continue;
		}

		std::string transferEncoding = ExtractHeader(headers, "transfer-encoding");
		std::string contentLength = ExtractHeader(headers, "content-length");
		std::string body;
		bool ok;
		if (transferEncoding.find("chunked") != std::string::npos) {
			ok = ReadChunked(t, leftover, body);
		} else if (!contentLength.empty()) {
			body = leftover;
			ok = ReadExact(t, body, (size_t)atol(contentLength.c_str()));
		} else {
			// No framing given: read until the server closes the connection.
			body = leftover;
			char buf[4096];
			int n;
			while ((n = t.Read(buf, sizeof(buf))) > 0)
				body.append(buf, n);
			ok = true;
		}

		if (ssl != NULL) { SSL_free(ssl); SSL_CTX_free(ctx); }

		outStatus = status;
		outBody = body;
		if (!ok) {
			outError = "connection closed before body completed";
			return false;
		}
		if (status < 200 || status >= 300) {
			char s[16];
			snprintf(s, sizeof(s), "%d", status);
			outError = "HTTP status " + std::string(s);
			return false;
		}
		return true;
	}

	outError = "too many redirects";
	return false;
}

#endif // HAIKU_HAS_OPENSSL

} // namespace HttpClient
