// This translation unit is the single place minimp3's implementation is
// compiled - MINIMP3_IMPLEMENTATION must be defined in exactly one .cpp.
// MINIMP3_NO_STDIO drops the file-based helpers we don't use (we only ever
// decode from a BDataIO), keeping the footprint to the raw frame decoder.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "Mp3StreamDecoder.h"

#include <cstring>

namespace {

// One decoded MP3 frame is at most 1152 samples * 2 channels. Read the source
// in reasonably large chunks so a slow (single-core arm64) guest isn't doing a
// syscall per handful of bytes.
const size_t kInputChunk = 16 * 1024;
const size_t kInputMax = 64 * 1024; // plenty to hold several frames plus slack

} // namespace

Mp3StreamDecoder::Mp3StreamDecoder(BDataIO* source)
	:
	fSource(source),
	fInput(NULL),
	fInputCapacity(0),
	fInputSize(0),
	fInputPos(0),
	fPcm(NULL),
	fPcmSampleCount(0),
	fPcmPos(0),
	fFormatKnown(false),
	fSourceExhausted(false),
	fStop(false)
{
	mp3dec_init(&fDecoder);
	memset(&fFormat, 0, sizeof(fFormat));
	fInputCapacity = kInputMax;
	fInput = new uint8[fInputCapacity];
	// minimp3 writes at most MINIMP3_MAX_SAMPLES_PER_FRAME int16s per frame.
	fPcm = new int16[MINIMP3_MAX_SAMPLES_PER_FRAME];
}

Mp3StreamDecoder::~Mp3StreamDecoder()
{
	delete[] fInput;
	delete[] fPcm;
}

bool
Mp3StreamDecoder::FillInput()
{
	if (fStop)
		return false;

	// Compact: drop already-consumed bytes to the front so there's room to
	// append. minimp3 needs a contiguous run to find/skip a frame.
	if (fInputPos > 0) {
		size_t remaining = fInputSize - fInputPos;
		if (remaining > 0)
			memmove(fInput, fInput + fInputPos, remaining);
		fInputSize = remaining;
		fInputPos = 0;
	}

	if (fSourceExhausted)
		return false;
	if (fInputSize >= fInputCapacity)
		return true; // buffer already full; caller can decode from what's here

	size_t want = fInputCapacity - fInputSize;
	if (want > kInputChunk)
		want = kInputChunk;

	ssize_t got = fSource->Read(fInput + fInputSize, want);
	if (got > 0) {
		fInputSize += static_cast<size_t>(got);
		return true;
	}

	// 0 == clean EOF, negative == error; either way there are no more bytes.
	fSourceExhausted = true;
	return false;
}

bool
Mp3StreamDecoder::DecodeFrame()
{
	for (;;) {
		if (fStop)
			return false;

		size_t avail = fInputSize - fInputPos;
		if (avail > 0) {
			mp3dec_frame_info_t info;
			int samples = mp3dec_decode_frame(&fDecoder,
				fInput + fInputPos, static_cast<int>(avail), fPcm, &info);

			// frame_bytes > 0 means minimp3 consumed that many bytes (a decoded
			// frame, or ID3/garbage it skipped). Advance regardless.
			if (info.frame_bytes > 0) {
				fInputPos += static_cast<size_t>(info.frame_bytes);

				if (samples > 0) {
					if (!fFormatKnown) {
						fFormat.format = media_raw_audio_format::B_AUDIO_SHORT;
						fFormat.channel_count = static_cast<uint32>(info.channels);
						fFormat.frame_rate = static_cast<float>(info.hz);
						fFormat.byte_order = B_MEDIA_HOST_ENDIAN;
						fFormat.buffer_size = 4096;
						fFormatKnown = true;
					}
					fPcmSampleCount = static_cast<size_t>(samples)
						* static_cast<size_t>(info.channels);
					fPcmPos = 0;
					return true;
				}
				// Skipped non-audio bytes; keep going without needing more input.
				continue;
			}

			// frame_bytes == 0: not enough bytes to find a frame header yet.
			// Fall through to read more, unless the buffer is already full and
			// still undecodable (corrupt) - guard against a spin below.
		}

		if (!FillInput()) {
			// No more input will arrive. If nothing is left to try, we're done.
			if (fInputSize - fInputPos == 0)
				return false;
			// A trailing partial frame that can never complete: bail rather
			// than loop forever on the same undecodable tail.
			if (fSourceExhausted && avail == fInputSize - fInputPos)
				return false;
		}
	}
}

status_t
Mp3StreamDecoder::Open()
{
	if (DecodeFrame())
		return B_OK;
	return B_IO_ERROR;
}

size_t
Mp3StreamDecoder::Read(void* buffer, size_t size)
{
	uint8* out = static_cast<uint8*>(buffer);
	size_t written = 0;

	while (written < size) {
		if (fPcmPos >= fPcmSampleCount) {
			// Need another decoded frame.
			if (!DecodeFrame())
				break; // end of stream / stopped
		}

		size_t availSamples = fPcmSampleCount - fPcmPos;
		size_t availBytes = availSamples * sizeof(int16);
		size_t need = size - written;
		size_t copy = need < availBytes ? need : availBytes;

		memcpy(out + written, fPcm + fPcmPos, copy);
		written += copy;
		fPcmPos += copy / sizeof(int16);
	}

	return written;
}
