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
#include "HttpAudioIO.h"
#include "NetworkFetch.h"

#ifdef RWORLDRADIO_EMBEDDED_MP3
#include "AacStreamDecoder.h"
#include "Mp3StreamDecoder.h"
#include "StreamSniffer.h"
#endif

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

bool
IsHttpsUrl(const std::string& url)
{
	if (url.size() < 8)
		return false;
	std::string scheme = url.substr(0, 8);
	for (size_t i = 0; i < scheme.size(); i++)
		scheme[i] = static_cast<char>(tolower(static_cast<unsigned char>(scheme[i])));
	return scheme == "https://";
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
//
// That add-on only speaks plain http:// though - confirmed by hand against
// several live stations that offer both an http:// and an https:// URL for
// the same stream: the http:// one opens fine, the https:// one comes back
// B_MEDIA_NO_HANDLER every time. So https:// direct URLs go through
// HttpAudioIO instead (also a BAdapterIO, so it reports the same flags),
// which fetches over the Network Kit's own BUrlRequest - the same one
// NetworkFetch already uses successfully for https - the same way
// HlsAdapterIO does for HLS.
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

	// Only set for https:// direct (non-HLS) stations - see the comment
	// above. Same ownership rules as hlsIo.
	HttpAudioIO* httpIo;

#ifdef RWORLDRADIO_EMBEDDED_MP3
	// Set instead of mediaFile/track on builds with no Media Kit decoder
	// plugins (the arm64 bootstrap image): decodes the stream straight from
	// httpIo in-process. Exactly one of these is non-NULL, chosen by sniffing
	// the first bytes. Both read through sniffIo, which replays the sniffed
	// prefix ahead of httpIo, so all three are deleted before httpIo below.
	Mp3StreamDecoder* mp3Decoder;
	AacStreamDecoder* aacDecoder;
	PrefixedDataIO* sniffIo;
#endif

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
		httpIo(NULL),
#ifdef RWORLDRADIO_EMBEDDED_MP3
		mp3Decoder(NULL),
		aacDecoder(NULL),
		sniffIo(NULL),
#endif
		levelBits(0)
	{
	}

	~Session()
	{
#ifdef RWORLDRADIO_EMBEDDED_MP3
		// Unblock a decoder read that may be waiting on httpIo before
		// BSoundPlayer::Stop() waits for the play thread that drives it.
		if (mp3Decoder != NULL)
			mp3Decoder->RequestStop();
		if (aacDecoder != NULL)
			aacDecoder->RequestStop();
#endif
		// BSoundPlayer::Stop() blocks until its play thread is idle, so this
		// is safe to do before releasing the track it was reading from.
		if (soundPlayer != NULL)
			soundPlayer->Stop();
		delete soundPlayer;
		soundPlayer = NULL;

		if (mediaFile != NULL && track != NULL)
			mediaFile->ReleaseTrack(track);
		track = NULL;
		delete mediaFile; // must go before hlsIo/httpIo - it reads from them
		mediaFile = NULL;
#ifdef RWORLDRADIO_EMBEDDED_MP3
		// Decoders read through sniffIo, which reads httpIo: tear down in
		// that order so nothing is left reading a freed source.
		delete mp3Decoder;
		mp3Decoder = NULL;
		delete aacDecoder;
		aacDecoder = NULL;
		delete sniffIo;
		sniffIo = NULL;
#endif
		delete hlsIo;
		hlsIo = NULL;
		delete httpIo;
		httpIo = NULL;
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

#ifdef RWORLDRADIO_EMBEDDED_MP3
	// No Media Kit decoder plugins in this image (arm64 bootstrap): decode
	// the stream in-process with the embedded MP3 decoder instead of handing
	// it to BMediaFile, which would return B_MEDIA_NO_HANDLER. HttpAudioIO is
	// the byte source for both http:// and https:// (its BUrlRequest fallback
	// handles plain http here; https needs OpenSSL, which this build lacks).
	// HLS is left to the BMediaFile path below - its playlist/segment handling
	// isn't covered by this decoder.
	if (!IsHlsUrl(streamUrl)) {
		session->httpIo = new HttpAudioIO(streamUrl);
		status_t openErr = session->httpIo->Open();
		if (openErr != B_OK) {
			std::string reason = session->httpIo->InitError();
			char detail[220];
			snprintf(detail, sizeof(detail), "could not open stream: %s (0x%08lx)%s%s",
				strerror(openErr), (long)openErr,
				reason.empty() ? "" : " - ", reason.c_str());
			delete session->httpIo;
			session->httpIo = NULL;
			if (IsCurrent(generation))
				EmitStatus(kError, station.name, detail);
			return;
		}

		// Pick the decoder by looking at the stream's first bytes rather than
		// trusting the URL or a Content-Type: stations mislabel both, and the
		// two formats are cheap to tell apart (see StreamSniffer). The bytes
		// consumed while sniffing are replayed through sniffIo so whichever
		// decoder runs still sees the stream from the beginning.
		std::string prefix;
		StreamSniffer::Format detected
			= StreamSniffer::Sniff(session->httpIo, prefix);
		session->sniffIo = new PrefixedDataIO(prefix, session->httpIo);

		status_t decErr = B_ERROR;
		if (detected == StreamSniffer::kAacAdts) {
			session->aacDecoder = new AacStreamDecoder(session->sniffIo);
			decErr = session->aacDecoder->Open();
			if (decErr != B_OK) {
				delete session->aacDecoder;
				session->aacDecoder = NULL;
			}
		} else if (detected == StreamSniffer::kMp3) {
			session->mp3Decoder = new Mp3StreamDecoder(session->sniffIo);
			decErr = session->mp3Decoder->Open();
			if (decErr != B_OK) {
				delete session->mp3Decoder;
				session->mp3Decoder = NULL;
			}
		}

		if (decErr != B_OK) {
			if (IsCurrent(generation)) {
				EmitStatus(kError, station.name, detected == StreamSniffer::kUnknown
					? "unrecognised audio format (embedded decoders handle "
						"MP3 and AAC)"
					: "could not decode audio stream");
			}
			return;
		}

		if (!IsCurrent(generation))
			return;

		media_raw_audio_format format = session->aacDecoder != NULL
			? session->aacDecoder->Format() : session->mp3Decoder->Format();
		BSoundPlayer* player = new BSoundPlayer(&format, station.name.c_str(),
			&RadioPlayer::PlayBufferProc, NULL, session.Get());
		if (player->InitCheck() != B_OK) {
			delete player;
			if (IsCurrent(generation))
				EmitStatus(kError, station.name, "could not open audio output");
			return;
		}
		session->soundPlayer = player;

		if (!IsCurrent(generation))
			return; // superseded while buffering; let it be torn down

		player->SetVolume(1.0);
		player->Start();
		player->SetHasData(true);
		EmitStatus(kPlaying, station.name, "");
		return;
	}
#endif // RWORLDRADIO_EMBEDDED_MP3

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
			std::string reason = session->hlsIo->InitError();
			char detail[220];
			snprintf(detail, sizeof(detail), "could not open HLS stream: %s (0x%08lx)%s%s",
				strerror(openErr), (long)openErr,
				reason.empty() ? "" : " - ", reason.c_str());
			delete session->hlsIo;
			session->hlsIo = NULL;
			if (IsCurrent(generation))
				EmitStatus(kError, station.name, detail);
			return;
		}
		session->mediaFile = new BMediaFile(session->hlsIo);
	} else if (IsHttpsUrl(streamUrl)) {
		session->httpIo = new HttpAudioIO(streamUrl);
		// Same reasoning as the HLS branch above - BMediaFile(BDataIO*)
		// never Opens() an arbitrary source itself, so that (which starts
		// the worker thread and blocks until the first bytes arrive or the
		// request fails) has to happen explicitly here.
		status_t openErr = session->httpIo->Open();
		if (openErr != B_OK) {
			std::string reason = session->httpIo->InitError();
			char detail[220];
			snprintf(detail, sizeof(detail), "could not open stream: %s (0x%08lx)%s%s",
				strerror(openErr), (long)openErr,
				reason.empty() ? "" : " - ", reason.c_str());
			delete session->httpIo;
			session->httpIo = NULL;
			if (IsCurrent(generation))
				EmitStatus(kError, station.name, detail);
			return;
		}
		session->mediaFile = new BMediaFile(session->httpIo);
	} else {
		// Some Haiku SDKs (the legacy x86/gcc2 secondary arch) declare BOTH
		// BUrl(const char*, bool = true) and BUrl(const char*), making a
		// single-argument call ambiguous - others (the primary x86_64/gcc13
		// SDK) only have the single-argument form, where a second bool
		// argument is a hard error. HAIKU_BURL_HAS_BOOL_CTOR is set by the
		// Makefile per-SDK - see its comment.
#ifdef HAIKU_BURL_HAS_BOOL_CTOR
		BUrl url(streamUrl.c_str(), true);
#else
		BUrl url(streamUrl.c_str());
#endif
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

#ifdef RWORLDRADIO_EMBEDDED_MP3
	bool haveSource = session->mp3Decoder != NULL || session->aacDecoder != NULL
		|| session->track != NULL;
#else
	bool haveSource = session->track != NULL;
#endif
	if (frameSize == 0 || !haveSource) {
		memset(buffer, 0, size);
		atomic_set(&session->levelBits, FloatToBits(0.0f));
		return;
	}

	size_t producedBytes;
#ifdef RWORLDRADIO_EMBEDDED_MP3
	if (session->mp3Decoder != NULL || session->aacDecoder != NULL) {
		// Embedded decoders yield interleaved 16-bit PCM straight into buffer;
		// clamp to whole frames so a short final read can't split a frame.
		size_t got = session->aacDecoder != NULL
			? session->aacDecoder->Read(buffer, size)
			: session->mp3Decoder->Read(buffer, size);
		producedBytes = (got / frameSize) * frameSize;
	} else
#endif
	{
		int64 frameCount = static_cast<int64>(size / frameSize);
		status_t err = session->track->ReadFrames(buffer, &frameCount);
		producedBytes = err == B_OK
			? static_cast<size_t>(frameCount) * frameSize : 0;
	}

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
