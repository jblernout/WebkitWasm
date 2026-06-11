# Decision 006 — Guest WebAssembly scoping: JSC IPInt on the CLoop port

Date: 2026-06-11. Status: **NO-GO on IPInt — the kill criterion fired.**
Scoping pass only (task #53), modeled on the Workers scoping
(decision-004). No build flag was flipped.

## Question

Discord-class sites hard-fail at "WebAssembly is undefined" (guest JS
feature-detects and takes the no-wasm death path). JSC's IPInt (In-Place
Interpreter) executes wasm without a JIT — it is build-time-assembled
offlineasm, needs no runtime executable memory, and is what Apple ships
when JIT is unavailable. Could `ENABLE(WEBASSEMBLY)` ride IPInt on our
JIT-less CLoop port?

## Kill criterion (verified in the pinned tree) — FIRED

The question was always: does IPInt exist under the offlineasm **cloop**
backend? It does not, at three independent levels:

1. **`IPInt::initialize()` release-asserts under C_LOOP**
   (llint/InPlaceInterpreter.cpp:72): the real body is gated on
   `#if !ENABLE(C_LOOP) && ((CPU(ADDRESS64) && (CPU(ARM64) || CPU(X86_64)))
   || (CPU(ADDRESS32) && CPU(ARM_THUMB2)))`; the `#else` branch is
   `RELEASE_ASSERT_NOT_REACHED("IPInt only supports ARM64 and X86_64 (for
   now).")`.
2. **The asm has no cloop lowering** (llint/InPlaceInterpreter.asm:681):
   `js_to_wasm_wrapper_entry` opens with `if not WEBASSEMBLY or C_LOOP →
   error`. IPInt is hand-written register-level offlineasm (raw `emit`s,
   SYSV ABI `extern "C"` entry points, computed-goto dispatch off base
   pointers, native tail calls); upstream maintains C_LOOP lowerings only
   for the JS LLInt (LowLevelInterpreter.asm), not for IPInt.
3. **The boot chain makes the abort unconditional**
   (llint/LLIntData.cpp:157): under `ENABLE(WEBASSEMBLY)`,
   `if (Options::useWasm()) IPInt::initialize();` runs at JSC init, and
   `useWasm` defaults to `canUseWasm()` = true whenever the feature is
   compiled in (Options.cpp:1526). So `-DENABLE_WEBASSEMBLY=1` on our port
   = **RELEASE_ASSERT abort at engine boot**. Setting `useWasm=false` to
   dodge the assert removes the `WebAssembly` global — i.e. today's state,
   pointless.

Supporting facts:
- `wtf/PlatformEnable.h` never auto-enables WEBASSEMBLY for our config
  (default paths require `CPU(ADDRESS64) && PLATFORM(COCOA) &&
  !ENABLE(C_LOOP)` or B3); a manual define would stick — and hit the
  abort above.
- **There is no fallback interpreter tier in this tree.** The old wasm
  LLInt (`llint/WebAssembly.asm`) was deleted upstream when IPInt replaced
  it; BBQ/OMG are real JITs (runtime executable memory — structurally
  impossible inside wasm32).
- 32-bitness is NOT the blocker: IPInt supports ADDRESS32 on ARMv7-thumb2.
  The missing piece is specifically an offlineasm backend for wasm32 /
  the cloop C backend.

## Paths that could still deliver guest wasm (cost-ranked)

- **A. Write an offlineasm cloop (or wasm32) backend for IPInt** — lower
  SYSV entries, computed dispatch, native tail calls and raw emits to C
  (or wasm). Effectively porting an interpreter written in assembly to a
  new architecture. Months, research-grade, permanently divergent from
  upstream. NO for now.
- **B. Bolt a C++ wasm interpreter into JSC as a tier** (wasm3-style
  vendored engine behind the WebAssembly JS API): must reimplement JSC's
  Wasm runtime objects, traps/exceptions, GC refs, JS API glue. Months.
  NO for now.
- **C. wasm2js translation shim (Binaryen) — the only GO-shaped option.**
  Polyfill the WebAssembly JS API in guest pages; translate module bytes
  → asm.js-style JS and eval in the guest. Binaryen can run HOST-side
  (it's a JS/wasm library — fast in the host's native browser) with the
  embedder bridging bytes out / JS back, like the existing host bridges.
  All embedder/host-side, NO WebKit-tree surgery, days-not-months.
  Limits: wasm-MVP features only (no threads, no SIMD, partial
  post-MVP), translated code runs at CLoop-JS speed (slow), huge modules
  produce huge JS. Discord ships SIMD-using modules — expect partial
  coverage at best; sites doing small wasm (codecs, hashing,
  feature-detect-then-fallback) become functional.
