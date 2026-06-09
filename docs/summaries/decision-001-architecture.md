# Decision 001 — Execution strategy & engine

**Date**: 2026-06-09 · **Status**: ACCEPTED (user choice, recorded)

## Decision
Build a **true source port of WebKit to WebAssembly** (WebCore + JSC via
Emscripten). Target engine: **WebKit**. No VM/emulation layer. Canvas is the
engine's renderer; networking goes over **Wisp**.

## Options considered
1. **QEMU-wasm / container2wasm VM** — real x86_64 Linux + unmodified modern
   browser. Authentic but slow; the "port" is mostly integration work.
2. **v86 JIT VM (Anura lineage)** — faster, native Wisp ecosystem, but
   32-bit only: no modern Chrome, ties us to Firefox i686.
3. **CheerpX/WebVM** — fastest VM, but proprietary engine and its networking
   fights the Wisp requirement.
4. **WebKit→WASM source port** ← CHOSEN. Highest effort and risk, best
   end-state: engine IS the wasm, canvas IS the renderer, no emulation tax.

## Why WebKit (vs Blink/Gecko) for a direct port
- Only major engine with a no-JIT interpreter mode that is a supported,
  maintained configuration (JSC CLoop) — mandatory, since wasm can't self-JIT.
- Embedded-port lineage (JSCOnly, WPE, PlayStation, WinCairo) means the tree
  already abstracts OS, network backend (curl option), and run loop.
- Prior art exists for the hardest first step: JSC has been compiled to wasm
  before (JSC.js); a partial WebCore port was attempted (webkit.js, 2014).
- Chromium/Gecko are multiprocess-entangled, JIT-entangled, and 3–5× larger.

## Consequences
- Multi-month frontier project; Phases 0–2 are deliberately cheap kill-gates.
- Single-process embedder = no site isolation inside the guest. Acceptable:
  the whole thing already lives inside the host browser's sandbox.
- JS performance capped by CLoop interpreter.
- We own three novel components: Emscripten platform layer for WebCore,
  canvas display/input bridge, curl-socket→Wisp bridge.

## Confidence
- WebKit as the right engine for a direct port: **high**.
- Overall port achievable to Phase 2 (first paint): **medium**.
- Reaching Phase 4+ (full browsing over Wisp): **medium-low** — honest frontier
  territory; gates exist to find out cheaply.
