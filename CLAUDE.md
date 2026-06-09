# BrowserInBrowser — WebKit compiled to WebAssembly

## Goal
A fully modern browser engine running INSIDE a browser tab. No VM, no x86
emulation: WebKit (WebCore + JavaScriptCore) is compiled to WASM with
Emscripten. The page hosts an interactable `<canvas>` that is the engine's
renderer. All networking leaves the tab over the **Wisp protocol**
(wisp-js / wisp-server-python on the server side).

## Architecture decisions (see docs/summaries/decision-001-architecture.md)
- **Engine**: WebKit (WebCore + JSC). Chosen for portability — it is the only
  major engine with a realistic direct-WASM story (JSCOnly/embedded port
  lineage, curl network backend, generic RunLoop).
- **JS execution**: JSC **CLoop interpreter** (JIT-less). WASM cannot self-JIT
  conventionally; CLoop is WebKit's supported no-JIT mode.
- **Process model**: single process, WebKit1-style thin embedder. We do NOT
  port the WebKit2 multiprocess/IPC layer.
- **Rendering**: software raster first (cairo or Skia CPU) → blit to canvas.
  GPU path (Skia → WebGL2, CanvasKit-style) is a later phase.
- **Networking**: WebKit curl backend → libcurl built on a Wisp socket shim
  (prior art: Mercury Workshop libcurl.js). TLS terminates inside the engine.
- **Threads**: Emscripten pthreads → SharedArrayBuffer → host page MUST be
  served with COOP/COEP headers.

## Repo layout
- `src/` — our code: embedder shell, platform glue, Wisp bridge, host page.
- `third_party/` — WebKit checkout, emsdk, dep sources. **Git-ignored**;
  pinned by exact hash in `docs/summaries/00-project-brief.md`.
- `docs/summaries/` — project brief, decisions, handoffs (one active handoff).
- `docs/research/` — research reports (prior art, build surface, Wisp).
- `docs/archive/` — superseded docs; read only when told.

## Hard constraints
- Local git only. Never publish without explicit permission.
- No JIT in the guest engine — JS-heavy sites will be slow; that's accepted.
- wasm32 4 GB memory ceiling unless we adopt Memory64 (decide in Phase 0).
- Dev server must send COOP `same-origin` + COEP `require-corp`.

## Workflow
- Phase gates are fail-fast: Phases 0–2 exist to kill the approach cheaply if
  a load-bearing assumption breaks (see brief §Phases).
- Before modifying WebKit sources: record every patch as a `.patch` in
  `src/patches/` so the pinned checkout stays reproducible.
- Codex review before presenting any non-trivial code change.
