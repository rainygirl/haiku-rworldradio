#include "RadioPlayer.h"

#include <MediaFile.h>
#include <MediaTrack.h>
#include <Message.h>
#include <OS.h>
#include <SoundPlayer.h>
#include <Url.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "HlsAdapterIO.h"
#include "NetworkFetch.h"

namespace {

// Trims trailing CR/LF/whitespace from a Tune.ashx text response, which is
// just the resolved stream URL on its own line.
std::string
TrimTrailing(const std::string& in)
{
	size_t end = in.size();
	while (end > 0 && isspace(static_cast<unsigned char>(in[end - 1])))
		end--;
	return in.substr(0, end);
}

// BMediaFile(BUrl) - via Haiku's own http_streamer add-on - only handles a
// plain progressive HTTP body, not HLS's playlist-of-segments scheme (no
// media add-on for that ships with Haiku at all). Stations whose stream is
// an .m3u8 playlist (common for broadcasters like the BBC, who moved their
// public endpoints to HLS-only some years ago) need HlsAdapterIO instead.
bool
IsHlsUrl(const std::string& url)
{
	std::string path = url;
	size_t query = path.find('?');
	if (query != std::string::npos)
		path.resize(query);
	if (path.size() < 5)
		return false;
	std::string suffix = path.substr(path.size() - 5);
	for (char& c : suffix)
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	return suffix == ".m3u8";
}

size_t
BytesPerSample(uint32 format)
{
	switch (format) {
		case media_raw_audio_format::B_AUDIO_FLOAT:
		case media_raw_audio_format::B_AUDIO_INT:
			return 4;
		case media_raw_audio_format::B_AUDIO_SHORT:
			return 2;
		case media_raw_audio_format::B_AUDIO_UCHAR:
		case media_raw_audio_format::B_AUDIO_CHAR:
			return 1;
		default:
			return 4;
	}
}

// Peak absolute amplitude across one played buffer, normalized to 0.0-1.0,
// for the simple level meter next to the "Now Playing" status. Cheap
// enough to run on every buffer (a few thousand samples at most) without
// needing any dedicated metering support from BSoundPlayer/Haiku.
float
PeakLevel(const void* buffer, size_t sampleCount, uint32 format)
{
	float peak = 0;
	switch (format) {
		case media_raw_audio_format::B_AUDIO_FLOAT:
		{
			const float* samples = static_cast<const float*>(buffer);
			for (size_t i = 0; i < sampleCount; i++) {
				float v = fabsf(samples[i]);
				if (v > peak)
					peak = v;
			}
			break;
		}
		case media_raw_audio_format::B_AUDIO_SHORT:
		{
			const int16_t* samples = static_cast<const int16_t*>(buffer);
			for (size_t i = 0; i < sampleCount; i++) {
				float v = fabsf(samples[i] / 32768.0f);
				if (v > peak)
					peak = v;
			}
			break;
		}
		case media_raw_audio_format::B_AUDIO_INT:
		{
			const int32_t* samples = static_cast<const int32_t*>(buffer);
			for (size_t i = 0; i < sampleCount; i++) {
				float v = fabsf(samples[i] / 2147483648.0f);
				if (v > peak)
					peak = v;
			}
			break;
		}
		case media_raw_audio_format::B_AUDIO_UCHAR:
		{
			const uint8_t* samples = static_cast<const uint8_t*>(buffer);
			for (size_t i = 0; i < sampleCount; i++) {
				float v = fabsf((static_cast<int>(samples[i]) - 128) / 128.0f);
				if (v > peak)
					peak = v;
			}
			break;
		}
		default:
			break;
	}
	return peak > 1.0f ? 1.0f : peak;
}

} // namespace

// BMediaFile(const BUrl&) hands the URL to Haiku's own Streamer add-on
// system (e.g. the http_streamer add-on, built on BAdapterIO) - the same
// mechanism Haiku's own MediaPlayer uses for internet radio. This matters:
// a hand-rolled BDataIO/BPositionIO subclass doesn't report the flags
// (B_MEDIA_STREAMING, size-known-after-headers, etc.) the format sniffer
// needs, and reader plugins silently refuse to even try ("no handler")
// even when the underlying bytes are perfectly valid MP3/AAC. Letting
// BMediaFile drive the network I/O itself sidesteps that entirely.
struct RadioPlayer::Session {
	std::unique_ptr<BMediaFile> mediaFile;
	BMediaTrack* track = nullptr;
	std::unique_ptr<BSoundPlayer> soundPlayer;

	// Only set for HLS stations. BMediaFile(BDataIO*) does NOT take
	// ownership of the source the way BMediaFile(BUrl)/BMediaFile(entry_ref)
	// do (confirmed in Haiku's MediaFile.cpp: fDeleteSource is only set to
	// true for those two, never for the raw-BDataIO* constructor) - so
	// unlike mediaFile, this needs to be deleted here, not by BMediaFile.
	std::unique_ptr<HlsAdapterIO> hlsIo;

