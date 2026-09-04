#ifndef HAIKU_RADIO_MP3_STREAM_DECODER_H
#define HAIKU_RADIO_MP3_STREAM_DECODER_H

#include <DataIO.h>
#include <MediaDefs.h>
#include <SupportDefs.h>

#include "thirdparty/minimp3.h"

// Decodes an MP3 byte stream to raw PCM entirely in-process, so playback does
// NOT depend on any Media Kit reader/decoder add-on being installed. This is
// the whole reason it exists: the RENKU arm64 bootstrap image ships with an
// empty /boot/system/add-ons/media/plugins, so BMediaFile/BMediaTrack return
// B_MEDIA_NO_HANDLER for every stream even though the bytes are valid MP3.
// Embedding minimp3 (public domain) and feeding its PCM straight to a
// BSoundPlayer sidesteps the plugin architecture completely - the same tactic
// HttpAudioIO uses to sidestep the broken Network Kit TLS.
//
// It reads encoded bytes on demand from a caller-owned BDataIO (the same
// HttpAudioIO / HlsAdapterIO source the BMediaFile path uses), so it streams
// rather than buffering the whole station. It does NOT take ownership of the
// source. MP3 only - AAC/HE-AAC stations still need the Media Kit path (which
// works on x86 where the plugins exist); on arm64 those simply won't play
// until a decoder for them is embedded too.
class Mp3StreamDecoder {
public:
	// source is borrowed, not owned, and must already be Open()ed and
	// delivering bytes (HttpAudioIO/HlsAdapterIO both are, after their own
	// Open()).
	explicit Mp3StreamDecoder(BDataIO* source);
	~Mp3StreamDecoder();

	// Pulls and decodes frames until the first one is decoded, so the audio
	// format (channel count, frame rate) is known. Blocks on the source's
	// I/O the same way the first BMediaTrack read would. B_OK on success;
	// B_IO_ERROR if the stream ended or never yielded a decodable MP3 frame.
	status_t Open();

	// Valid only after a successful Open(). 16-bit host-endian PCM.
	const media_raw_audio_format& Format() const { return fFormat; }

	// Fills up to size bytes of buffer with decoded PCM, decoding more frames
	// from the source as needed. Returns the number of bytes actually written
	// (< size only at end of stream or after RequestStop()). Called from the
	// BSoundPlayer buffer thread.
	size_t Read(void* buffer, size_t size);

	// Makes the current and subsequent Read()/Open() calls return promptly so
	// teardown does not block on a stalled network read.
	void RequestStop() { fStop = true; }

private:
	// Ensures at least some encoded bytes are available in fInput, reading
	// from the source and compacting consumed bytes. Returns false at EOF or
	// after RequestStop().
	bool FillInput();

	// Decodes exactly one MP3 frame into fPcm (setting fPcmSampleCount and,
	// on the first frame, fFormat). Returns true if a frame was produced,
	// false if the stream ended before another frame could be decoded.
	bool DecodeFrame();

	BDataIO* fSource;
	mp3dec_t fDecoder;

	uint8* fInput;         // rolling encoded-byte buffer
	size_t fInputCapacity; // allocated size of fInput
	size_t fInputSize;     // valid bytes currently in fInput
	size_t fInputPos;      // next undecoded byte in fInput

	int16* fPcm;           // decoded PCM of the most recent frame
	size_t fPcmSampleCount;// total int16 samples in fPcm (channels interleaved)
	size_t fPcmPos;        // next unread sample in fPcm

	media_raw_audio_format fFormat;
	bool fFormatKnown;
	bool fSourceExhausted;
	bool fStop;
};

#endif // HAIKU_RADIO_MP3_STREAM_DECODER_H
