// Host-bridge media engine implementation (M-A audio-only). See header.
//
// Threading: every method here runs on the ENGINE thread (HTMLMediaElement
// lives there). C->JS control crosses to the browser main thread via
// MAIN_THREAD_ASYNC_EM_ASM (host elements must live there); JS->C events
// arrive through the bib_media_event export, which self-proxies back onto
// the engine thread (W-B1 marshaling) before touching any player state —
// so all player fields are engine-thread-only, no locks.
//
// State model: the host element is the source of truth for the CLOCK
// (currentTime/duration/buffered, cached here on events at timeupdate
// cadence); the guest element is the source of truth for INTENT
// (play/pause/seek/volume, forwarded fire-and-forget). paused() flips
// optimistically on play()/pause() so HTMLMediaElement's synchronous
// paused-flag semantics hold; the host's playing/pause events reconcile.

#include "config.h"

#include "BibMediaPlayer.h"

#include "ContentType.h"
#include "DestinationColorSpace.h"
#include "MediaPlayerPrivate.h"
#include "PlatformTimeRanges.h"
#include <cmath>
#include <cstring>
#include <emscripten.h>
#include <emscripten/threading.h>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace BIB {

bool g_mediaEnabled = false;

using namespace WebCore;

// Event codes shared with web/browser.html's bibMediaWire() — keep in sync.
enum BibMediaEvent {
    EvLoadedMetadata = 1, // a = duration
    EvCanPlay = 2,
    EvCanPlayThrough = 3,
    EvPlaying = 4,
    EvPause = 5,
    EvTimeUpdate = 6, // a = currentTime
    EvEnded = 7, // a = currentTime
    EvError = 8, // a = MediaError.code (1 aborted, 2 network, 3 decode, 4 src)
    EvDurationChange = 9, // a = duration
    EvProgress = 10, // a = buffered end
    EvSeeked = 11, // a = currentTime
};

class BibMediaPlayer;
static HashMap<int, BibMediaPlayer*>& playerRegistry()
{
    static NeverDestroyed<HashMap<int, BibMediaPlayer*>> registry;
    return registry;
}

// Host canPlayType answers, cached per full content-type string: the query
// is a SYNC proxy hop to the main thread (bibHTML pattern) and supportsType
// runs for every <source> candidate.
static int hostCanPlayType(const String& contentType)
{
    static NeverDestroyed<HashMap<String, int>> cache;
    return cache.get().ensure(contentType, [&] {
        CString utf8 = contentType.utf8();
        return MAIN_THREAD_EM_ASM_INT({
            return (typeof Module !== "undefined" && Module && Module.bibMediaCanPlay)
                ? Module.bibMediaCanPlay(UTF8ToString($0)) : 0;
        }, utf8.data());
    }).iterator->value;
}

