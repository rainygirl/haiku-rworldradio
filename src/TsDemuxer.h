#ifndef HAIKU_RADIO_TS_DEMUXER_H
#define HAIKU_RADIO_TS_DEMUXER_H

#include <string>

// Extracts the raw elementary audio stream from one MPEG-TS segment (as
// used by HLS), stripping TS packet headers and PES framing: parses PAT to
// find the PMT, PMT to find the audio elementary stream's PID and codec,
// then concatenates that PID's de-PES'd payload.
//
// Stateless and re-parses PAT/PMT from scratch each call. Real encoders
// repeat PAT/PMT periodically specifically so a demuxer can start from any
// segment, so this is fine to call independently per HLS segment rather
// than carrying PID state across segments.
namespace TsDemuxer {

enum class AudioCodec {
	Unknown,
	AdtsAac,
	MpegAudio, // MP3/MP2 (stream_type 0x03/0x04)
};

struct Result {
	AudioCodec codec = AudioCodec::Unknown;
	std::string elementaryStream;
};

Result Extract(const std::string& tsData);

} // namespace TsDemuxer

#endif
