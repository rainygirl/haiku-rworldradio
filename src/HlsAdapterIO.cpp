#include "HlsAdapterIO.h"

#include <set>

#include "M3u8Parser.h"
#include "NetworkFetch.h"
#include "TsDemuxer.h"

namespace {
// How long Open() (and BAdapterIO's own internal read timeout) waits for
// data before giving up - matches the spirit of Haiku's own HTTPMediaIO,
// which uses a 10s timeout for a single HTTP response; HLS involves an
// extra playlist round-trip first, so this is a bit more generous.
const bigtime_t kHlsTimeout = 20000000;
}

HlsAdapterIO::HlsAdapterIO(const std::string& playlistUrl)
	:
	BAdapterIO(B_MEDIA_STREAMING | B_MEDIA_SEEKABLE, kHlsTimeout),
	fPlaylistUrl(playlistUrl),
	fInputAdapter(nullptr),
	fWorkerThread(-1),
	fInitSem(create_sem(0, "hls-adapter-init")),
	fInitReleased(false),
	fInitSucceeded(false),
	fStopRequested(false),
	fRunning(false)
{
}

HlsAdapterIO::~HlsAdapterIO()
{
	fStopRequested = true;
	if (fWorkerThread >= 0) {
		status_t exitValue;
		wait_for_thread(fWorkerThread, &exitValue);
	}
	delete_sem(fInitSem);
}

void
HlsAdapterIO::GetFlags(int32* flags) const
{
	*flags = B_MEDIA_STREAMING | B_MEDIA_SEEK_BACKWARD;
}

status_t
HlsAdapterIO::Open()
{
	fInputAdapter = BuildInputAdapter();

	fWorkerThread = spawn_thread(&HlsAdapterIO::WorkerThreadEntry,
		"hls-worker", B_NORMAL_PRIORITY, this);
	if (fWorkerThread < 0)
		return fWorkerThread;
	resume_thread(fWorkerThread);

	status_t err = acquire_sem_etc(fInitSem, 1, B_RELATIVE_TIMEOUT, kHlsTimeout);
	if (err != B_OK)
		return err;
	if (!fInitSucceeded)
		return B_ERROR;

	return BAdapterIO::Open();
}

bool
HlsAdapterIO::IsRunning() const
{
	return fRunning.load();
}

status_t
HlsAdapterIO::WorkerThreadEntry(void* cookie)
{
	static_cast<HlsAdapterIO*>(cookie)->RunWorker();
	return B_OK;
}

void
HlsAdapterIO::ReleaseInitOnce(bool success)
{
	if (fInitReleased)
		return;
	fInitReleased = true;
	fInitSucceeded = success;
	release_sem(fInitSem);
}

void
HlsAdapterIO::RunWorker()
{
	fRunning = true;

	std::string mediaPlaylistUrl = fPlaylistUrl;

	NetworkFetch::Result initial = NetworkFetch::Get(fPlaylistUrl);
	if (!initial.ok) {
		ReleaseInitOnce(false);
		fRunning = false;
		return;
	}

	// If this is a master playlist, pick the highest-bandwidth variant -
	// internet radio bitrates are all small enough that "best available"
	// isn't a real bandwidth concern for anyone able to stream at all.
	if (M3u8Parser::IsMasterPlaylist(initial.body)) {
		std::vector<M3u8Parser::Variant> variants
			= M3u8Parser::ParseMasterPlaylist(initial.body, fPlaylistUrl);
		if (variants.empty()) {
			ReleaseInitOnce(false);
			fRunning = false;
			return;
		}
		const M3u8Parser::Variant* best = &variants[0];
		for (const M3u8Parser::Variant& variant : variants) {
			if (variant.bandwidth > best->bandwidth)
				best = &variant;
		}
		mediaPlaylistUrl = best->uri;
	}

	std::set<std::string> seenSegments;

	while (!fStopRequested.load()) {
		NetworkFetch::Result playlistFetch = NetworkFetch::Get(mediaPlaylistUrl);
		if (!playlistFetch.ok) {
			ReleaseInitOnce(false); // only takes effect if never succeeded yet
			snooze(2000000);
			continue;
		}

		M3u8Parser::MediaPlaylist playlist
			= M3u8Parser::ParseMediaPlaylist(playlistFetch.body, mediaPlaylistUrl);

		bool wroteAny = false;
		for (const M3u8Parser::Segment& segment : playlist.segments) {
			if (fStopRequested.load())
				break;
			if (seenSegments.count(segment.uri) != 0)
				continue;
			seenSegments.insert(segment.uri);

			NetworkFetch::Result segmentFetch = NetworkFetch::Get(segment.uri);
			if (!segmentFetch.ok)
				continue;

			TsDemuxer::Result demuxed = TsDemuxer::Extract(segmentFetch.body);
			if (demuxed.elementaryStream.empty())
				continue;

			fInputAdapter->Write(demuxed.elementaryStream.data(),
				demuxed.elementaryStream.size());
			ReleaseInitOnce(true);
			wroteAny = true;
		}

		if (playlist.isEndList)
			break;
		if (fStopRequested.load())
			break;

		if (!wroteAny) {
			// Nothing new this round - wait roughly a target-duration
			// before asking again, matching standard HLS live polling.
			bigtime_t waitTime
				= static_cast<bigtime_t>(playlist.targetDuration * 1000000);
			if (waitTime <= 0)
				waitTime = 3000000;
			snooze(waitTime);
		}

		// A live sliding-window playlist won't repeat old segment URIs, so
		// this can be dropped periodically rather than growing forever
		// across a multi-hour listen.
		if (seenSegments.size() > 500)
			seenSegments.clear();
	}

	ReleaseInitOnce(false); // no-op if we already succeeded above
	fRunning = false;
}
