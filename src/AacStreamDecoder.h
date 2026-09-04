#ifndef HAIKU_RADIO_AAC_STREAM_DECODER_H
#define HAIKU_RADIO_AAC_STREAM_DECODER_H

#include <DataIO.h>
#include <MediaDefs.h>
#include <SupportDefs.h>

// Opaque so the FDK-AAC headers stay out of this one; the .cpp owns them.
struct AAC_DECODER_INSTANCE;
typedef struct AAC_DECODER_INSTANCE* HANDLE_AACDECODER;

// Decodes an AAC (ADTS) byte stream to raw PCM entirely in-process, for the
// same reason Mp3StreamDecoder exists: the RENKU arm64 image ships an empty
// /boot/system/add-ons/media/plugins, so BMediaFile can't decode anything, and
// the ffmpeg build feature that would normally supply an AAC decoder is not
// available on this bootstrap. A large fraction of internet radio is AAC or
// HE-AAC ("AAC+") rather than MP3, so without this those stations cannot play
// at all on that target.
//
// Backed by Fraunhofer's FDK-AAC (vendored under thirdparty/fdk-aac), driven
// through its documented streaming pattern: aacDecoder_Fill() to hand it
// bytes, aacDecoder_DecodeFrame() to pull a frame, AAC_DEC_NOT_ENOUGH_BITS to
// mean "feed me more". Handles ADTS, which is what icecast/shoutcast AAC
// stations serve; it does not demux a container.
//
// Like Mp3StreamDecoder it borrows, and does not own, the source BDataIO.
class AacStreamDecoder {
public:
	explicit AacStreamDecoder(BDataIO* source);
	~AacStreamDecoder();

	// Pulls and decodes until the first frame comes out, so the audio format
	// is known. B_OK on success; B_IO_ERROR if the stream ended or never
	// yielded a decodable AAC frame (which is also how a non-AAC stream fails
	// here, letting the caller fall back to another decoder).
	status_t Open();

	// Valid only after a successful Open(). 16-bit host-endian PCM.
	const media_raw_audio_format& Format() const { return fFormat; }

	// Fills up to size bytes of buffer with decoded PCM, decoding more as
	// needed. Returns bytes written (< size only at end of stream or after
	// RequestStop()). Called from the BSoundPlayer buffer thread.
	size_t Read(void* buffer, size_t size);

	// Makes current and subsequent Read()/Open() calls return promptly, so
	// teardown doesn't block on a stalled network read.
	void RequestStop() { fStop = true; }

private:
	bool FillInput();
	bool DecodeFrame();

	BDataIO* fSource;
	HANDLE_AACDECODER fDecoder;

	unsigned char* fInput;	// rolling encoded-byte buffer
	size_t fInputCapacity;
	size_t fInputSize;		// valid bytes in fInput

	int16* fPcm;			// decoded PCM of the most recent frame
	size_t fPcmSampleCount;	// total int16 samples (channels interleaved)
	size_t fPcmPos;			// next unread sample

	media_raw_audio_format fFormat;
	bool fFormatKnown;
	bool fSourceExhausted;
	bool fStop;
};

#endif // HAIKU_RADIO_AAC_STREAM_DECODER_H