	std::string stationName;

	// Updated on every PlayBufferProc call, polled from the UI thread via
	// RadioPlayer::CurrentLevel() for the level meter.
	std::atomic<float> level{0.0f};

	~Session()
	{
		// BSoundPlayer::Stop() blocks until its play thread is idle, so this
		// is safe to do before releasing the track it was reading from.
		if (soundPlayer)
			soundPlayer->Stop();
		soundPlayer.reset();

		if (mediaFile && track)
			mediaFile->ReleaseTrack(track);
		track = nullptr;
		mediaFile.reset(); // must go before hlsIo - it reads from hlsIo
		hlsIo.reset();
	}
};

namespace {

struct SetupArgs {
	RadioPlayer* self;
	std::shared_ptr<RadioPlayer::Session>* session;
	Station station;
	uint64 generation;
};

struct TeardownArgs {
	std::shared_ptr<RadioPlayer::Session>* session;
};

} // namespace

RadioPlayer::RadioPlayer(const BMessenger& statusTarget)
	:
	fStatusTarget(statusTarget),
	fGeneration(0)
{
}

RadioPlayer::~RadioPlayer()
{
	Stop();
}

bool
RadioPlayer::IsCurrent(uint64 generation)
{
	std::lock_guard<std::mutex> lock(fMutex);
	return generation == fGeneration;
}

void
RadioPlayer::EmitStatus(State state, const std::string& stationName,
	const std::string& detail)
{
	BMessage msg(kStatusMessage);
	msg.AddInt32("state", static_cast<int32>(state));
	msg.AddString("station", stationName.c_str());
	msg.AddString("detail", detail.c_str());
	fStatusTarget.SendMessage(&msg);
}

void
RadioPlayer::DetachTeardown(std::shared_ptr<Session> session)
{
	if (!session)
		return;
	auto* args = new TeardownArgs{new std::shared_ptr<Session>(std::move(session))};
	thread_id t = spawn_thread(&RadioPlayer::TeardownThreadEntry,
		"radio-teardown", B_NORMAL_PRIORITY, args);
	if (t < 0) {
		// Extremely unlikely; fall back to a blocking teardown right here
		// rather than leaking the session.
		delete args->session;
		delete args;
		return;
	}
	resume_thread(t);
}

status_t
RadioPlayer::TeardownThreadEntry(void* cookie)
{
	auto* args = static_cast<TeardownArgs*>(cookie);
	args->session->reset();
	delete args->session;
	delete args;
	return B_OK;
}

void
RadioPlayer::Play(const Station& station)
{
	auto session = std::make_shared<Session>();
	session->stationName = station.name;

	std::shared_ptr<Session> old;
	uint64 generation;
	{
		std::lock_guard<std::mutex> lock(fMutex);
		old = fSession;
		fSession = session;
		generation = ++fGeneration;
	}
	DetachTeardown(std::move(old));

	EmitStatus(kConnecting, station.name, "");

	auto* sessionHolder = new std::shared_ptr<Session>(session);
	auto* args = new SetupArgs{this, sessionHolder, station, generation};
	thread_id t = spawn_thread(&RadioPlayer::SetupThreadEntry, "radio-setup",
		B_NORMAL_PRIORITY, args);
	if (t < 0) {
		delete sessionHolder;
		delete args;
		EmitStatus(kError, station.name, "failed to start setup thread");
		return;
	}
	resume_thread(t);
}

void
RadioPlayer::Stop()
{
	std::shared_ptr<Session> old;
	{
		std::lock_guard<std::mutex> lock(fMutex);
		old = std::move(fSession);
		fSession.reset();
		++fGeneration;
	}
	if (old) {
		DetachTeardown(std::move(old));
		EmitStatus(kStopped, "", "");
	}
}

status_t
RadioPlayer::SetupThreadEntry(void* cookie)
{
	auto* args = static_cast<SetupArgs*>(cookie);
	RadioPlayer* self = args->self;
	std::shared_ptr<Session> session = std::move(*args->session);
	Station station = std::move(args->station);
	uint64 generation = args->generation;
	delete args->session;
	delete args;

	self->RunSetup(std::move(session), std::move(station), generation);
	return B_OK;
}

