#ifndef HAIKU_RADIO_STREAM_SNIFFER_H
#define HAIKU_RADIO_STREAM_SNIFFER_H

#include <DataIO.h>
#include <SupportDefs.h>

#include <string>

// Decides whether a stream is MP3 or AAC(ADTS) by looking at its first bytes,
// and hands those bytes back so the chosen decoder still sees a complete
// stream. Needed because the embedded decoders each read from the source
// directly: without this, sniffing would eat the bytes the decoder needs, and
// a network stream can't be rewound.
//
// Both formats begin with an 0xFF sync byte, so the discriminator is the
// layer field in the second byte: MPEG audio (MP3) uses layer I/II/III (01,
// 10, 11) while ADTS AAC always has it as 00.
namespace StreamSniffer {

enum Format {
	kUnknown,
	kMp3,
	kAacAdts
};

// Reads up to a few KB from source (consuming them) and guesses the format.
// The consumed bytes are returned in outPrefix so they can be replayed.
Format Sniff(BDataIO* source, std::string& outPrefix);

} // namespace StreamSniffer

// A read-only BDataIO that yields a prefix first and then delegates to the
// underlying source, so a decoder sees the stream as if nothing was consumed.
// Borrows, and does not own, the source.
class PrefixedDataIO : public BDataIO {
public:
	PrefixedDataIO(const std::string& prefix, BDataIO* source);

	ssize_t Read(void* buffer, size_t size);
	ssize_t Write(const void* buffer, size_t size);

private:
	std::string fPrefix;
	size_t fPrefixPos;
	BDataIO* fSource;
};

#endif // HAIKU_RADIO_STREAM_SNIFFER_H
