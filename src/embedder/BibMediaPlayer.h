// Host-bridge media engine (M-A, audio-only v1).
//
// Tier A2 (ENABLE_VIDEO=ON, zero engines) gave the DOM real <audio>/<video>
// elements with spec-correct FAILURE — canPlayType "", play() rejects.
// This engine makes audio actually play by mirroring state to a host-page
// Audio() element (docs/summaries/analysis-media-playback.md): the host
// does fetch + decode + clock, the engine does DOM semantics + events.
//
// Registration is called from MediaPlayer.cpp buildMediaEnginesVector()
// behind #if defined(__EMSCRIPTEN__) (the ONE WebKit-tree hunk of this
// feature) and runtime-gated: without Module.bibMedia (?media=1) nothing
// registers and A2 behavior is unchanged. canPlayType answers flip from ""
// to the HOST browser's truthful answers only behind the flag — sites
// gating features on canPlayType get untested paths otherwise (scoping doc
// risk #1).
//
// WISP INVARIANT EXCEPTION (flagged, accepted in scoping): the host
// element fetches the media URL DIRECTLY from the host browser, bypassing
// wisp and guest cookies. Progressive audio only; MSE mirroring (M-C)
// keeps the invariant when it lands.

#pragma once

#include "MediaPlayer.h"

namespace BIB {

// Set once in main() from Module.bibMedia before the first MediaPlayer is
// constructed (buildMediaEnginesVector caches its result for the session).
extern bool g_mediaEnabled;

void registerBibMediaEngine(WebCore::MediaEngineRegistrar);

// Cross-thread helpers defined in main.cpp (W-B1 marshaling block) —
// shared so BibMediaPlayer.cpp's bib_media_event export can self-proxy.
bool onEngineThread();
bool proxyToEngine(void (*task)(void*), void* arg);

} // namespace BIB