- **D. Host-native execution bridge** (guest wasm runs in the host
  realm): REJECTED — guest code escaping into the host page breaks the
  security/architecture model, same grounds as the host-V8 JS hybrid
  (decision-005 era verdict).

## Verdict

- IPInt on CLoop: **NO-GO** — record and move on. Re-check only if
  upstream ever grows a cloop/wasm lowering for IPInt (watch
  InPlaceInterpreter.cpp's architecture gate).
- Guest wasm remains achievable ONLY via Option C (wasm2js shim). If
  Discord-class support stays a goal, scope it as its own epic:
  feasibility spike = grab Discord's actual .wasm modules, run wasm2js,
  measure translated-JS size + whether SIMD/threads kill it. Cheap spike
  (~half a day) before committing.

## Option C feasibility spike — run 2026-06-11 (task #54): **GO**

Method: scraped every `.wasm` referenced by discord.com's asset bundles
via host Chromium (login page network capture + bundle-scan of all
loaded JS), then ran emsdk's `wasm2js -all -O1` (Binaryen v130) on each.
Throwaway rigs in `build/spike-wasm2js/` (capture.mjs, analyze.sh,
run-translated.mjs); modules + results.tsv there too (gitignored).

What Discord actually ships (103 modules, 84.3 MB):
- **101 are tree-sitter grammars** (code-block syntax highlighting),
  Emscripten `dylink` side modules, lazily loaded per language. 4.7 KB
  (ini) to 9.1 MB (lean).
- **1 Lottie/Skottie-style animation renderer** (714ffcb9…, 1.45 MB).
- **1 Rust/wasm-bindgen client-state engine** (6e6e667c….module.wasm,
  1.18 MB — guild/role action handlers, idna). Post-login app code, not
  fetched at login.

Translation results (`results.tsv`):
- **102/103 translate OK**: 84.3 MB wasm → 292.4 MB JS, **avg ×3.47**
  expansion (worst ×52 on one 43 KB module; biggest single output
  13.7 MB JS from the 9.1 MB lean grammar). Translate time mostly <3 s
  per module, worst 109 s — host-side, lazy, cacheable, off the guest
  thread.
- **1/103 FAILS**: the Rust state module — `Fatal: modules with multiple
  tables are not supported yet` (wasm-bindgen externref table). A
  structural wasm2js limitation; not fixable from our side.
- **ZERO SIMD/threads failures.** The "Discord uses SIMD" fear from the
  original scoping did not materialize in the actual payload.

Executability proven (the shim's core bet): the translated ini grammar
runs in pure JS with NO WebAssembly anywhere — `tree_sitter_ini()`
returns a relocated TSLanguage struct whose first u32 reads
**abi_version = 14** (correct tree-sitter ABI). Emulated `env` imports
needed: `memory(.buffer)`, `__memory_base`, `__table_base`,
`__indirect_function_table`. Output form: ES6 module with
`import * as env from 'env'` + base64 data segments → trivially
rewrapped as a factory function for guest eval.

Login-path requirement is tiny: the login page fetches **zero** .wasm.
It dies on feature-detect — `wasmSupported` instantiates a ~40-byte
probe module from a Uint8Array literal. Polyfill surface used across
the login bundles: `instantiate`, `instantiateStreaming`, `Module`,
`Instance`, `Memory`, `CompileError`, `RuntimeError`, plus a guarded
`WebAssembly.Exception` presence check (Sentry) — stub class suffices.
Other gates (libdave voice E2EE) reject gracefully without wasm.

Shim epic shape (phased, all embedder/host-side, no WebKit surgery):
1. **Phase S-A — API polyfill + host translation bridge**: guest-page
   `WebAssembly` global; module bytes → host (Binaryen wasm2js in the
   host page) → translated JS string → guest eval via factory wrap.
   Unblocks login (probe module) and tree-sitter highlighting.
2. **Phase S-B — coverage/limits**: translation cache, instantiate
   import-object plumbing (Memory/Table emulation classes), graceful
   `CompileError` on untranslatable modules (multi-table Rust module
   takes Discord's own no-wasm fallback paths, which exist).
Known limits to record up front: CLoop-speed execution, avg ×3.5 JS
size in guest heap (lazy per-module, acceptable), multi-table
wasm-bindgen modules unsupported.

## Phase S-A — SHIPPED 2026-06-11 (task #55)

Implementation (all embedder/host-side; ONE one-word WebKit relax):
- **Injection point**: `BibFrameLoaderClient::dispatchDidClearWindowObjectInWorld`
  (EmptyFrameLoaderClient.h relaxes the method final→override — same
  family as the four Phase-4 loader-client kills; patch ledger updated).
  Fires once per new document window including subframes, before any
  author script. Main world only.
- **Engine pipes (src/embedder/main.cpp)**: `bibWasm2jsHostFunction`
  (JSC_DEFINE_HOST_FUNCTION, C++ linkage outside the extern "C" export
  block) — installs as `__bibWasm2js` on the guest global; synchronously
  round-trips base64 module bytes → EM_ASM → `Module.bibWasm2js` (host
  realm, same thread) → translated JS string back into the guest.
  `wasmPolyfillSource()` reads `Module.bibWasmPolyfill` once
  (NeverDestroyed cache). `bib_wasm_alloc` (KEEPALIVE malloc) services
  EM_ASM string returns.
- **Guest polyfill (web/wasm-polyfill.js)**: full WebAssembly namespace
  (Module/Instance/Memory/Table/Global/Tag/Exception + 3 error classes,
  validate/compile/instantiate/instantiateStreaming). Captures+deletes
  `__bibWasm2js` before any early return. Sync `new Module()` works
  (Discord's wasmSupported probe). Untranslatable → CompileError.
- **Host translator (web/browser.html)**: binaryen.js v130 (vendored,
  web/vendor/binaryen.js, 13.4 MB) readBinary →
  **setFeatures(Features.All)** → emitAsmjs → rewrap ES6 output as
  `(function(imports){...; return asmFunc(imports);})` (import lines →
  `var local = imports["name"]`, auto-instantiation tail cut at
  column-0 `var memasmFunc|retasmFunc`). Engine boot is GATED: the
  embedder.js script tag is appended only after binaryen.js + polyfill
  text settle, so Module.bibWasmPolyfill is set before first injection.

Hard-won traps (recorded in the porting playbook too):
- **binaryen.js defaults to MVP features** — a bulk-memory module then
  hard-aborts binaryen's own wasm instance (C-level assert), which in
  the host page would brick the shim for the session. setFeatures(All)
  immediately after readBinary. The CLI's `-all` masked this; only the
  binaryen.js parity run exposed it.
- Codex review (0 HIGH / 2 MED / 1 LOW, all fixed): tail-cut regex must
  be column-0 anchored (verified only-unindented across the 102-module
  corpus); import-local regex must accept `$` ([A-Za-z_$][\w$]*);
  bridge deletion must precede every polyfill early-return path.

Validation:
- **gate9 (NEW, tools/gate9-wasm-test.mjs + web/probe/wasmprobe.html):
  12/12 PASS** — typeof/classes/Exception presence, validate good+bad,
  the EXACT sync Discord probe sequence, async instantiate with import
  object, exported-memory reads, Memory class grow, bad-bytes
  CompileError rejection, instantiateStreaming over the engine's
  curl→Wisp network stack.
- **gate2 (raster) PASS, gate8 (GPU) PASS** — no regression from the
  engine rebuild or the dynamic boot gate.
- **discord.com/login in the engine: the wasm death is GONE.** Boot now
  proceeds deep into app init (DatabaseManager, MediaEngineWebRTC,
  OverlayRenderStore, crash reporter). libdiscore (the multi-table Rust
  module) gets CompileError → Discord's own graceful skip
  ("Unsupported browser, skipping libdiscore"). New top blockers are
  NOT wasm: `ReferenceError: Can't find variable: Audio` (we build with
  ENABLE_VIDEO=OFF — no HTMLAudioElement named constructor) and one
  NetworkError. Page still paints white — next root-cause is the Audio
  gap, backlog item.
- **binaryen.js↔CLI parity over the full 103-module corpus: 101 OK /
  2 FAIL** (build/spike-wasm2js/parity-binaryenjs.mjs — chunked child
  processes; binaryen.js's wasm heap grows monotonically across
  translations and OOMs a single process). FAIL 1 = the multi-table
  Rust module (expected, matches CLI). FAIL 2 = tree_sitter_vim
  (8f278c…, 1.1 MB — the CLI's slowest module at 109 s): "Maximum call
  stack size exceeded" inside binaryen.js — a binaryen.js-vs-native
  divergence, browser host pages have the same JS stack limits, so vim
  code blocks degrade to unhighlighted via clean CompileError. S-B
  candidate: retry translation in a host Worker or pre-translate a
  cache.
