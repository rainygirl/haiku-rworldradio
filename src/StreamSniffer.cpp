#include "StreamSniffer.h"

#include <cstring>

namespace {

// Enough to skip an ID3v2 tag of typical size and still land on real frames.
const size_t kSniffBytes = 8192;

// True if buf[i..] looks like an MPEG audio (MP3) frame header: 11 sync bits,
// a layer field that isn't the reserved 00, and a bitrate index that isn't
// the "free"/invalid 1111.
bool
LooksLikeMp3Header(const unsigned char* b, size_t avail)
{
	if (avail < 4)
		return false;
	if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0)
		return false;
	unsigned layer = (b[1] >> 1) & 0x03;
	if (layer == 0)
		return false; // reserved for MPEG audio; this is what ADTS uses
	unsigned version = (b[1] >> 3) & 0x03;
	if (version == 1)
		return false; // reserved
	unsigned bitrateIndex = (b[2] >> 4) & 0x0F;
	if (bitrateIndex == 0x0F)
		return false;
	unsigned sampleRateIndex = (b[2] >> 2) & 0x03;
	if (sampleRateIndex == 0x03)
		return false;
	return true;
}

// True if buf[i..] looks like an ADTS AAC frame header: 12 sync bits and the
// layer field fixed at 00, which is what distinguishes it from MP3.
bool
LooksLikeAdtsHeader(const unsigned char* b, size_t avail)
{
	if (avail < 7)
		return false;
	if (b[0] != 0xFF || (b[1] & 0xF0) != 0xF0)
		return false;
	if (((b[1] >> 1) & 0x03) != 0)
		return false; // layer must be 00 for ADTS
	unsigned samplingFreqIndex = (b[2] >> 2) & 0x0F;
	if (samplingFreqIndex > 12)
		return false; // 13..15 are invalid/reserved
	return true;
}

} // namespace

namespace StreamSniffer {

Format
Sniff(BDataIO* source, std::string& outPrefix)
{
	outPrefix.clear();
	char buf[1024];
	while (outPrefix.size() < kSniffBytes) {
		ssize_t n = source->Read(buf, sizeof(buf));
		if (n <= 0)
			break;
		outPrefix.append(buf, n);

		// Scan what we have so far; the first plausible header wins. Checking
		// on every read means a station that identifies itself in the first
		// few hundred bytes doesn't pay for a full 8 KB of buffering.
		const unsigned char* b = (const unsigned char*)outPrefix.data();
		size_t size = outPrefix.size();
		for (size_t i = 0; i + 7 <= size; i++) {
			if (b[i] != 0xFF)
				continue;
			if (LooksLikeAdtsHeader(b + i, size - i))
				return kAacAdts;
			if (LooksLikeMp3Header(b + i, size - i))
				return kMp3;
		}
	}
	return kUnknown;
}

} // namespace StreamSniffer

PrefixedDataIO::PrefixedDataIO(const std::string& prefix, BDataIO* source)
	:
	fPrefix(prefix),
	fPrefixPos(0),
	fSource(source)
{
}

ssize_t
PrefixedDataIO::Read(void* buffer, size_t size)
{
	if (fPrefixPos < fPrefix.size()) {
		size_t avail = fPrefix.size() - fPrefixPos;
		size_t copy = size < avail ? size : avail;
		memcpy(buffer, fPrefix.data() + fPrefixPos, copy);
		fPrefixPos += copy;
		return (ssize_t)copy;
	}
	return fSource->Read(buffer, size);
}

ssize_t
PrefixedDataIO::Write(const void*, size_t)
{
	return B_NOT_ALLOWED;
}
