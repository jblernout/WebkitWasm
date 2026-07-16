# WebkitWasm

--- Fun facts about this port
- It was done by mostly fable 5, opus 4.8 and GLM 5.2, the inital "working" version took 6 hours.
- Took approximately $5000 in API costs (But easily doable on the single $200/month anthropic sub)
- The port also contains docs, md files, recaps, from my session and workflow with claude making it easier for anyone to see progress and work with agents to build it out
- Sister project and more polished port of gecko by a friend here: https://github.com/HeyPuter/firefox-wasm
- If I have time i may continue some work on it, it currently has some known issues (for example, video support) but the engine port itself is actually moderately fast, possibly more so then the gecko port due to webkit being lighter and more portable to begin with.

A full modern browser engine running **inside a browser tab**. WebKit
(WebCore + JavaScriptCore) is compiled to WebAssembly with Emscripten — no VM,
no x86 emulation. The page hosts an interactive `<canvas>` that *is* the
engine's renderer, and all networking leaves the tab over the **Wisp** protocol.

It boots, paints through Skia, runs guest JavaScript (JIT-less), keeps cookies +
localStorage across reloads, and loads real sites — including a live Discord
session — entirely within the wasm engine.

> Status: research prototype. Single-process, WebKit1-style embedding against
> internal headers. JS runs on the JSC **CLoop interpreter (no JIT)** — JS-heavy
> sites are slow by design (wasm can't self-JIT conventionally).

## Quick start

A clone ships only our ~15 MB of source. The ~11 GB engine tree (WebKit, emsdk,
built wasm deps) is gitignored and recreated by the bootstrap:

```bash
bash tools/bootstrap.sh          # clone pinned WebKit + emsdk, build wasm deps (~30-90 min, ~15 GB)
bash tools/build-webcore.sh      # build the engine (WebCore + embedder) -> build/webcore/bin
npm run wisp &                   # Wisp network server on 127.0.0.1:5001
node tools/dev-server.mjs web --mount /engine=build/webcore/bin   # serves on :8080 with COOP/COEP
```

Then open **`http://localhost:8080/browser.html?url=https://example.com`**.

Requirements: Linux host with `git curl cmake ninja make python3 pkg-config tar
xz unzip` and a C/C++ compiler; `node` for the dev/Wisp servers. See
[`BUILD.md`](BUILD.md) for the full walkthrough and troubleshooting.

## Branches

| Branch | Engine | Use it for |
|---|---|---|
| **`main`** | pthread GPU (`-sPROXY_TO_PTHREAD`) | daily driver — the host tab stays responsive while the engine runs on a worker. Smooths heavy-JS sites. |
| **`non-pthread`** | main-thread GPU (`BIB_PTHREAD=0`) | max graphics throughput (MotionMark ~109) and no-SharedArrayBuffer hosts. Trade-off: a long guest-JS task freezes the whole tab. |

Same code; the branches differ only in the default build flag. `wb1-pthread` is
an early pthread snapshot kept for reference.

## How it works (one paragraph)

WebCore + JSC are built as a custom `PORT=Emscripten` (CLoop, `ENABLE_JIT=OFF`).
Rendering is Skia → the engine owns a WebGL2/Ganesh context and presents to the
page `<canvas>` (directly on `non-pthread`, via an OffscreenCanvas transfer on
`main`). Networking is WebKit's curl backend (curl 8.17 + OpenSSL 3.5 + nghttp2)
over Emscripten SOCKFS, with wisp-js swapped in as the transport — TLS
terminates inside the engine. Persistence is an OPFS-backed profile. Full
rationale in [`docs/summaries/`](docs/summaries/) (decision-00x docs).

## Serving requirement

The host page **must** be served with COOP `same-origin` + COEP `require-corp`
(for SharedArrayBuffer / pthreads). `tools/dev-server.mjs` does this. Static
hosts that can't set those headers must use the `non-pthread` build.

## Licensing

This repo contains only original code (embedder, host page, tooling) plus
`src/patches/webkit-emscripten.patch`. It does **not** redistribute WebKit —
`bootstrap.sh` fetches it from upstream. WebKit is **LGPL-2.1 / BSD**; the patch
is a derivative of WebKit source and inherits those terms. Choose a license for
the original code before going public — see [`LICENSING.md`](LICENSING.md).