class BibMediaPlayer final
    : public MediaPlayerPrivateInterface
    , public CanMakeWeakPtr<BibMediaPlayer>
    , public RefCounted<BibMediaPlayer> {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(BibMediaPlayer);

public:
    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    explicit BibMediaPlayer(MediaPlayer& player)
        : m_player(player)
        , m_id(++s_nextID)
    {
        playerRegistry().set(m_id, this);
        MAIN_THREAD_ASYNC_EM_ASM({
            if (Module.bibMediaCreate) Module.bibMediaCreate($0);
        }, m_id);
    }

    ~BibMediaPlayer()
    {
        playerRegistry().remove(m_id);
        MAIN_THREAD_ASYNC_EM_ASM({
            if (Module.bibMediaDestroy) Module.bibMediaDestroy($0);
        }, m_id);
    }

    // EXTERNAL_HOLEPUNCH is off in this build, so nothing downcasts on this
    // value — reusing it avoids patching the MediaPlayerType enum.
    constexpr MediaPlayerType mediaPlayerType() const final { return MediaPlayerType::HolePunch; }

    void load(const String& url) final
    {
        resetResourceState(); // src change must not bleed prior state (Codex)
        m_networkState = MediaPlayer::NetworkState::Loading;
        CString utf8 = url.utf8();
        char* copy = strdup(utf8.data());
        if (copy) {
            // Async cross-heap string: freed main-thread-side, the
            // bibPushFrameIfDirty protocol.
            MAIN_THREAD_ASYNC_EM_ASM({
                if (typeof growMemViews === "function") growMemViews();
                var url = UTF8ToString($1);
                _bib_wasm_free($1);
                if (Module.bibMediaLoad) Module.bibMediaLoad($0, url);
            }, m_id, copy);
        } else {
            // No host load was queued — permanent Loading would wedge the
            // element; fail like an unreachable resource (Codex).
            m_networkState = MediaPlayer::NetworkState::NetworkError;
        }
        if (auto player = m_player.get())
            player->networkStateChanged();
    }
#if ENABLE(MEDIA_SOURCE)
    void load(const URL&, const LoadOptions&, MediaSourcePrivateClient&) final { } // M-C
#endif
#if ENABLE(MEDIA_STREAM)
    void load(MediaStreamPrivate&) final { }
#endif
    void cancelLoad() final
    {
        resetResourceState();
        ctl(7);
    }

    void play() final
    {
        m_paused = false;
        m_ended = false;
        ctl(1);
    }
    void pause() final
    {
        m_paused = true;
        ctl(2);
    }
    bool paused() const final { return m_paused; }

    void seekToTarget(const SeekTarget& target) final
    {
        m_seeking = true;
        double t = target.time.toDouble();
        m_currentTime = t;
        ctl(3, t);
    }
    bool seeking() const final { return m_seeking; }

    void setVolumeDouble(double volume) final { ctl(4, volume); }
    void setMuted(bool muted) final { ctl(5, muted ? 1.0 : 0.0); }
    void setRateDouble(double rate) final { ctl(6, rate); }

    MediaTime duration() const final { return toMediaTime(m_duration); }
    MediaTime currentTime() const final { return toMediaTime(m_currentTime); }
    MediaTime maxTimeSeekable() const final { return duration(); }

    FloatSize naturalSize() const final { return { }; }
    bool hasVideo() const final { return false; }
    bool hasAudio() const final { return true; }
    bool ended() const final { return m_ended; }

    void setPageIsVisible(bool) final { }

    MediaPlayer::NetworkState networkState() const final { return m_networkState; }
    MediaPlayer::ReadyState readyState() const final { return m_readyState; }

    const PlatformTimeRanges& buffered() const final { return m_buffered; }
    bool didLoadingProgress() const final { return std::exchange(m_didProgress, false); }

    void paint(GraphicsContext&, const FloatRect&) final { }
    DestinationColorSpace colorSpace() final { return DestinationColorSpace::SRGB(); }

    void handleEvent(int code, double a, double b)
    {
        auto player = m_player.get();
        switch (code) {
        case EvLoadedMetadata:
            m_duration = a;
            m_readyState = MediaPlayer::ReadyState::HaveMetadata;
            if (player) {
                player->durationChanged();
                player->readyStateChanged();
                player->characteristicChanged();
            }
            break;
        case EvCanPlay:
            // Leave Loading here too: hosts may never fire canplaythrough
            // (streams), which would strand the guest in NETWORK_LOADING
            // (Codex). The host element owns the actual fetch state anyway.
            if (m_networkState == MediaPlayer::NetworkState::Loading)
                m_networkState = MediaPlayer::NetworkState::Idle;
            if (m_readyState < MediaPlayer::ReadyState::HaveFutureData)
                m_readyState = MediaPlayer::ReadyState::HaveFutureData;
            if (player) {
                player->readyStateChanged();
                player->networkStateChanged();
            }
            break;
        case EvCanPlayThrough:
            m_readyState = MediaPlayer::ReadyState::HaveEnoughData;
            if (m_networkState == MediaPlayer::NetworkState::Loading)
                m_networkState = MediaPlayer::NetworkState::Idle;
            if (player) {
                player->readyStateChanged();
                player->networkStateChanged();
            }
            break;
        case EvPlaying:
            m_paused = false;
            if (player)
                player->playbackStateChanged();
            break;
        case EvPause:
            m_paused = true;
            if (player)
                player->playbackStateChanged();
            break;
        case EvTimeUpdate:
            // Cache only: HTMLMediaElement polls currentTime on its own
            // timer; timeChanged() is reserved for discontinuities.
            m_currentTime = a;
            m_didProgress = true;
            break;
        case EvEnded:
            m_currentTime = a;
            m_ended = true;
            m_paused = true;
            if (player)
                player->timeChanged(); // element checks ended() here
            break;
        case EvError:
            m_networkState = a == 3 ? MediaPlayer::NetworkState::DecodeError
                : a == 4 ? MediaPlayer::NetworkState::FormatError
                         : MediaPlayer::NetworkState::NetworkError;
            if (player)
                player->networkStateChanged();
            break;
        case EvDurationChange:
            m_duration = a;
            if (player)
                player->durationChanged();
            break;
        case EvProgress:
            m_buffered = PlatformTimeRanges { MediaTime::zeroTime(), toMediaTime(a) };
            m_didProgress = true;
            break;
        case EvSeeked:
            m_seeking = false;
            m_currentTime = a;
            if (player)
                player->timeChanged();
            break;
        default:
            (void)b;
            break;
        }
    }

private:
    // Per-resource state must not bleed across src changes (Codex).
    void resetResourceState()
    {
        m_networkState = MediaPlayer::NetworkState::Empty;
        m_readyState = MediaPlayer::ReadyState::HaveNothing;
        m_duration = 0;
        m_currentTime = 0;
        m_paused = true;
        m_seeking = false;
        m_ended = false;
        m_didProgress = false;
        m_buffered = PlatformTimeRanges { };
    }

    // op codes shared with browser.html bibMediaCtl: 1 play, 2 pause,
    // 3 seek(a), 4 volume(a), 5 muted(a), 6 rate(a), 7 unload.
    void ctl(int op, double a = 0)
    {
        MAIN_THREAD_ASYNC_EM_ASM({
            if (Module.bibMediaCtl) Module.bibMediaCtl($0, $1, $2);
        }, m_id, op, a);
    }

    static MediaTime toMediaTime(double seconds)
    {
        if (std::isnan(seconds))
            return MediaTime::zeroTime();
        if (std::isinf(seconds))
            return MediaTime::positiveInfiniteTime();
        return MediaTime::createWithDouble(seconds);
    }

    ThreadSafeWeakPtr<MediaPlayer> m_player;
    int m_id { 0 };
    static int s_nextID;

    MediaPlayer::NetworkState m_networkState { MediaPlayer::NetworkState::Empty };
    MediaPlayer::ReadyState m_readyState { MediaPlayer::ReadyState::HaveNothing };
    double m_duration { 0 };
    double m_currentTime { 0 };
    bool m_paused { true };
    bool m_seeking { false };
    bool m_ended { false };
    mutable bool m_didProgress { false };
    PlatformTimeRanges m_buffered;
};