void
RadioPlayer::RunSetup(std::shared_ptr<Session> session, Station station,
	uint64 generation)
{
	if (!IsCurrent(generation))
		return;

	std::string streamUrl = station.PlaybackUrl();
	if (station.needsTuneInResolve) {
		// TuneIn's browse listings only ever give a Tune.ashx resolver link,
		// never a directly playable stream - resolve it to the real URL
		// first. The response is just the URL as plain text.
		NetworkFetch::Result resolved = NetworkFetch::Get(streamUrl);
		if (!resolved.ok) {
			if (IsCurrent(generation)) {
				EmitStatus(kError, station.name,
					"could not resolve TuneIn stream: " + resolved.error);
			}
			return;
		}
		streamUrl = TrimTrailing(resolved.body);
	}

	if (!IsCurrent(generation))
		return;

	if (IsHlsUrl(streamUrl)) {
		session->hlsIo.reset(new HlsAdapterIO(streamUrl));
		// BMediaFile(BDataIO*) never calls Open() on an arbitrary source -
		// that only happens automatically inside BMediaFile(BUrl)'s own
		// internal streamer setup. We're bypassing that (there's no add-on
		// for HLS to hand a BUrl to), so Open() - which starts the worker
		// thread and blocks until the first segment is decoded or the
		// stream fails - has to be called explicitly here.
		status_t openErr = session->hlsIo->Open();
		if (openErr != B_OK) {
			char detail[160];
			snprintf(detail, sizeof(detail), "could not open HLS stream: %s (0x%08lx)",
				strerror(openErr), (long)openErr);
			session->hlsIo.reset();
			if (IsCurrent(generation))
				EmitStatus(kError, station.name, detail);
			return;
		}
		session->mediaFile.reset(new BMediaFile(session->hlsIo.get()));
	} else {
		BUrl url(streamUrl.c_str());
		if (!url.IsValid()) {
			if (IsCurrent(generation))
				EmitStatus(kError, station.name, "invalid stream URL");
			return;
		}
		session->mediaFile.reset(new BMediaFile(url));
	}
	status_t initErr = session->mediaFile->InitCheck();
	if (initErr != B_OK) {
		char detail[160];
		snprintf(detail, sizeof(detail), "stream format error: %s (0x%08lx)",
			strerror(initErr), (long)initErr);
		if (IsCurrent(generation))
			EmitStatus(kError, station.name, detail);
		return;
	}

	if (!IsCurrent(generation))
		return;

	int32 trackCount = session->mediaFile->CountTracks();

	BMediaTrack* track = nullptr;
	for (int32 i = 0; i < trackCount; i++) {
		BMediaTrack* candidate = session->mediaFile->TrackAt(i);
		if (candidate == nullptr)
			continue;
		media_format format = {};
		status_t encErr = candidate->EncodedFormat(&format);
		if (encErr == B_OK && format.IsAudio()) {
			track = candidate;
			break;
		}
		session->mediaFile->ReleaseTrack(candidate);
	}
	if (track == nullptr) {
		if (IsCurrent(generation))
			EmitStatus(kError, station.name, "no audio track in stream");
		return;
	}
	session->track = track;

	media_format requested = {};
	requested.type = B_MEDIA_RAW_AUDIO;
	status_t decErr = track->DecodedFormat(&requested);
	if (decErr != B_OK) {
		if (IsCurrent(generation))
			EmitStatus(kError, station.name, "audio decoder negotiation failed");
		return;
	}

	auto* player = new BSoundPlayer(&requested.u.raw_audio,
		station.name.c_str(), &RadioPlayer::PlayBufferProc, nullptr,
		session.get());
	if (player->InitCheck() != B_OK) {
		delete player;
		if (IsCurrent(generation))
			EmitStatus(kError, station.name, "could not open audio output");
		return;
	}
	session->soundPlayer.reset(player);

	if (!IsCurrent(generation))
		return; // superseded while we were buffering; let it be torn down

	player->SetVolume(1.0);
	player->Start();
	player->SetHasData(true);

	EmitStatus(kPlaying, station.name, "");
}

void
RadioPlayer::PlayBufferProc(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	auto* session = static_cast<Session*>(cookie);
	size_t sampleSize = BytesPerSample(format.format);
	size_t frameSize = sampleSize * format.channel_count;
	if (frameSize == 0 || session->track == nullptr) {
		memset(buffer, 0, size);
		session->level.store(0.0f);
		return;
	}

	int64 frameCount = static_cast<int64>(size / frameSize);
	status_t err = session->track->ReadFrames(buffer, &frameCount);
	size_t producedBytes = err == B_OK
		? static_cast<size_t>(frameCount) * frameSize : 0;

	if (producedBytes < size)
		memset(static_cast<char*>(buffer) + producedBytes, 0, size - producedBytes);

	size_t producedSamples = producedBytes / sampleSize;
	session->level.store(PeakLevel(buffer, producedSamples, format.format));
}

float
RadioPlayer::CurrentLevel() const
{
	std::lock_guard<std::mutex> lock(fMutex);
	return fSession ? fSession->level.load() : 0.0f;
}
