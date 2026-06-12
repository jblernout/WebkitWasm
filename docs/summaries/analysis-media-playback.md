# Media playback scoping pass: host-bridge MediaPlayerPrivate

Date: 2026-06-12 (task #65). Analysis only — no build flip. Modeled on
the #40 / #53 / #63 scoping passes.

## Problem

Tier A2 (`ENABLE_VIDEO=ON`, zero engines, commit f1592cc) gave us REAL
`<video>`/`<audio>` elements with spec-correct *failure*: `canPlayType`
returns "", `play()` rejects NotSupportedError, error event code 4.
Sites stopped crashing and misdetecting — but nothing plays. "Videos
don't error but don't load" is now the most visible site-support gap
(news sites, YouTube, Twitter video, Discord sounds).

Decoding media IN wasm (ffmpeg-class) is rejected out of hand: huge
binary, huge CPU, and the host browser three feet away already has
every codec, hardware-accelerated. The obvious move is a **bridge
media engine**: an embedder-side `MediaPlayerPrivate` that mirrors
state to a HOST-side `<audio>`/`<video>` element. Host does fetch +
decode + clock; engine does DOM semantics + events + geometry.

## What upstream already gives us

- **`MediaPlayerPrivateHolePunch`** (platform/graphics/holepunch/,
  **289 lines total**) is the exact minimal-surface precedent: a player
  that decodes nothing, paints a transparent punch-through rect, and
  reports just enough state for WebCore to build the element. WPE uses
  it when an external pipeline presents the video. Our bridge = this
  skeleton + real state mirroring to a host element.
- **Registration**: `MediaPlayer.cpp buildMediaEnginesVector()` is
  compile-time per-engine; one `#if defined(__EMSCRIPTEN__)` hunk
  calling `BIB::registerBibMediaEngine(addMediaEngine)` (factory makes
  our player; `supportsTypeAndCodecs` consults the HOST's real
  `canPlayType` via JS — truthful per-browser answers for free).
- The interface is big (173 virtuals) but the pure-virtual core is
  ~25 methods (load/play/pause/seek/states/buffered/naturalSize/paint/
  …) and every one has a 1:1 host-element equivalent. Mechanical.
  Estimate: ~500 lines embedder C++ + ~200 lines browser.html JS
  (player registry keyed by ID, C→JS via EM_ASM, JS→C events via
  ccall dispatch — same shape as the GPU/present plumbing).

## The hard part is not the interface — it's two architecture tensions

### 1. The wisp invariant
"All networking leaves the tab over Wisp" (CLAUDE.md hard line). A host
`<video src=URL>` fetches DIRECTLY from the host browser — bypassing
wisp, guest cookies, and the privacy model.
- **MSE sites (YouTube-class) don't have this problem**: the SITE
  fetches segments through the GUEST network stack (wisp, cookies
  intact) and pushes bytes via `SourceBuffer.appendBuffer` — we mirror
  those bytes to a host `MediaSource`. The invariant holds end-to-end.
- **Progressive src does**: options are (a) accept direct host fetch
  for media only, flagged and documented (`?media=host-fetch`); (b)
  pump bytes guest-curl→host-`MediaSource` — but MSE requires
  fragmented MP4/WebM, so plain progressive MP4s need a REMUX step
  (mp4box-class JS, real work, codec-dependent). Recommendation:
  ship (a) behind the flag first, scope (b) only if a load-bearing
  auth'd-media site appears.

### 2. Presentation (video only)
- **M-B overlay (hole-punch)**: engine paints transparent; host
  positions the real `<video>` over the canvas. Geometry comes from
  `paint(GraphicsContext&, FloatRect)` — extract device-space rect from
  the CTM at paint time, push to host on change. Dirty-rect repaints on
  scroll already re-invoke paint, so the overlay follows scroll. Known
  v1 jank: z-order (guest popups render UNDER the overlay), CSS
  transforms/clips ignored, fullscreen needs special-casing. Accepted.
- **M-C composite (correct but costly)**: host `requestVideoFrameCallback`
  → ImageBitmap → pixels into wasm heap → Skia composites. Perfect
  z-order; ~105 MB/s memcpy at 720p30. Defer; only if overlay jank
  blocks a real site.
- **Audio has neither problem** — no paint, no rect. The bridge is
  nearly free once state mirroring exists.

## Interaction with W-B (pthread)

Build M-* AFTER W-B1 or design for it: every C→JS hook must use the
MAIN_THREAD_EM_ASM/proxy helpers from day one (W-B0 proved worker
Modules don't inherit page hooks). Host `<video>` element lives on the
main thread either way — the bridge is naturally main-thread-targeted,
so W-B makes it *cleaner*, not harder.

## Risks

- **canPlayType flips from "" to truthful** — sites that today degrade
  gracefully (Discord's "Unsupported browser, skipping libdiscore"
  class of probes is fine, but some sites gate FEATURES on canPlayType)
  will start exercising untested paths. Mitigation: flag-gate
  (`?media=1`) until a sweep shows no regressions, keep A2 zero-engine
  as the default until then.
- DRM/EME sites (Netflix-class): out of scope forever, fail as today.
- WebRTC (Discord voice): out of scope, separate epic, not this.
- Autoplay policies apply to the HOST element (user-gesture state lives
  in the guest) — host `play()` may reject NotAllowedError; surface it
  as the spec equivalent on the guest element; harness runs use
  `--autoplay-policy=no-user-gesture-required`.

## Recommendation: GO, phased — confidence: medium-high

- **M-0 (spike, ~half session)**: probe page outside WebKit — host
  Audio()/`<video>` controlled from wasm via EM_ASM; overlay rect math
  from a fake paint CTM; host canPlayType inventory. Kill criteria:
  none expected (all host-standard APIs) — this spike is for plumbing
  shape, not feasibility.
- **M-A (audio bridge, ~1 session)**: register bridge for audio MIME
  types only; direct host fetch flagged. Acceptance: mp3 `<audio>` page
  plays + Discord notification sound path constructs without error.
- **M-B (video overlay, 1-2 sessions)**: hole-punch paint + overlay
  positioning. Acceptance: tekeye video test page (the A2 acceptance
  page) actually plays inline; html5test media section.
- **M-C (MSE mirror, scope later)**: SourceBuffer byte mirroring for
  YouTube-class sites. Re-scope after M-B ships; depends on demand.

Ordering vs the rest of the queue: **W-B1 stays first** (it unblocks
every site, media included — a playing video under a frozen tab is
worthless). M-0/M-A is the natural session AFTER W-B1 lands, or a
filler while W-B1 builds run.