int BibMediaPlayer::s_nextID = 0;

class BibMediaPlayerFactory final : public MediaPlayerFactory {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(BibMediaPlayerFactory);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(BibMediaPlayerFactory);

private:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::HolePunch; }

    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final
    {
        return adoptRef(*new BibMediaPlayer(player));
    }

    void getSupportedTypes(HashSet<String>& types) const final
    {
        // Candidate inventory filtered by the HOST's truthful answers.
        static constexpr ASCIILiteral candidates[] = {
            "audio/mpeg"_s, "audio/mp4"_s, "audio/aac"_s, "audio/ogg"_s,
            "audio/wav"_s, "audio/x-wav"_s, "audio/webm"_s, "audio/flac"_s,
        };
        for (auto& mime : candidates) {
            if (hostCanPlayType(mime))
                types.add(mime);
        }
    }

    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final
    {
        if (parameters.isMediaSource || parameters.isMediaStream)
            return MediaPlayer::SupportsType::IsNotSupported;
        // Audio-only v1: video containers keep their A2 zero-engine
        // behavior until M-B (overlay presentation).
        if (!parameters.type.containerType().startsWith("audio/"_s))
            return MediaPlayer::SupportsType::IsNotSupported;
        switch (hostCanPlayType(parameters.type.raw())) {
        case 2:
            return MediaPlayer::SupportsType::IsSupported;
        case 1:
            return MediaPlayer::SupportsType::MayBeSupported;
        default:
            return MediaPlayer::SupportsType::IsNotSupported;
        }
    }
};

void registerBibMediaEngine(MediaEngineRegistrar registrar)
{
    if (!g_mediaEnabled)
        return;
    registrar(makeUnique<BibMediaPlayerFactory>());
}

} // namespace BIB

// JS->C event channel: the host element's listeners call this from the
// BROWSER MAIN thread; self-proxy to the engine thread before touching the
// registry (which is engine-thread-only).
extern "C" {

struct BibMediaEventTask {
    int id;
    int code;
    double a;
    double b;
};

static void bibRunMediaEvent(void* p)
{
    auto* task = static_cast<BibMediaEventTask*>(p);
    if (auto* player = BIB::playerRegistry().get(task->id))
        player->handleEvent(task->code, task->a, task->b);
    delete task;
}

EMSCRIPTEN_KEEPALIVE void bib_media_event(int id, int code, double a, double b)
{
    if (BIB::onEngineThread()) {
        if (auto* player = BIB::playerRegistry().get(id))
            player->handleEvent(code, a, b);
        return;
    }
    auto* task = new BibMediaEventTask { id, code, a, b };
    if (!BIB::proxyToEngine(bibRunMediaEvent, task))
        delete task;
}

} // extern "C"
