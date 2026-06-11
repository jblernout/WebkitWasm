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
