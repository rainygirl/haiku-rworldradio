#include "RadioPlayer.h"

#include <MediaFile.h>
#include <MediaTrack.h>
#include <Message.h>
#include <OS.h>
#include <SoundPlayer.h>
#include <Url.h>

#include <Autolock.h>

#include <cctype>
#include <cmath>
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
	for (size_t i = 0; i < suffix.size(); i++)
		suffix[i] = static_cast<char>(tolower(static_cast<unsigned char>(suffix[i])));
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
			const int16* samples = static_cast<const int16*>(buffer);
			for (size_t i = 0; i < sampleCount; i++) {
				float v = fabsf(samples[i] / 32768.0f);
				if (v > peak)
					peak = v;
			}
			break;
		}
		case media_raw_audio_format::B_AUDIO_INT:
		{
			const int32* samples = static_cast<const int32*>(buffer);
			for (size_t i = 0; i < sampleCount; i++) {
				float v = fabsf(samples[i] / 2147483648.0f);
				if (v > peak)
					peak = v;
			}
			break;
		}
		case media_raw_audio_format::B_AUDIO_UCHAR:
		{
			const uint8* samples = static_cast<const uint8*>(buffer);
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

// IEEE-754 bit-pattern round trip so a float can be stored/loaded with
// Haiku's atomic_set()/atomic_get() (int32-only, no std::atomic pre-C++11).
int32
FloatToBits(float v)
{
	int32 bits;
	memcpy(&bits, &v, sizeof(bits));
	return bits;
}

float
BitsToFloat(int32 bits)
{
	float v;
	memcpy(&v, &bits, sizeof(v));
	return v;
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
	BMediaFile* mediaFile;
	BMediaTrack* track;
	BSoundPlayer* soundPlayer;

	// Only set for HLS stations. BMediaFile(BDataIO*) does NOT take
	// ownership of the source the way BMediaFile(BUrl)/BMediaFile(entry_ref)
	// do (confirmed in Haiku's MediaFile.cpp: fDeleteSource is only set to
	// true for those two, never for the raw-BDataIO* constructor) - so
	// unlike mediaFile, this needs to be deleted here, not by BMediaFile.
	HlsAdapterIO* hlsIo;

	std::string stationName;

	// Updated on every PlayBufferProc call, polled from the UI thread via
	// RadioPlayer::CurrentLevel() for the level meter - an IEEE-754 bit
	// pattern read/written via atomic_set()/atomic_get() (see FloatToBits/
	// BitsToFloat above).
	int32 levelBits;

	Session()
		:
		mediaFile(NULL),
		track(NULL),
		soundPlayer(NULL),
		hlsIo(NULL),
		levelBits(0)
	{
	}

	~Session()
	{
		// BSoundPlayer::Stop() blocks until its play thread is idle, so this
		// is safe to do before releasing the track it was reading from.
		if (soundPlayer != NULL)
			soundPlayer->Stop();
		delete soundPlayer;
		soundPlayer = NULL;

		if (mediaFile != NULL && track != NULL)
			mediaFile->ReleaseTrack(track);
		track = NULL;
		delete mediaFile; // must go before hlsIo - it reads from hlsIo
		mediaFile = NULL;
		delete hlsIo;
		hlsIo = NULL;
	}
};

RadioPlayer::SessionPtr::SessionPtr()
	:
	fSession(NULL),
	fRefCount(NULL)
{
}

RadioPlayer::SessionPtr::SessionPtr(Session* session)
	:
	fSession(session),
	fRefCount(session != NULL ? new int32(1) : NULL)
{
}

RadioPlayer::SessionPtr::SessionPtr(const SessionPtr& other)
	:
	fSession(other.fSession),
	fRefCount(other.fRefCount)
{
	Acquire();
}

RadioPlayer::SessionPtr&
RadioPlayer::SessionPtr::operator=(const SessionPtr& other)
{
	if (this != &other) {
		Release();
		fSession = other.fSession;
		fRefCount = other.fRefCount;
		Acquire();
	}
	return *this;
}

RadioPlayer::SessionPtr::~SessionPtr()
{
	Release();
}

void
RadioPlayer::SessionPtr::Acquire()
{
	if (fRefCount != NULL)
		atomic_add(fRefCount, 1);
}

void
RadioPlayer::SessionPtr::Release()
{
	// atomic_add() returns the value from BEFORE the add, so a result of 1
	// means this was the last outstanding reference.
	if (fRefCount != NULL && atomic_add(fRefCount, -1) == 1) {
		delete fSession;
		delete fRefCount;
	}
	fSession = NULL;
	fRefCount = NULL;
}

void
RadioPlayer::SessionPtr::Reset()
{
	Release();
}

namespace {

struct SetupArgs {
	RadioPlayer* self;
	RadioPlayer::SessionPtr* session;
	Station station;
	uint64 generation;

	SetupArgs(RadioPlayer* s, RadioPlayer::SessionPtr* sess,
		const Station& st, uint64 gen)
		:
		self(s),
		session(sess),
		station(st),
		generation(gen)
	{
	}
};

struct TeardownArgs {
	RadioPlayer::SessionPtr* session;

	explicit TeardownArgs(RadioPlayer::SessionPtr* s) : session(s) {}
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
	BAutolock lock(fMutex);
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
RadioPlayer::DetachTeardown(SessionPtr session)
{
	if (!session)
		return;
	TeardownArgs* args = new TeardownArgs(new SessionPtr(session));
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
	TeardownArgs* args = static_cast<TeardownArgs*>(cookie);
	args->session->Reset();
	delete args->session;
	delete args;
	return B_OK;
}

void
RadioPlayer::Play(const Station& station)
{
	SessionPtr session(new Session());
	session->stationName = station.name;

	SessionPtr old;
	uint64 generation;
	{
		BAutolock lock(fMutex);
		old = fSession;
		fSession = session;
		generation = ++fGeneration;
	}
	DetachTeardown(old);

	EmitStatus(kConnecting, station.name, "");

	SessionPtr* sessionHolder = new SessionPtr(session);
	SetupArgs* args = new SetupArgs(this, sessionHolder, station, generation);
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
	SessionPtr old;
	{
		BAutolock lock(fMutex);
		old = fSession;
		fSession.Reset();
		++fGeneration;
	}
	if (old) {
		DetachTeardown(old);
		EmitStatus(kStopped, "", "");
	}
}

status_t
RadioPlayer::SetupThreadEntry(void* cookie)
{
	SetupArgs* args = static_cast<SetupArgs*>(cookie);
	RadioPlayer* self = args->self;
	SessionPtr session = *args->session;
	Station station = args->station;
	uint64 generation = args->generation;
	delete args->session;
	delete args;

	self->RunSetup(session, station, generation);
	return B_OK;
}

void
RadioPlayer::RunSetup(SessionPtr session, Station station, uint64 generation)
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
		session->hlsIo = new HlsAdapterIO(streamUrl);
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
			delete session->hlsIo;
			session->hlsIo = NULL;
			if (IsCurrent(generation))
				EmitStatus(kError, station.name, detail);
			return;
		}
		session->mediaFile = new BMediaFile(session->hlsIo);
	} else {
		// Explicit second argument: this Haiku SDK's Url.h declares both
		// BUrl(const char*, bool = true) and a legacy BUrl(const char*)
		// overload, which is ambiguous with a single argument.
		BUrl url(streamUrl.c_str(), true);
		if (!url.IsValid()) {
			if (IsCurrent(generation))
				EmitStatus(kError, station.name, "invalid stream URL");
			return;
		}
		session->mediaFile = new BMediaFile(url);
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

	BMediaTrack* track = NULL;
	for (int32 i = 0; i < trackCount; i++) {
		BMediaTrack* candidate = session->mediaFile->TrackAt(i);
		if (candidate == NULL)
			continue;
		media_format format;
		memset(&format, 0, sizeof(format));
		status_t encErr = candidate->EncodedFormat(&format);
		if (encErr == B_OK && format.IsAudio()) {
			track = candidate;
			break;
		}
		session->mediaFile->ReleaseTrack(candidate);
	}
	if (track == NULL) {
		if (IsCurrent(generation))
			EmitStatus(kError, station.name, "no audio track in stream");
		return;
	}
	session->track = track;

	media_format requested;
	memset(&requested, 0, sizeof(requested));
	requested.type = B_MEDIA_RAW_AUDIO;
	status_t decErr = track->DecodedFormat(&requested);
	if (decErr != B_OK) {
		if (IsCurrent(generation))
			EmitStatus(kError, station.name, "audio decoder negotiation failed");
		return;
	}

	BSoundPlayer* player = new BSoundPlayer(&requested.u.raw_audio,
		station.name.c_str(), &RadioPlayer::PlayBufferProc, NULL,
		session.Get());
	if (player->InitCheck() != B_OK) {
		delete player;
		if (IsCurrent(generation))
			EmitStatus(kError, station.name, "could not open audio output");
		return;
	}
	session->soundPlayer = player;

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
	Session* session = static_cast<Session*>(cookie);
	size_t sampleSize = BytesPerSample(format.format);
	size_t frameSize = sampleSize * format.channel_count;
	if (frameSize == 0 || session->track == NULL) {
		memset(buffer, 0, size);
		atomic_set(&session->levelBits, FloatToBits(0.0f));
		return;
	}

	int64 frameCount = static_cast<int64>(size / frameSize);
	status_t err = session->track->ReadFrames(buffer, &frameCount);
	size_t producedBytes = err == B_OK
		? static_cast<size_t>(frameCount) * frameSize : 0;

	if (producedBytes < size)
		memset(static_cast<char*>(buffer) + producedBytes, 0, size - producedBytes);

	size_t producedSamples = producedBytes / sampleSize;
	atomic_set(&session->levelBits,
		FloatToBits(PeakLevel(buffer, producedSamples, format.format)));
}

float
RadioPlayer::CurrentLevel() const
{
	BAutolock lock(fMutex);
	return fSession ? BitsToFloat(atomic_get(&fSession->levelBits)) : 0.0f;
}
