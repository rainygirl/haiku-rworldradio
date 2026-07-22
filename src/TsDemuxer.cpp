#include "TsDemuxer.h"

#include <vector>

namespace {

const size_t kPacketSize = 188;

struct PacketInfo {
	int pid;
	bool payloadStart;
	std::string payload; // TS header (and adaptation field, if any) stripped

	PacketInfo() : pid(0), payloadStart(false) {}
};

std::vector<PacketInfo>
ParsePackets(const std::string& data)
{
	std::vector<PacketInfo> packets;
	size_t pos = 0;
	while (pos + kPacketSize <= data.size()) {
		if (static_cast<unsigned char>(data[pos]) != 0x47) {
			pos++; // resync byte-by-byte if we're not aligned
			continue;
		}
		const unsigned char* p
			= reinterpret_cast<const unsigned char*>(data.data() + pos);
		bool payloadStart = (p[1] & 0x40) != 0;
		int pid = ((p[1] & 0x1F) << 8) | p[2];
		int adaptationFieldControl = (p[3] >> 4) & 0x3;

		if (adaptationFieldControl == 2) {
			pos += kPacketSize; // adaptation field only, no payload
			continue;
		}
		size_t payloadOffset = 4;
		if (adaptationFieldControl == 3)
			payloadOffset = 5 + p[4];
		if (payloadOffset > kPacketSize) {
			pos += kPacketSize; // malformed packet, skip it
			continue;
		}

		PacketInfo info;
		info.pid = pid;
		info.payloadStart = payloadStart;
		info.payload = data.substr(pos + payloadOffset, kPacketSize - payloadOffset);
		packets.push_back(info);
		pos += kPacketSize;
	}
	return packets;
}

int
FindPmtPid(const std::vector<PacketInfo>& packets)
{
	for (size_t pktIdx = 0; pktIdx < packets.size(); pktIdx++) {
		const PacketInfo& pkt = packets[pktIdx];
		if (pkt.pid != 0 || !pkt.payloadStart || pkt.payload.size() < 9)
			continue;
		const unsigned char* raw
			= reinterpret_cast<const unsigned char*>(pkt.payload.data());
		size_t secStart = 1 + raw[0]; // skip pointer_field
		if (secStart + 8 > pkt.payload.size())
			continue;
		const unsigned char* section = raw + secStart;
		if (section[0] != 0x00) // PAT table_id
			continue;
		int sectionLength = ((section[1] & 0x0F) << 8) | section[2];
		size_t progEnd = 3 + sectionLength - 4; // exclude trailing CRC32
		for (size_t i = 8; i + 4 <= progEnd
				&& secStart + i + 4 <= pkt.payload.size(); i += 4) {
			int programNumber = (section[i] << 8) | section[i + 1];
			int pmtPid = ((section[i + 2] & 0x1F) << 8) | section[i + 3];
			if (programNumber != 0) // program_number 0 entries are the NIT
				return pmtPid;
		}
	}
	return -1;
}

struct AudioStreamInfo {
	int pid;
	TsDemuxer::AudioCodec codec;

	AudioStreamInfo() : pid(-1), codec(TsDemuxer::Unknown) {}
};

AudioStreamInfo
FindAudioStream(const std::vector<PacketInfo>& packets, int pmtPid)
{
	AudioStreamInfo info;
	for (size_t pktIdx = 0; pktIdx < packets.size(); pktIdx++) {
		const PacketInfo& pkt = packets[pktIdx];
		if (pkt.pid != pmtPid || !pkt.payloadStart || pkt.payload.size() < 13)
			continue;
		const unsigned char* raw
			= reinterpret_cast<const unsigned char*>(pkt.payload.data());
		size_t secStart = 1 + raw[0];
		if (secStart + 12 > pkt.payload.size())
			continue;
		const unsigned char* section = raw + secStart;
		if (section[0] != 0x02) // PMT table_id
			continue;
		int sectionLength = ((section[1] & 0x0F) << 8) | section[2];
		int programInfoLength = ((section[10] & 0x0F) << 8) | section[11];
		size_t idx = 12 + programInfoLength;
		size_t end = 3 + sectionLength - 4;
		while (idx + 5 <= end && secStart + idx + 5 <= pkt.payload.size()) {
			int streamType = section[idx];
			int elementaryPid = ((section[idx + 1] & 0x1F) << 8) | section[idx + 2];
			int esInfoLength = ((section[idx + 3] & 0x0F) << 8) | section[idx + 4];

			TsDemuxer::AudioCodec codec = TsDemuxer::Unknown;
			if (streamType == 0x0F)
				codec = TsDemuxer::AdtsAac;
			else if (streamType == 0x03 || streamType == 0x04)
				codec = TsDemuxer::MpegAudio;
			if (codec != TsDemuxer::Unknown && info.pid < 0) {
				info.pid = elementaryPid;
				info.codec = codec;
			}
			idx += 5 + esInfoLength;
		}
		if (info.pid >= 0)
			break;
	}
	return info;
}

} // namespace

namespace TsDemuxer {

Result
Extract(const std::string& tsData)
{
	Result result;
	std::vector<PacketInfo> packets = ParsePackets(tsData);

	int pmtPid = FindPmtPid(packets);
	if (pmtPid < 0)
		return result;
	AudioStreamInfo audio = FindAudioStream(packets, pmtPid);
	if (audio.pid < 0)
		return result;

	result.codec = audio.codec;
	for (size_t pktIdx = 0; pktIdx < packets.size(); pktIdx++) {
		const PacketInfo& pkt = packets[pktIdx];
		if (pkt.pid != audio.pid)
			continue;
		if (pkt.payloadStart) {
			if (pkt.payload.size() < 9)
				continue;
			const unsigned char* p
				= reinterpret_cast<const unsigned char*>(pkt.payload.data());
			if (!(p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01))
				continue; // not a PES start code, drop this packet's payload
			size_t pesHeaderDataLength = p[8];
			size_t esStart = 9 + pesHeaderDataLength;
			if (esStart < pkt.payload.size())
				result.elementaryStream.append(pkt.payload.substr(esStart));
		} else {
			result.elementaryStream.append(pkt.payload);
		}
	}
	return result;
}

} // namespace TsDemuxer
