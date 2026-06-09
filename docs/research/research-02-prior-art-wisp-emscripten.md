# Research 02 — Prior Art, Wisp/Mercury Stack, Emscripten 2026, Size Envelope

**Date**: 2026-06-09 (all URLs accessed this date) · **Scope**: web research for Phase 0
**Companion**: research on WebKit port surface is tracked separately (research-01).

---

## 1. PRIOR ART AUTOPSIES

### 1.1 mbbill/JSC.js — JavaScriptCore on WebAssembly

Repo: <https://github.com/mbbill/JSC.js> (484 stars, created 2017-09-15, **last push 2021-10-12**, not archived; last non-README commit 2020-05-20 — per GitHub repos/commits API, accessed 2026-06-09).

- **WebKit version**: vendored JavaScriptCore + WTF snapshot; commit 2019-05-10 says *"Updated JavaScriptCore to Apr.6 2019"* — so the engine is a **WebKit trunk snapshot of 2019-04-06** (<https://github.com/mbbill/JSC.js/commits/master>).
- **Build config**: **CLoop confirmed**. GitHub code search in the repo shows `Source/JavaScriptCore/offlineasm_cloop/` (a pre-run offlineasm CLoop generator), `interpreter/CLoopStack.cpp`, `llint/LowLevelInterpreter.cpp` — i.e. LLInt in C-loop mode, no JIT. Build is **gn + ninja** (`build/BUILD.gn`, `buildtools/`), *not* WebKit's CMake — the author rebuilt the build system from scratch and pre-generated derived sources (commit *"Added missing derived sources"*, 2019-05-09).
- **Binary size**: README: *"The size of JSC.wasm is around 4MB (compressed js and mem file)."* Demo: <https://mbbill.github.io/JSC.js/demo/index.html>.
- **Patches needed** (from commit log): pre-generated DerivedSources checked into the tree; suppression of `BINARYEN_TRAP_MODE` after the LLVM wasm backend switch; fp-conversion warning workarounds; Windows `prep_env.bat` env shims (commits of 2020-05-16/20).
- **Lesson**: JSC (CLoop) + WTF compiles and runs under Emscripten with modest patching, but the author sidestepped WebKit's build system entirely (gn rewrite + checked-in derived sources) — that approach froze the tree at one 2019 snapshot and the project went dormant. For us: keep WebKit's CMake (JSCOnly port) instead of a parallel build system, or accept the same freeze.

### 1.2 trevorlinton/webkit.js (2014) — partial WebCore port

Repo: <https://github.com/trevorlinton/webkit.js> (1,966 stars, created 2014-01-16, **last push 2018-12-30**; README accessed via GitHub API 2026-06-09).

- **How far it got** (README "DONE" items): WebCore + WTF compiled via Emscripten (asm.js era, pre-wasm); HTML5/CSS3/SVG rendering to WebGL/Canvas; fonts via **freetype2 + fontconfig + cairo**; libjpeg-turbo + zlib integrated; hidpi; a JS API generated from WebCore C++ interfaces; LTO/outlining experiments to dodge code-size limits. It really did paint pages.
- **What never landed** (README "Current Issues"/"In Progress"): **no resource loader (networking) ever implemented**; no mouse/keyboard events; inline-SVG and CSS-keyframe segfaults attributed to threading (no real threads in 2014 JS); demo *"runs best in Firefox (Chrome/Safari … freezes due to garbage collection)"*.
- **Why it stalled** (evidence-based): single maintainer; asm.js-era platform gaps (no SharedArrayBuffer/pthreads, no wasm, heap-size ceilings, 16 GB disk + bespoke toolchain to build); the hard 20% (network, input, events) remained when activity stopped ~2015; a vendored full-WebKit fork rotted. Notably the README records *"Investigate if SKIA might provide better rendering — no, skia does not support accelerated back-tile compositing needed by WebKit"* — **obsolete today**: WebKitGTK 2.46 shipped Skia as the default rendering backend, replacing cairo (<https://webkitgtk.org/2024/10/04/webkitgtk-2.46.html>, 2024-10-04).
- **Lesson**: first-paint is provably achievable even with 2014 tooling; networking + input is where it died. Our Phase 3/4 ordering targets exactly that gap, and 2026 tooling (pthreads, wasm, OPFS, Wisp) removes the era blockers webkit.js hit.

### 1.3 2020–2026 attempts at engine-to-wasm

Searched 2026-06-09: GitHub repo search (`browser engine webassembly emscripten`, `webcore wasm`, `webkit wasm port`, `netsurf wasm`, `servo webassembly`), GitHub issue search in LadybirdBrowser/ladybird, plus general web search ("WebKit WebAssembly port", "Ladybird wasm", "Servo wasm browser").

- **No shipped or even seriously-attempted WebCore/Blink/Gecko-on-wasm project exists in 2020–2026.** GitHub searches return only game engines and toys; `webcore wasm` and `webkit wasm port` return zero relevant repos (GitHub search API, 2026-06-09).
- **Servo**: discussion *"Could we compile stylo/servo into web assembly easily?"* (<https://github.com/servo/servo/discussions/28070>) — answer: Stylo (CSS engine) compiles to wasm with a small patch, but SpiderMonkey, GL bindings (WebRender), networking, and platform code block a full-Servo wasm build. No follow-through found.
- **Ladybird**: LibWasm is Ladybird's *own* wasm interpreter (running wasm inside Ladybird), not a port of Ladybird to wasm (<https://github.com/LadybirdBrowser/ladybird>; <https://dzfrias.dev/blog/ladybird-wasm-0/>). Issue search for emscripten/wasm build of Ladybird itself: empty (2026-06-09).
- **NetSurf**: no wasm port found (GitHub search empty, 2026-06-09).
- **Adjacent non-ports**: HeyPuter **browser.js** is Scramjet-based proxy/rewriting, not an engine (<https://github.com/HeyPuter/browser.js>); v86 / QEMU-wasm run real browsers under emulation (our rejected option). litehtml compiles under Emscripten in scattered demos but is not a web-compatible engine (<https://github.com/litehtml/litehtml>).
- **Conclusion**: the field is empty. The living precedents are *component-level*: JSC.js (JS engine), CanvasKit/skwasm (Skia raster/GPU), libcurl.js (HTTP+TLS network stack). Our project is gluing exactly those three proven components to the one unproven one (WebCore platform layer).

### 1.4 Flutter CanvasKit / skwasm — Skia-on-wasm precedent

- **CanvasKit** is Skia's official wasm build: WebGL-backed `SkSurface` drawing into an HTML canvas, hardware accelerated (<https://skia.org/docs/user/modules/canvaskit/>).
- **Sizes** (Flutter docs, <https://docs.flutter.dev/platform-integration/web/renderers>, accessed 2026-06-09): `canvaskit` renderer = *"a copy of Skia compiled to WebAssembly, which adds about **1.5 MB** in download size"* (default build mode); `skwasm` = *"more compact version of Skia … about **1.1 MB**"*. The `canvaskit-wasm` npm package (v0.41.1) is 25.5 MB unpacked across 25 files — that's all variants (full/profiling/debug), not the shipped payload (<https://registry.npmjs.org/canvaskit-wasm/latest>).
- **Threading model**: skwasm renders **on a dedicated web worker thread** when the server meets SharedArrayBuffer (COOP/COEP) requirements, and **silently falls back to single-threaded** when not — graceful degradation, not hard failure (Flutter renderers doc, ibid.).
- **Lessons for us**: (a) a full 2D engine compresses to ~1.5 MB — Skia is not our size problem; WebCore+ICU is; (b) ship feature-trimmed builds (skwasm vs full CanvasKit ≈ 27% smaller); (c) design render threading with single-thread fallback; (d) WebGL-backed Skia surface is the proven Phase 6 GPU path.

---

## 2. WISP PROTOCOL + MERCURY WORKSHOP STACK

### 2.1 Protocol spec

Spec repo: <https://github.com/MercuryWorkshop/wisp-protocol> — v1 at `blob/v1/protocol.md`, v2 at `blob/v2/protocol.md` (both read in raw form 2026-06-09). Authored by @ading2210; current v1 doc is "Version 1.2".

**Framing (v1 and v2, all little-endian)**: every WebSocket binary message is one packet:
`| type: uint8 | stream_id: uint32 | payload… |`

| Type | Name | Payload |
|---|---|---|
| 0x01 | CONNECT | stream type `uint8` (0x01 TCP, 0x02 UDP), port `uint16`, hostname `char[]` |
| 0x02 | DATA | raw stream bytes |
| 0x03 | CONTINUE | buffer remaining `uint32` (flow control) |
| 0x04 | CLOSE | reason `uint8` |
| 0x05 | INFO (v2 only) | major `uint8`, minor `uint8`, extension data |

- **Multiplexing**: client picks a stream_id per CONNECT; all streams share one WebSocket. Client may send DATA immediately after CONNECT without waiting (latency optimization).
- **Flow control**: server-granted packet-count budget per TCP stream via CONTINUE; client decrements per DATA sent; UDP streams have no flow control.
- **TCP vs UDP**: stream type in CONNECT. v1: *"UDP support is mandatory for both the server and the client"*; v2 moves UDP behind **extension 0x01**.
- **CLOSE reasons**: generic 0x01–0x03; server-only 0x41 invalid info, 0x42 unreachable, 0x43 timeout, 0x44 refused, 0x47 TCP transfer timeout, 0x48 host blocked, 0x49 throttled.
- **v2 handshake**: server sends INFO immediately on WS open (version + supported extensions); client replies INFO (with credentials if authenticating); server accepts with CONTINUE (stream 0) or rejects with CLOSE. **Downgrade rule**: if the client receives CONTINUE first, the server is v1. Extensions: 0x01 UDP, 0x02 password auth, 0x03 public/private-key auth, 0x04 MOTD, 0x05 stream-open confirmation.

### 2.2 Server/client implementations — maturity & throughput

Implementations list: wisp-protocol README. Benchmarks: **WispMark** <https://github.com/MercuryWorkshop/wispmark> (current results on Ryzen 9 5950X, README accessed 2026-06-09):

| Server | wisp-js client (10 streams) | wisp-mux Rust client (5×10) |
|---|---|---|
| wisp-js (node) | 1,288 MiB/s | 1,326 MiB/s |
| wisp-server-python | 1,229 MiB/s | **4,676 MiB/s** (multi-process) |
| epoxy-server (Rust, multithread) | 1,514 MiB/s | 4,142 MiB/s |
| go-wisp | 1,589 MiB/s | 3,396 MiB/s |

- **wisp-js** (<https://github.com/MercuryWorkshop/wisp-js>): client+server, **supports Wisp v2 and v1**, browser + Node, npm `@mercuryworkshop/wisp-js` v0.4.1, server CLI included. Last push 2026-03-18. This is the same code embedded in libcurl.js as the `wisp_client` submodule.
- **wisp-server-python** (<https://github.com/MercuryWorkshop/wisp-server-python>): PyPI `wisp-python` v0.9.0; *complete v1 incl. UDP*, rate limits, but README roadmap still lists **"Wisp v2 support" as unfinished**; AGPL-3.0; last push 2025-10-30.
- **epoxy-server** (Rust, in epoxy-tls repo): fastest single-thread, production-grade, actively maintained (repo pushed 2026-06-07).
- **Recommendation**: dev = `wisp-js` server (v2, trivial npx setup); load/perf = `epoxy-server`. Avoid wisp-server-python as primary (v1-only today; all our v2 needs — auth, UDP extension — would be unavailable). Throughput is a non-issue for a browser engine: every implementation exceeds 1 GiB/s, orders of magnitude above page-load needs.

### 2.3 libcurl.js — exact mechanism (the key prior art)

Repo: <https://github.com/ading2210/libcurl.js> (LGPL-3.0-or-later, npm `libcurl.js` v0.7.4). All file references below read via GitHub API 2026-06-09.

**What is compiled** (`client/tools/*.sh`, `client/build.sh`):
- **curl 8.17.0** from a lightly patched fork (`git clone -b 8.17.0-patched https://github.com/ading2210/curl`; the only configure.ac patch removes `pipe2`), built with `emconfigure ./configure --host i686-linux --disable-ipv6 --disable-threaded-resolver --enable-websockets …` and **`--with-mbedtls`** — **the TLS library is mbedTLS** (not OpenSSL/wolfSSL), plus **zlib, brotli, nghttp2 (HTTP/2), cJSON**, all built as wasm static libs by ~20-line scripts in `client/tools/`.
- Link line: `emcc client/libcurl/*.c -lcurl -lmbedtls -lmbedcrypto -lmbedx509 -lcjson -lz -lbrotlidec -lbrotlicommon -lnghttp2 -lwebsocket.js -sENVIRONMENT=worker,web -sALLOW_MEMORY_GROWTH …` with `-Oz -flto` for release (`client/build.sh`). **No pthreads.** Emscripten 3.1.6 and 3.1.72 are the tested compiler versions (README).
- Result: **552 KB compressed** total footprint (README "Features"), ~5.9 MB unpacked npm package (all variants).

**How curl's sockets reach Wisp — the crucial detail**: there is **no C-level Wisp shim at all**. The C code (`client/libcurl/*.c`) is a thin wrapper around vanilla libcurl APIs (multi handle, `curl_ws_send/recv` with `CURLOPT_CONNECT_ONLY=2` for WS). curl calls ordinary BSD `socket()/connect()/send()`; **Emscripten's built-in SOCKFS layer (`-lwebsocket.js`) turns each socket into a WebSocket**. Then, post-link, `client/tools/patch_js.py` applies regex "fragments" to the Emscripten-generated runtime JS:
- `fragments/force_wsproxy.js` — rewrites SOCKFS's WebSocket URL construction to `<proxy-url>/<host>:<port>` (wsproxy-style addressing, so the destination hostname survives; no in-wasm DNS needed).
- `fragments/wisp_support.js` — replaces `ws = new WebSocketConstructor(url, opts)` with `ws = new WispWebSocket(url)` (or any custom transport class) — `WispWebSocket` being the wisp-js client polyfill bundled from the `wisp_client` submodule. One Emscripten socket ⇒ one Wisp stream.
- `fragments/fix_socket_limit.js` — deletes Emscripten's `fd < 64` asserts.

**Reusability verdict**: the bridge is **JS-glue at the Emscripten runtime-library level, not a portable C library** — but that is exactly what makes it reusable for us: any Emscripten-linked binary that uses BSD sockets (i.e. WebKit's curl network backend) gets Wisp transport by (a) linking the same `curl-wasm` static libs and (b) swapping the SOCKFS WebSocket constructor for `WispWebSocket`. The directly liftable assets: `client/tools/{mbedtls,curl,zlib,brotli,nghttp2}.sh` build recipes, the fragment logic (to be re-implemented as a clean `--js-library` override rather than regex-patching emcc output), and the wisp-js client. The C wrapper files are libcurl.js-API-specific and not needed — WebCore's `NetworkStorageSession`/curl backend plays that role in our build.

### 2.4 epoxy-tls — architecture and fit

Repo: <https://github.com/MercuryWorkshop/epoxy-tls> (pushed 2026-06-07; npm `@mercuryworkshop/epoxy-tls` v2.1.19-1, ~5.2 MB unpacked).

- **Architecture** (client/Cargo.toml, READMEs): Rust compiled to **wasm32-unknown-unknown via wasm-bindgen (not Emscripten)**, nightly required. HTTP: **hyper 1.4** + `hyper-util-wasm` fork + patched `h2` for wasm (HTTP/2). TLS: **futures-rustls (rustls + ring)** with webpki-roots. WebSockets: fastwebsockets. Wisp: their own **`wisp-mux`** Rust crate (also used by epoxy-server). Tokio single-thread executor on wasm. Exposes `fetch()`, WebSockets, and raw TLS/TCP/UDP streams; `wisp_v2` and `udp_extension_required` options; a "minimal" build variant exists.
- **vs libcurl.js**: epoxy is the more modern, more actively-developed Wisp client with HTTP/2-over-rustls; libcurl.js is C/Emscripten with mbedTLS. Both terminate TLS inside wasm.
- **As our network layer?** No. It is a *separate wasm-bindgen module*: it cannot be statically linked into an Emscripten C++ binary, so using it would mean bridging every socket/request across two wasm modules through JS and abandoning WebCore's native curl backend (cookies, cache, auth, redirects all reimplemented). Keep epoxy as: (a) reference implementation of a hardened Wisp client, (b) `epoxy-server` as our perf-tier Wisp server, (c) a debugging cross-check ("does this URL fetch over Wisp at all?").

---

## 3. EMSCRIPTEN STATE OF THE ART (2025–2026)

Current release: **Emscripten 6.0.0, released 2026-06-04** (ChangeLog: *"6.0.0 - 06/04/26"*, with 6.0.1 in development; <https://github.com/emscripten-core/emscripten/blob/main/ChangeLog.md>). Notable recent changes: `-sUSE_PTHREADS`/`-sMEMORY64` deprecated in favor of standard `-pthread`/`-m64`; minimum targets bumped to Chrome 85 / Firefox 79 / Safari 14.1; streaming-Fetch chunks capped at 8 MB; IDBFS autopersist callbacks.

### 3.1 pthreads
Doc: <https://emscripten.org/docs/porting/pthreads.html> (accessed 2026-06-09).
- **Stable** ("This support is considered stable in Emscripten"), built on SharedArrayBuffer + Atomics; **requires COOP/COEP headers** in deployment or threads silently unavailable.
- **Best practice**: `-pthread -sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency` to pre-spawn workers at preRun — otherwise `pthread_create` cannot complete until the event loop turns, which breaks create-then-join patterns (a real risk in WTF::Thread usage).
- **`-sPROXY_TO_PTHREAD`**: runs `main()` on a worker so application code may block legally. This is the right shape for us: **WebKit's main thread = a worker; the browser main thread only does canvas blits, input capture, and WebSocket I/O.** Rationale: `Atomics.wait` is forbidden on the main browser thread; Emscripten falls back to busy-waiting there (tab jank, battery burn) — any `pthread_mutex_lock/usleep/pthread_join` on the real main thread is a hazard.
- **Proxying gotchas**: DOM/WebGL/WebSocket JS calls from workers are auto-proxied to the main thread (`__proxy: 'sync'|'async'` annotations); sync proxying blocks the calling worker — fine — but adds latency on hot paths; no POSIX signals.

### 3.2 Memory64
- **Shipped**: Firefox 134 (Jan 2025) and Chrome 133 (Feb 2025); **Safari: not shipped** as of the sources reviewed (SpiderMonkey blog 2025-01-15 <https://spidermonkey.dev/blog/2025/01/15/is-memory64-actually-worth-using.html>; chromestatus <https://chromestatus.com/feature/5070065734516736>; caniuse <https://caniuse.com/wf-wasm-memory64>).
- **Measured penalty**: *"can range from just 10% to over 100% — a 2x slowdown just from changing your pointer size"*, because the 4 GB-reservation bounds-check-elision trick is impossible in 64-bit; JS API additionally caps memories at 16 GB (SpiderMonkey blog, ibid.).
- **Verdict for us**: stay **wasm32**; treat >4 GB pressure as a feature-trimming bug, not a Memory64 use case.

### 3.3 JSPI vs Asyncify
- **JSPI status**: spec at W3C Wasm CG **Phase 4, standardized April 2025**; shipped in **Chrome 137 and Firefox 139**; Safari withdrew its objection late 2025 and has implementation assigned but **has not shipped** (interop tracking <https://github.com/web-platform-tests/interop/issues/1093>; chromestatus <https://chromestatus.com/feature/5674874568704000>; intro <https://v8.dev/blog/jspi>; recap <https://platform.uno/blog/the-state-of-webassembly-2025-2026/> via <https://webassembly.org/news/2026-01-21-states-of-webassembly/>).
- **Asyncify cost**: official docs — Asyncify instruments the whole binary and *"can cause the Wasm output to be much larger"*, while JSPI *"code size will remain the same"* but is still flagged experimental in Emscripten (<https://emscripten.org/docs/porting/asyncify.html>). Community measurements commonly cite ~50%+ size and meaningful runtime overhead for Asyncify on large apps — on a 30–60 MB WebCore binary, Asyncify is a non-starter.
- **Our angle**: with `PROXY_TO_PTHREAD`, *synchronous-over-async is mostly unnecessary* — WebKit threads can legally block on Atomics while JS I/O completes on the main thread (this is how Emscripten already proxies sockets). JSPI remains an optional optimization for any residual main-thread-synchronous edge, gated on Safari shipping.

### 3.4 Filesystems
Doc: <https://emscripten.org/docs/api_reference/Filesystem-API.html> (accessed 2026-06-09).
- **WasmFS**: *"high-performance, fully-multithreaded, WebAssembly-based file system layer … will replace the existing JavaScript version"*; status *"Stable, but not yet feature-complete with the old FS"*. Crucially the legacy JS FS **proxies every FS op to the main thread under pthreads** — exactly wrong for a threaded engine — while WasmFS is thread-native. WasmFS has an **OPFS backend** (`-sWASMFS` + OPFS backend API).
- **IDBFS**: legacy JS FS + explicit/auto `FS.syncfs` persistence to IndexedDB; `autoPersist: true` mount option; fine for low-rate data, wrong for a page cache.
- **Precedent**: Photoshop-on-web uses **OPFS access handles** as its mmap-like high-performance scratch storage (<https://web.dev/articles/ps-on-the-web>).
- **Recommendation**: cookies/localStorage-like small state → IDBFS (or simple JS-side KV) early; HTTP cache + sqlite-backed stores → **WasmFS + OPFS** at Phase 4.

### 3.5 SIMD and exception handling
- **SIMD128** and the **wasm exception-handling proposal** are baseline across Chrome/Firefox/Safari (feature matrix <https://webassembly.org/features/>; Emscripten 6.0 now *assumes* post-2020 engines — min Safari 14.1, ChangeLog ibid.). Use `-msimd128` selectively (Skia/pixel paths) and **`-fwasm-exceptions`** (native wasm EH, not the legacy JS-EH emulation) — WebKit builds with exceptions mostly off, but ICU/HarfBuzz/stdlib paths and `WTF::CrashOnOverflow`-adjacent code keep EH cheap insurance at near-zero cost in native EH mode.

### 3.6 Emscripten ports coverage
From <https://github.com/emscripten-core/emscripten/tree/main/tools/ports> (listing fetched 2026-06-09):
- **Available**: `icu.py`, `freetype.py`, `harfbuzz.py` (3.2.0), `libpng.py` (1.6.58 per ChangeLog), `libjpeg.py` (9f), `zlib.py`, `sqlite3.py`, `bzip2.py`, `giflib.py`, sdl2/sdl3 family.
- **Missing (build ourselves — all proven trivial by libcurl.js's ~20-line scripts)**: **brotli**, libwebp, libxml2/libxslt, cairo (if chosen over Skia), mbedTLS, nghttp2, curl itself.
- Caveat: port versions (e.g. HarfBuzz 3.2.0) lag what modern WebKit expects — plan to pin our own builds of freetype/harfbuzz/icu rather than relying on `--use-port` defaults.

---

## 4. SIZE / PERF ENVELOPE

**Anchors (all cited above)**:
| Precedent | Payload |
|---|---|
| JSC.js = JSC(CLoop)+WTF, 2019 | ~**4 MB compressed** (README) |
| CanvasKit (full Skia) | ~**1.5 MB** added download (Flutter docs) |
| skwasm (trimmed Skia) | ~**1.1 MB** (Flutter docs) |
| libcurl.js = curl+mbedTLS+nghttp2+zlib+brotli | **552 KB compressed** (README) |
| Photoshop web | size unpublished; relies on threads + OPFS + lazy loading (web.dev) |

**Estimate for trimmed WebCore+JSC** (estimate, medium confidence): native WebCore release builds are roughly an order of magnitude larger than JSC. Scaling from JSC.js's 4 MB compressed: WebKit.wasm (WebCore+JSC+WTF, minimal feature flags, Skia CPU raster, curl backend) ≈ **30–60 MB uncompressed wasm, 10–20 MB brotli**. Assets on top: filtered ICU data (full `icudt*.dat` is ~30 MB; the ICU Data Build Tool filters to a few MB — <https://unicode-org.github.io/icu/userguide/icu_data/buildtool.html>), ~2–5 MB font subset (Noto/DejaVu), CA bundle (~200 KB, libcurl.js ships one). **Realistic initial download target: 15–25 MB brotli** — large but in line with big-wasm practice and ~25× smaller than the VM alternatives we rejected.

**Load strategy** (from precedents):
1. **Brotli-precompress** everything; serve with correct `Content-Encoding` + long-lived caching; `WebAssembly.instantiateStreaming` so compile overlaps download.
2. **No Emscripten dynamic linking** for v1 (`dlopen`/`SIDE_MODULE` costs perf and complexity; even Photoshop's dynamic-linking needs drove custom toolchain work). Single MAIN module; split *data*, not code.
3. **Lazy data packages**: ICU data, fonts, and any DevTools/inspector extras as separately-fetched file_packager bundles mounted on demand; start with `en` ICU filter, fetch locales lazily.
4. **Threading**: pthread pool sized `min(hardwareConcurrency, ~8)`; COOP/COEP from day one (dev server already planned).
5. **Defer** Memory64 (ruled out) and GPU canvas (Phase 6, CanvasKit-style WebGL2 Skia surface).

---

## VERDICT: recommended network bridge design + toolchain pins

**Network bridge (confidence: high)** — Adopt the libcurl.js *pattern*, not its artifacts wholesale:
1. Build WebCore with its **curl network backend**; link **curl 8.17.x + mbedTLS + nghttp2 + zlib + brotli** compiled for Emscripten using libcurl.js's `client/tools/*.sh` recipes (LGPL: keep our shim isolated; recipes are trivial to re-derive).
2. Let curl's BSD sockets hit **Emscripten SOCKFS** (`-lwebsocket.js`), and replace the WebSocket constructor with **wisp-js's `WispWebSocket`** — implemented as a clean custom `--js-library`/runtime override in our repo (maintainable), not regex fragment patching of emcc output (libcurl.js's approach, fine for them, brittle for a binary our size). One socket = one Wisp stream; hostname survives via wsproxy-style URL addressing, so no in-wasm DNS.
3. TLS terminates inside WebKit.wasm (mbedTLS under curl) exactly as the project brief requires; the Wisp server stays a dumb relay. **epoxy-tls is not linkable into an Emscripten binary (wasm-bindgen world)** — keep it only as a reference client and use **epoxy-server** when we need a fast relay.

**Toolchain pins**:
- **emsdk: pin 6.0.0** (released 2026-06-04; record exact emsdk hash in the brief once installed). It is days old — if Phase 1 hits toolchain bugs, fall back to the last 5.x tag and note it. (confidence: medium)
- **Memory64: NO** — wasm32 only. 10–100% perf tax, Safari absent, 16 GB JS-API cap anyway; trim features instead. (confidence: high)
- **JSPI vs Asyncify: neither in the core architecture.** Run the engine under `-pthread -sPROXY_TO_PTHREAD` with a pre-sized pool so blocking happens on workers; **never ship Asyncify** on a 30–60 MB binary; revisit **JSPI as an optimization** once Safari ships (Chrome 137/Firefox 139 already have it). (confidence: medium-high)
- **libcurl.js reuse vs custom shim: reuse recipes + wisp-js client, write our own thin shim** at the Emscripten JS-library layer. (confidence: high)
- **Wisp server pin: wisp-js (`@mercuryworkshop/wisp-js` 0.4.1) for dev** — Wisp v2 client+server in one npm package; epoxy-server for perf tier. **Do not pin wisp-server-python** (still v1-only per its README roadmap). (confidence: medium-high)

**Corrections to project-brief assumptions**:
- Brief lists "wisp-js / wisp-server-python" as the server: drop the Python server from the default path (v1-only).
- Brief's risk "ICU data / total size 100 MB+" is overstated: evidence-based envelope is **15–25 MB brotli initial** with lazy ICU/fonts.
- Brief's "cairo-or-Skia" open question: webkit.js's 2014 anti-Skia finding is obsolete — WebKit (GTK 2.46+, Oct 2024) made **Skia the default and deprecated cairo**; prefer Skia CPU raster to stay on WebKit's maintained path.
- Brief Phase 6 "maybe Memory64": remove — ruled out on evidence.
