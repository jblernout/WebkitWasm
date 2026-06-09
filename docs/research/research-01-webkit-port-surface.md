# Research 01 — WebKit Port Surface for an Emscripten/WASM Port

**Date**: 2026-06-09 · **Author**: research agent (web research only)
**Scope**: Answers the five Phase-0 questions from `00-project-brief.md` about the
WebKit port surface. All tree facts checked against `WebKit/WebKit@main` on
2026-06-09 via GitHub API/raw files unless noted. URLs cited inline.

---

## 1. PORT MATRIX (as of 2025–2026)

Ports recognized by the build system today (`ALL_PORTS` in
[Source/cmake/WebKitCommon.cmake](https://github.com/WebKit/WebKit/blob/main/Source/cmake/WebKitCommon.cmake), lines 75–93):
**GTK, IOS, JSCOnly, Mac, PlayStation, WPE, Win**. "WinCairo" no longer exists as
a name — commit "[CMake] Rename WinCairo port to Win" landed **2024-07-12**
(OptionsWin.cmake history, [github.com/WebKit/WebKit commits for Source/cmake/OptionsWin.cmake](https://github.com/WebKit/WebKit/commits/main/Source/cmake/OptionsWin.cmake)).

| Port | Network | Graphics | Run/event loop | Platform layer | Health |
|---|---|---|---|---|---|
| **GTK** | libsoup3 (`find_package(Soup3 3.0.0 REQUIRED)`, [OptionsGTK.cmake](https://github.com/WebKit/WebKit/blob/main/Source/cmake/OptionsGTK.cmake)) | **Skia** (`USE_SKIA ON`, `USE_CAIRO OFF` hardcoded; cairo still *linked* for printing — `find_package(Cairo 1.16.0 REQUIRED)`) | GLib main loop (`GLib 2.70.0 REQUIRED`) | glib/unix/gtk dirs | **Healthy** — Igalia-led, 6-month release train, 2.52.4 tarball dated 2026-06-01 |
| **WPE** | libsoup3 ([OptionsWPE.cmake](https://github.com/WebKit/WebKit/blob/main/Source/cmake/OptionsWPE.cmake) line 18) | **Skia** (`USE_SKIA ON`, `USE_CAIRO FALSE` line 433) | GLib main loop | libwpe + new WPEPlatform API | **Healthy** — flagship embedded port ([wpewebkit.org release schedule](https://wpewebkit.org/release/schedule/)) |
| **Win** (ex-WinCairo) | **curl** (`find_package(CURL 7.87.0 REQUIRED)`, `USE_CURL ON`, [OptionsWin.cmake](https://github.com/WebKit/WebKit/blob/main/Source/cmake/OptionsWin.cmake) lines 44, 122) | **Skia default since 2025-04-09** ("[Win] Switch to use Skia and HarfBuzz"); cairo retained as fallback (`if NOT USE_SKIA → Cairo 1.18.0`, lines 166–173); `COORDINATED_GRAPHICS` disabled | Windows-message-pump RunLoop; being reworked ("re-landing the runloop changes", [Grunert Nov 2025](https://iangrunert.com/2025/11/06/webkit-windows-port-update-november-2025)) | `platform/win` | **Active** — Microsoft (Ian Grunert) + Sony + community; all JSC JIT tiers enabled (Oct 2024), libpas ported, Azure-sponsored CI, cross-compile-from-Linux landed 2026-03-10 ([bug 282276](https://bugs.webkit.org/show_bug.cgi?id=282276)) |
| **PlayStation** | **curl** (`CURL 7.85.0 REQUIRED`, `USE_CURL ON`, [OptionsPlayStation.cmake](https://github.com/WebKit/WebKit/blob/main/Source/cmake/OptionsPlayStation.cmake) lines 99, 266) | **Skia default**, cairo fallback (`if NOT USE_SKIA → Cairo`, lines 271–275; `SceCairoForWebKit`) | **WTF generic RunLoop** (`platform/generic`; "Fix PlayStation build following 306535@main" in `wtf/generic` history 2026-01-31) | `platform/playstation` + WPEBackendPlayStation | **Active** — Sony; build fixes through Jan 2026 |
| **JSCOnly** | none | none | **Generic WTF RunLoop by default** (`DEFAULT_EVENT_LOOP_TYPE "Generic"` → `USE_GENERIC_EVENT_LOOP`, [OptionsJSCOnly.cmake](https://github.com/WebKit/WebKit/blob/main/Source/cmake/OptionsJSCOnly.cmake) lines 30–114); GLib only opt-in | minimal (WTF + JSC) | **Active** — commits through 2026-01-21 ("[Win] Fix JSCOnly build on Windows") |
| (Mac/iOS, reference) | CFNetwork (`platform/network/cf`, `cocoa`) | CoreGraphics | CFRunLoop | Apple-only | n/a for us |

**Network backends in the tree** (`Source/WebCore/platform/network/` subdirs,
checked 2026-06-09): `cf cocoa curl glib ios mac playstation soup win` — i.e.
only **two cross-platform backends: curl and soup**.

**Is the curl backend maintained?** Yes. Commits touching
`Source/WebCore/platform/network/curl` run continuously through **2026-06-09**
("[Win] Fix OpenSSLHelper summary() fallback") including functional work, not
just refactors: CHIPS partitioned-cookie blocking (2025-08-16), Cookie Store API
fix (2025-08-14), Enhanced Security request tracking (2026-01-27)
([commit list](https://github.com/WebKit/WebKit/commits/main/Source/WebCore/platform/network/curl)).
It is the production backend for two corporate-backed ports (Win: Microsoft/Sony;
PlayStation: Sony).

**Skia migration timeline (important nuance):**
- Feb 2024: Igalia announces Skia switch for GTK/WPE, with Google/Sony/Apple/Red Hat
  buy-in for GTK, WPE, PlayStation and WinCairo
  ([carlosgc blog](https://blogs.igalia.com/carlosgc/2024/02/19/webkit-switching-to-skia-for-2d-graphics-rendering/),
  [webkit-dev thread](https://lists.webkit.org/pipermail/webkit-dev/2024-February/032615.html)).
- Sept/Oct 2024: GTK/WPE 2.46 ships Skia as default; cairo kept only for
  big-endian and explicitly "no longer receiving active development"
  ([webkitgtk.org 2.46 notes](https://webkitgtk.org/2024/10/04/webkitgtk-2.46.html),
  [wpewebkit 2.46 highlights](https://wpewebkit.org/blog/2024-wpewebkit-2.46.html)).
- 2025-04-09: Windows port switches to Skia+HarfBuzz by default (OptionsWin history).
- Feb 2026: `USE_SKIA=OFF` (cairo) **no longer supported** for GTK/WPE — commit
  [306343@main](https://commits.webkit.org/306343@main); 2.53.1 is the first dev
  release without the choice; 2.54.0 (Sept 2026) will be the first stable
  ([Igalia WebKit Periodical #56, 2026-02-09](https://blogs.igalia.com/webkit/blog/2026/wip-56/)).
- As of 2026-06-09 `Source/WebCore/platform/graphics/cairo` still exists on main,
  but only as the non-default Win/PlayStation fallback. **Skia is vendored in-tree
  at `Source/ThirdParty/skia`** and built by WebKit's own CMake (verified via
  GitHub contents API 2026-06-09).

**Healthy vs bitrotted:** GTK and WPE are the healthiest (Igalia, release
cadence, CI). Win is genuinely active again (Microsoft hired staff; 2025 saw
JIT-complete, libpas, LTO; see [Grunert's Nov 2025 update](https://iangrunert.com/2025/11/06/webkit-windows-port-update-november-2025)).
PlayStation is corporate-maintained but builds against closed SDK pieces
(public CI builds a Linux-ish variant with [WebKitRequirements](https://github.com/WebKitForWindows/WebKitRequirements)-style packages).
JSCOnly is small but maintained. Nothing in this list is bitrotted; the one
*deprecated* thing is the cairo rendering path.

---

## 2. STANDALONE WEBCORE (no WebKit2)

**Can WebCore build without the WebKit2 layer? Yes, structurally.** The CMake
system has port-controlled gates `ENABLE_WEBCORE`, `ENABLE_WEBKIT` (WebKit2) and
`ENABLE_WEBKIT_LEGACY`; JSCOnly proves the machinery by setting all three OFF
([OptionsJSCOnly.cmake](https://github.com/WebKit/WebKit/blob/main/Source/cmake/OptionsJSCOnly.cmake)
lines 39–42: `set(ENABLE_WEBCORE OFF)`, `set(ENABLE_WEBKIT_LEGACY OFF)`,
`set(ENABLE_WEBKIT OFF)`). `Source/CMakeLists.txt` adds `bmalloc`, `WTF`,
`JavaScriptCore` unconditionally and the rest behind those flags
([Source/CMakeLists.txt](https://github.com/WebKit/WebKit/blob/main/Source/CMakeLists.txt)).
A custom port can therefore set `ENABLE_WEBCORE=ON, ENABLE_WEBKIT=OFF` and get
`libWebCore` + PAL as static libraries.

**What does NOT exist: a supported non-Apple single-process embedding API.**
- WebKitLegacy (WK1) survives **only for Cocoa**: `Source/WebKitLegacy/` today
  contains `mac/`, `ios/`, `cf/`, `PlatformMac/IOS/Cocoa.cmake` — no `win/`
  ([directory listing](https://github.com/WebKit/WebKit/tree/main/Source/WebKitLegacy), checked 2026-06-09).
- Windows WebKitLegacy was deleted **2023-02-16** ("Remove Windows WebKitLegacy
  code"; WinCairo had already disabled it 2023-02-13, and the AppleWin port was
  removed 2023-02-09 — [WebKitLegacy/win commit history](https://github.com/WebKit/WebKit/commits/main/Source/WebKitLegacy/win)).
- Every CMake port sets `ENABLE_WEBKIT_LEGACY OFF` (OptionsWin.cmake line 31,
  OptionsPlayStation.cmake line 21, OptionsJSCOnly.cmake line 40).

**Consequence for us:** a WK1-style embedder means writing our own
"WebCoreSupport" client layer (ChromeClient, FrameLoaderClient, Page
construction, etc.) against **internal, unstable WebCore interfaces** — exactly
what webkit.js did in 2014 and what WebKitLegacy/mac still does. The cmake
plumbing supports it; nothing *API-stable* supports it.

**How ports define themselves** (the downstream-port recipe):
1. `PORT` is a cache variable validated against `ALL_PORTS`
   (WebKitCommon.cmake lines 75–93; unknown values are a fatal error, so a
   custom port patches `ALL_PORTS` — one line).
2. `include(Options${PORT})` pulls `Source/cmake/Options<Port>.cmake`
   (WebKitCommon.cmake line 313) — this is where deps, `USE_*` and
   `ENABLE_*` defaults live.
3. Each target directory optionally includes `Platform${PORT}.cmake` via
   `WEBKIT_INCLUDE_CONFIG_FILES_IF_EXISTS()`
   ([WebKitMacros.cmake](https://github.com/WebKit/WebKit/blob/main/Source/cmake/WebKitMacros.cmake)
   lines 111–119) — e.g. `Source/WebCore/PlatformWin.cmake` adds the per-port
   source files. A new `PORT=Emscripten` needs `OptionsEmscripten.cmake` +
   `PlatformEmscripten.cmake` fragments in WTF/JSC/WebCore/PAL.

**Feature-flag surface:** 142 `WEBKIT_OPTION_DEFINE(ENABLE_…)` options in
[WebKitFeatures.cmake](https://github.com/WebKit/WebKit/blob/main/Source/cmake/WebKitFeatures.cmake)
(count verified 2026-06-09). A tiny build starts from PlayStation/Win defaults
and turns off, at minimum: `ENABLE_VIDEO`/`ENABLE_WEB_AUDIO` (drops GStreamer),
`ENABLE_WEBGL`/`USE_ANGLE_EGL` (drops vendored ANGLE), `ENABLE_WEBRTC`/`USE_LIBWEBRTC`,
`ENABLE_AV1` (drops dav1d), `ENABLE_GPU_PROCESS`, `ENABLE_WEBDRIVER`,
`ENABLE_GAMEPAD`, `ENABLE_GEOLOCATION`, `ENABLE_ENCRYPTED_MEDIA`,
`ENABLE_FULLSCREEN_API`, plus JIT flags per §3. ThirdParty subdirectories are
gated on exactly these flags (Source/CMakeLists.txt), so each OFF genuinely
shrinks the build.

---

## 3. JSC ON WASM (CLoop / JSCOnly / Emscripten)

**CLoop status: supported and actively maintained.**
- Option: `WEBKIT_OPTION_DEFINE(ENABLE_C_LOOP "Enable CLoop interpreter" …)`,
  [WebKitFeatures.cmake line 195](https://github.com/WebKit/WebKit/blob/main/Source/cmake/WebKitFeatures.cmake).
- Default logic in [PlatformEnable.h lines 750–756](https://github.com/WebKit/WebKit/blob/main/Source/WTF/wtf/PlatformEnable.h):
  `#if ENABLE(JIT) || CPU(X86_64) || CPU(ARM64) → C_LOOP 0, else C_LOOP 1` —
  i.e. **on an unknown CPU like wasm32 with JIT off, CLoop turns itself on**.
- Maintenance signal: CLoop-specific fixes and regression triage continuously
  through **2026-04-27** ("REGRESSION(311624@main): LLIntAssembly.h… error"),
  2026-01-07, 2025-10-27 ("REGRESSION(Tahoe): **CLoop build-and-test** regressed"
  — i.e. there is CI exercising CLoop), 2025-08-25 (CLoop stack-overflow
  detection fix) ([GitHub commit search "cloop"](https://github.com/search?q=repo%3AWebKit%2FWebKit+cloop&type=commits&s=committer-date&o=desc)).

**JSCOnly without glib: yes, that is the default.**
`DEFAULT_EVENT_LOOP_TYPE "Generic"`; glib is only pulled in when
`EVENT_LOOP_TYPE=GLib` or `ENABLE_JSC_GLIB_API=ON`
([OptionsJSCOnly.cmake](https://github.com/WebKit/WebKit/blob/main/Source/cmake/OptionsJSCOnly.cmake)
lines 25–114). Hard requirement even for JSCOnly: **ICU ≥ 70.1** (line 117).
Build entry: `Tools/Scripts/build-jsc --jsc-only`
([trac JSCOnly wiki](https://trac.webkit.org/wiki/JSCOnly), archived but accurate).

**Emscripten in the WebKit tree: zero build support.** A GitHub code search for
EMSCRIPTEN in `WebKit/WebKit` returns only test fixtures (JSTests
`emscripten-cube2hash`, the **pglite** wasm stress test, JetStream wasm
binaries), `Source/WTF/icu/unicode/platform.h` (vendored ICU header), and
`Source/bmalloc/mimalloc/.../prim/emscripten/prim.c` — the latter because
**WebKit replaced old bmalloc with vendored mimalloc in Jan 2026**
("[bmalloc] Replace old bmalloc with mimalloc", relanded 2026-01-20), and
upstream mimalloc ships an Emscripten primitive layer. That is a small
tailwind: the allocator now has an upstream wasm story.

**Bugzilla:** quicksearch "emscripten" (REST API, 2026-06-09) returns ~30 bugs,
**all** about running Emscripten output *on* JSC/Safari (wasm threads, OPFS,
memory), **none** about building WebKit *with* Emscripten
([bugs.webkit.org quicksearch](https://bugs.webkit.org/buglist.cgi?quicksearch=emscripten)).

**Prior art, 2023–2026: effectively none on current trees.**
- [mbbill/JSC.js](https://github.com/mbbill/JSC.js) — "JavaScriptCore on
  WebAssembly", 484 stars, JSC (CLoop) compiled with Emscripten, ~4 MB
  compressed wasm. **But**: it used a custom **GN** build, not WebKit's CMake,
  and the last push was **2021-10-12** (engine snapshot ~2019). It proves CLoop
  runs under wasm; it does *not* validate WebKit's CMake under emcmake.
- [wapm-packages/jsc](https://github.com/wapm-packages/jsc) — JSC built for
  WASI, used by [Wasmer 3.3 (2023)](https://wasmer.io/posts/wasmer-3_3-and-javascriptcore)
  as a JS backend. Different toolchain (WASI SDK), same lesson: the interpreter
  core is portable.
- Web/GitHub/Bugzilla searches (June 2026) found **no** 2023–2026 reports of
  building JSC or WebCore with Emscripten. Phase 1 will be first-of-its-kind on
  a current tree.

---

## 4. DEPENDENCY PORTABILITY UNDER EMSCRIPTEN

WebKit's minimum dependency set vs. wasm reality (emscripten-ports inventory
from [tools/ports on main](https://github.com/emscripten-core/emscripten/tree/main/tools/ports),
checked 2026-06-09):

| Dependency | WebKit min (OptionsWin/GTK) | Emscripten port? | Status / pain |
|---|---|---|---|
| zlib | 1.2.11 | ✅ `zlib` | Known-good |
| libpng | 1.6.34 | ✅ `libpng` | Known-good |
| libjpeg(-turbo) | 1.5.2 (API) | ✅ `libjpeg` = **IJG jpeg 9f**, not turbo | Works; turbo's SIMD asm is skipped — decode is slower but fine |
| FreeType | — | ✅ `freetype` (VER-2-13-3) | Known-good |
| HarfBuzz | ≥ 2.7.4 (GTK) / 1.4.2 (Win) | ✅ `harfbuzz` **3.2.0 (2021)** | Port is old; WebKit's current font stack may want newer — plan to self-build current HarfBuzz (harfbuzz compiles cleanly with emcc, cf. harfbuzzjs) |
| **ICU** | **≥ 70.1** (all ports) | ⚠️ `icu` port = **release-68-2** — *below WebKit's minimum* | Must self-compile ICU (standard 2-stage cross build) and manage data: full `icudt` is ~30 MB; use the [ICU data build tool filters](https://unicode-org.github.io/icu/userguide/icu_data/buildtool.html) to cut it |
| sqlite3 | 3.23.1 | ✅ `sqlite3` | Known-good; SQLite also ships an [official wasm build](https://sqlite.org/wasm/doc/trunk/index.md) |
| brotli (woff2) | dec only | ❌ no port | Trivial plain-C build; [brotli-wasm npm](https://www.npmjs.com/package/brotli-wasm) proves it |
| libwebp | demux+decode | ❌ no port | Upstream ships official wasm build scripts in [`webp_js/`](https://github.com/webmproject/libwebp/tree/main/webp_js) — known-good |
| libxml2 | 2.9.7 | ❌ no port | Known-good community builds: [libxml2-wasm npm](https://www.npmjs.com/package/libxml2-wasm) v0.7.1, updated 2026-03-14 |
| libxslt | 1.1.32 (optional: `ENABLE_XSLT`) | ❌ no port | Buildable ([libxslt-wasm npm](https://www.npmjs.com/package/libxslt-wasm) exists, marginal) — or set `ENABLE_XSLT=OFF` initially |
| **Skia** (CPU raster) | vendored `Source/ThirdParty/skia` | n/a | **Strongest story of all**: Google's [CanvasKit](https://skia.org/docs/user/modules/canvaskit/) *is* Skia built with Emscripten, continuously shipped. Risk is integrating WebKit's vendored Skia GN→CMake build with emcmake, not Skia itself |
| cairo + pixman (fallback path) | 1.18 (Win fallback) | ❌ no port | Possible but DIY/hobby-grade ([cairo-wasm org](https://github.com/cairo-wasm), [roozbehid/cairo for WasmWinforms](https://github.com/roozbehid/cairo)); pixman's x86/ARM SIMD assembly must be configured out; **and cairo inside WebKit is deprecated/unmaintained** (see §1). Skia-CPU is the lower-risk raster path despite the bigger build |
| curl + OpenSSL (TLS-in-wasm) | 7.85+/7.87+ | ❌ no ports | Both compile under emscripten with custom socket plumbing (libcurl.js lineage — covered by the companion prior-art research doc); this is "our novel component #3" per decision-001 |

**Flagged pains:** (1) ICU data size + the stale ICU port (must self-build);
(2) anything SIMD-assembly-heavy (pixman, libjpeg-turbo) falls back to C;
(3) the emscripten ports that *do* exist pin old versions — treat
emscripten-ports as proof-of-buildability, but **vendor and pin our own builds
of all 12 deps** for version control.

---

## 5. BUILD MECHANICS

**Checkout size:** the GitHub-side repository is **≈12.6 GB**
(`size` = 12,635,998 KB from the [repos API](https://api.github.com/repos/WebKit/WebKit),
2026-06-09). The [ReadMe](https://github.com/WebKit/WebKit/blob/main/ReadMe.md)
prescribes a plain full clone (plus `git config core.fsmonitor true`); plan for
~15–25 GB on disk with a build tree, more with multiple configs.
A release tarball is far smaller: `webkitgtk-2.52.4.tar.xz` is **65,093,228
bytes (~62 MB)**, dated 2026-06-01 ([webkitgtk.org/releases](https://webkitgtk.org/releases/)).

**Build entry points:**
- `Tools/Scripts/build-webkit --gtk|--wpe|--release …` — wrapper used by all
  port docs ([docs.webkit.org Building Options](https://docs.webkit.org/Build%20%26%20Debug/BuildOptions.html),
  [Windows port docs](https://docs.webkit.org/Ports/WindowsPort.html)).
- `Tools/Scripts/build-jsc --jsc-only` for JSCOnly ([trac wiki](https://trac.webkit.org/wiki/JSCOnly)).
- **Raw CMake is fully supported and is what release packagers use** — the
  WebKitGTK team's own published invocation is plain
  `cmake -DPORT=GTK -DCMAKE_BUILD_TYPE=Release -GNinja` against a tarball
  ([2.46 release notes, "WebKit Build" section](https://webkitgtk.org/2024/10/04/webkitgtk-2.46.html)).
  For a cross/emcmake build, raw CMake is the right layer; `build-webkit` is a
  convenience we will outgrow immediately.

**Pinning options:**
- **Trunk (`main`)**: moving target, ~hundreds of commits/week. Pin a hash.
- **Stable branches `webkitglib/2.52` / `webkitglib/2.48`** (shared by
  WebKitGTK + WPE): `webkitglib/2.52` last commit **2026-06-08** (active,
  current stable train); `webkitglib/2.48` last commit 2025-11-18 (EOL-ish)
  (branch API, 2026-06-09). Cadence: two feature releases/year (March +
  September), even minor = stable, odd = development; GTK and WPE share release
  branches ([wpewebkit.org/release/schedule](https://wpewebkit.org/release/schedule/)).
- **Release tarballs (`webkitgtk-2.52.x.tar.xz` / `wpewebkit-2.52.x.tar.xz`)**:
  convenient and small, **but** they are GTK/WPE distribution artifacts — the
  dist manifest filters the tree for those ports, so Win/PlayStation port files
  (notably parts of the curl backend glue and `OptionsWin/PlayStation.cmake`)
  may be absent. *Assumption to verify on first download — if the filtered
  tarball lacks the curl backend, this is decided in favor of git.*
- **2.54 caveat**: 2.54.0 (Sept 2026) removes the cairo option for GTK/WPE
  entirely ([Periodical #56](https://blogs.igalia.com/webkit/blog/2026/wip-56/));
  irrelevant if we go Skia, but it dates how fast deprecated paths get deleted.

**Recommendation for a long-lived downstream port:** clone the monorepo and pin
the **`webkitglib/2.52` branch head** (gets: full tree including Win/PlayStation
/JSCOnly port files, stable-branch backports for ~1 year, and a tarball-equivalent
baseline). Record the exact hash in `00-project-brief.md` Pins. Rebase to
`webkitglib/2.54` only at a planned milestone, never continuously.

---

## VERDICT: best base port + biggest blocker

**Best base: JSCOnly first, then a custom `PORT=Emscripten` modeled on
PlayStation, borrowing the Win/PlayStation curl network backend and the
in-tree Skia (CPU raster).** Confidence: **high** on this being the right base
among the options; **medium** that it carries through to Phase 2 first paint.

Why this and not the alternatives:
- **JSCOnly** is the only port designed for "as few dependencies as possible";
  it defaults to the **generic WTF RunLoop with no glib**, needs only ICU +
  threads, and CLoop self-enables on unknown CPUs. It is the cheapest possible
  Phase-1 vehicle and is exactly the bring-up path the build system already
  supports (`ENABLE_WEBCORE=OFF` proves the gating works).
- **PlayStation is the closest existing analog to our target**: curl
  networking, WTF generic RunLoop, Skia-with-fallback graphics, static
  single-app embedding, deps delivered as prebuilt packages. Its
  Options/Platform files are the best template for `OptionsEmscripten.cmake`.
- **Win contributes** the most battle-tested curl/OpenSSL network glue
  (`platform/network/curl` is alive as of 2026-06-09) and proof that Skia in
  WebKit runs *without* `COORDINATED_GRAPHICS` (a simpler pipeline we want).
- **Trimmed WPE is the wrong base**: glib event loop, soup networking, libwpe/
  Wayland-shaped platform API, and the WebKit2 multiprocess architecture are
  all things we would spend months deleting. WPE's value to us is its Skia
  rendering code (cross-port anyway) — not its skeleton.
- **Graphics: Skia CPU, not cairo.** The project brief's "cairo-or-Skia"
  question is now settled by upstream: cairo is unmaintained in WebKit since
  2.46 (Sept 2024) and the option is already deleted for GTK/WPE (Feb 2026);
  meanwhile CanvasKit is a permanent, official Emscripten build of Skia.
  Betting on cairo means betting on a code path upstream is actively removing.

**Biggest blocker:** **there is no supported single-process embedding API
outside Apple's Cocoa WebKitLegacy** — Windows WK1 was deleted in Feb 2023 and
every CMake port is WebKit2-only. Our embedder must construct
`WebCore::Page` and implement the client interfaces (ChromeClient,
FrameLoaderClient, …) directly against internal headers that change weekly on
trunk; additionally, the curl network stack is only ever exercised in-tree
inside the WebKit2 NetworkProcess, so running it in-process is itself novel
integration territory. This — not CLoop, not the dependencies — is where the
project most likely dies or thrives, and it is exactly what the Phase-2 gate
is for. Mitigation: pin one stable branch (webkitglib/2.52), copy the
structure of `WebKitLegacy/mac/WebCoreSupport` + PlayStation's port layer,
and keep all churn behind our own `PlatformEmscripten.cmake`/patch set.

**Secondary blockers (ranked):**
1. **WebKit's CMake under emcmake is unproven** — JSC.js bypassed it with GN in
   2019; nobody has reported `PORT=JSCOnly` + emcmake working. (Phase-1 gate.)
2. **Dependency version drift**: emscripten-ports ICU (68.2) is below WebKit's
   floor (70.1) and harfbuzz (3.2.0) is stale — all 12 deps must be self-built
   and pinned; ICU data needs filtering to keep the bundle sane.
3. **Vendored-Skia build integration** (GN-generated sources consumed by
   WebKit's CMake) under emcmake — proven possible by CanvasKit, but the
   WebKit-side wiring is ours to do.
