#include "AacStreamDecoder.h"

#include <cstring>

#include "thirdparty/fdk-aac/libAACdec/include/aacdecoder_lib.h"

namespace {

// Read the source in reasonably large chunks so a slow single-core arm64
// guest isn't doing a syscall per handful of bytes.
const size_t kInputChunk = 8 * 1024;
const size_t kInputMax = 32 * 1024;

// FDK-AAC can emit up to 8 channels * 2048 samples for one frame; size the
// PCM buffer for the worst case it documents rather than the common stereo
// case, so a surround stream can't overrun it.
const size_t kMaxPcmSamples = 8 * 2048;

} // namespace

AacStreamDecoder::AacStreamDecoder(BDataIO* source)
	:
	fSource(source),
	fDecoder(NULL),
	fInput(NULL),
	fInputCapacity(kInputMax),
	fInputSize(0),
	fPcm(NULL),
	fPcmSampleCount(0),
	fPcmPos(0),
	fFormatKnown(false),
	fSourceExhausted(false),
	fStop(false)
{
	memset(&fFormat, 0, sizeof(fFormat));
	fInput = new unsigned char[fInputCapacity];
	fPcm = new int16[kMaxPcmSamples];
	// TT_MP4_ADTS: the framing icecast/shoutcast AAC stations serve. One
	// layer - we never feed it scalable/multi-layer streams.
	fDecoder = aacDecoder_Open(TT_MP4_ADTS, 1);
}

AacStreamDecoder::~AacStreamDecoder()
{
	if (fDecoder != NULL)
		aacDecoder_Close(fDecoder);
	delete[] fInput;
	delete[] fPcm;
}

bool
AacStreamDecoder::FillInput()
{
	if (fStop || fSourceExhausted)
		return false;
	if (fInputSize >= fInputCapacity)
		return true; // already full; caller can decode from what's here

	size_t want = fInputCapacity - fInputSize;
	if (want > kInputChunk)
		want = kInputChunk;

	ssize_t got = fSource->Read(fInput + fInputSize, want);
	if (got > 0) {
		fInputSize += (size_t)got;
		return true;
	}

	// 0 == clean EOF, negative == error; either way no more bytes.
	fSourceExhausted = true;
	return false;
}

bool
AacStreamDecoder::DecodeFrame()
{
	for (;;) {
		if (fStop || fDecoder == NULL)
			return false;

		if (fInputSize > 0) {
			// aacDecoder_Fill() consumes from the front and reports how much
			// is left unconsumed in bytesValid, so shift the remainder down.
			UCHAR* buffers[1] = { fInput };
			const UINT bufferSizes[1] = { (UINT)fInputSize };
			UINT bytesValid = (UINT)fInputSize;
			AAC_DECODER_ERROR fillErr
				= aacDecoder_Fill(fDecoder, buffers, bufferSizes, &bytesValid);

			size_t consumed = fInputSize - (size_t)bytesValid;
			if (consumed > 0) {
				memmove(fInput, fInput + consumed, (size_t)bytesValid);
				fInputSize = (size_t)bytesValid;
			}

			if (fillErr == AAC_DEC_OK) {
				AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(fDecoder,
					fPcm, (INT)kMaxPcmSamples, 0);
				if (err == AAC_DEC_OK) {
					CStreamInfo* info = aacDecoder_GetStreamInfo(fDecoder);
					if (info != NULL && info->sampleRate > 0
						&& info->numChannels > 0) {
						if (!fFormatKnown) {
							fFormat.format
								= media_raw_audio_format::B_AUDIO_SHORT;
							fFormat.channel_count = (uint32)info->numChannels;
							fFormat.frame_rate = (float)info->sampleRate;
							fFormat.byte_order = B_MEDIA_HOST_ENDIAN;
							fFormat.buffer_size = 4096;
							fFormatKnown = true;
						}
						fPcmSampleCount = (size_t)info->frameSize
							* (size_t)info->numChannels;
						fPcmPos = 0;
						if (fPcmSampleCount > 0)
							return true;
					}
					// Decoded but produced nothing usable; keep going.
					continue;
				}
				if (err != AAC_DEC_NOT_ENOUGH_BITS) {
					// Concealed/!OK frames are normal on a live stream (a lost
					// packet, or the leading bytes before the first full ADTS
					// frame). Keep feeding rather than giving up, unless the
					// input is exhausted, which the fill path below handles.
					if (IS_DECODE_ERROR(err) && fSourceExhausted)
						return false;
					continue;
				}
			}
		}

		size_t before = fInputSize;
		if (!FillInput()) {
			// No more input will arrive. If nothing is left, or we made no
			// progress on the tail, stop rather than spinning on it.
			if (fInputSize == 0 || fInputSize == before)
				return false;
		}
	}
}

status_t
AacStreamDecoder::Open()
{
	if (fDecoder == NULL)
		return B_NO_INIT;
	if (DecodeFrame())
		return B_OK;
	return B_IO_ERROR;
}

size_t
AacStreamDecoder::Read(void* buffer, size_t size)
{
	unsigned char* out = (unsigned char*)buffer;
	size_t written = 0;

	while (written < size) {
		if (fPcmPos >= fPcmSampleCount) {
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
