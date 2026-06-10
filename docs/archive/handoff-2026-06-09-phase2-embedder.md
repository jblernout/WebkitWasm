# Handoff — Phase 2: embedder + first paint (the ONE active handoff)

**Written**: 2026-06-09 23:00 EDT, after WebCore compiled.
**Supersedes**: handoff-2026-06-09-phase2-port-skeleton.md (→ docs/archive/).

## State (all committed through `b1a41dc`, tree clean)
- **libWebCore.a (17 MB) COMPILES** under PORT=Emscripten, alongside
  libJavaScriptCore/libWTF/libPAL/libSkia in `build/webcore/lib/`.
- Full sysroot at `third_party/wasm-sysroot`: 15 static libs (ICU 77.1,
  zlib, png, jpeg, webp, freetype, harfbuzz, libxml2, sqlite3, OpenSSL
  3.5.0, nghttp2, brotli, libpsl, curl 8.17, fontconfig 2.15) + pkgconfig
  (incl. zlib.pc shim, libpsl.pc Requires.private: icu-uc) + `etc/fonts/`
  (fontconfig runtime config, staged for wasm FS packaging).
- Patch ledger `src/patches/webkit-emscripten.patch`: 640 lines. Codex
  review 2026-06-09: 0 critical / 0 high; 3 findings fixed (`b1a41dc`).
- Build scripts: `tools/build-webcore.sh` (resumable; ninja -k 50),
  `tools/build-deps/curl-tier.sh`, `tools/build-deps/webcore-deps.sh`.

## NEXT TASK: the embedder (make-or-break gate)
Goal: `WebCore::Page` renders `<h1>hello</h1>` to pixels, blitted to the
host `<canvas>`. Plan of attack:
1. **Scaffold `src/embedder/`** (our code, not WebKit tree): main.cpp that
   builds a `WebCore::PageConfiguration` and `Page`. START FROM
   `Source/WebCore/loader/EmptyClients.h` —
   `pageConfigurationWithEmptyClients()` is how SVGImage builds an
   embedded page internally; it supplies every client stub. Then override
   only what first paint needs (ChromeClient for invalidation, maybe).
   Cross-reference PlayStation's thin embedder under
   `Source/WebKitLegacy/playstation/` (if present in tree) for the
   Page+LocalFrame+load+layout sequence.
2. **Load + paint offscreen first** (no canvas yet): construct Page →
   LocalFrame → `loadHTMLString`-equivalent (FrameLoader::load with a
   data: substitute or DocumentLoader substitute data) → force layout →
   paint via WebCore GraphicsContext backed by an SkSurface (CPU raster)
   → dump raw RGBA/PNG. Verify under node before touching the browser.
3. **Expect an undefined-symbol batch at link.** Known deferred:
   - `CryptoAlgorithm*` (WebCrypto sources excluded — see
     PlatformEmscripten.cmake comment; either stub or BoringSSL swap).
   - `AXObjectCache::attachWrapper`/`detachWrapper`/`detachPlatformWrapper`
     etc. (no accessibility/emscripten/*.cpp yet — crib
     accessibility/playstation/AXObjectCachePlayStation.cpp, ~50 lines).
   - Probably theme/scrollbar/platform-strategy functions (RenderTheme,
     PlatformStrategies, pasteboard, cursors). Resolve by cribbing
     PlayStation/generic stubs into PlatformEmscripten.cmake sources or
     the embedder. Batch-collect with `-k 50`-style iteration: link, list
     undefineds, stub, repeat.
4. **Link flags** (from playbook / Phase 1): -sSTACK_SIZE=8MB,
   -sDEFAULT_PTHREAD_STACK_SIZE=2MB, -sINITIAL_MEMORY=128MB (likely more),
   -sALLOW_MEMORY_GROWTH=1, -sMAXIMUM_MEMORY=4GB, -pthread, pool sized.
5. **FONTS ARE LOAD-BEARING**: text paints nothing without a real font.
   Package into the wasm FS: `/etc/fonts` (from sysroot/etc/fonts), a TTF
   (e.g. DejaVuSans) at `/usr/share/fonts/`, ICU .dat at its compiled-in
   path. fontconfig cache dir `/var/cache/fontconfig` must exist/be
   writable (MEMFS ok).

## Build discipline (cost facts)
- NEVER `rm -rf build/webcore` — full compile ≈ 35 min on 12 cores;
  incremental fix loop ≈ 2–8 min. cmake re-runs itself when port .cmake
  files change. ccache installed but unwired (would force one full rebuild).
- Every WebKit-tree edit → `tools/export-webkit-patches.sh`.

## Deferred / open
- WebCrypto (Phase 4): patch 4 files vs OpenSSL 3.5 or swap tier to
  BoringSSL — decision-003 documents both routes.
- TextureMapper stayed OFF and WebCore compiled — decision-002's open
  question RESOLVED (CPU raster needs none of it).
- Wisp networking, cookies/cache persistence: Phase 4.

## Standing constraints (do not violate)
- One mutator thread per VM (Thread::suspend traps under wasm).
- ENABLE_SAMPLING_PROFILER stays OFF. wasm32 only (no Memory64).
- Local git only. Codex review before presenting non-trivial code.
- Background Bash: `cd` to project root by absolute path first.
- Memory playbook: ~/.claude/.../memory/webkit-wasm-porting-playbook.md.
