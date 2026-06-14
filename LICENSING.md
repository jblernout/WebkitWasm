# Licensing notes

This is a research prototype. Licensing isn't finalized — this file lays out the
situation so it can be settled before the repo goes fully public. (For a private
repo with invited collaborators it's lower-stakes, but read this first.)

## What this repo actually contains

- **Original work** (ours): `src/embedder/`, `web/`, `tools/`, `docs/`. You
  choose the license for these.
- **`src/patches/webkit-emscripten.patch`** — a diff against WebKit source. It
  is a **derivative of WebKit** and inherits the license of each file it modifies.
- It does **not** contain WebKit, Skia, curl, ICU, OpenSSL, etc. Those are
  fetched from upstream by `tools/bootstrap.sh` and never committed here.

## Upstream licenses (fetched, not redistributed by us)

| Component | License |
|---|---|
| WebCore | **LGPL-2.1** |
| JavaScriptCore, WTF, bmalloc | **BSD-2-Clause** |
| Skia | BSD-3-Clause |
| curl | curl (MIT-like) · OpenSSL | Apache-2.0 · ICU | Unicode/ICU |
| zlib, libpng, libjpeg-turbo, libwebp, freetype, harfbuzz, libxml2, sqlite, nghttp2, brotli, libpsl, fontconfig | individual permissive licenses |

All permissive and mutually compatible. The one with copyleft reach is **WebCore
(LGPL-2.1)** — the embedder statically links it.

## The thing to decide before going fully public

The embedder statically links LGPL-2.1 WebCore. LGPL static linking obliges you
to let recipients relink against a modified WebCore (e.g. ship the embedder
object files, or document the build well enough to rebuild — which `BUILD.md` +
`bootstrap.sh` largely already do). The patch hunks themselves remain LGPL/BSD.

**Recommendation:** license the original code **BSD-2-Clause** (matches JSC/WTF,
permissive, no friction), and add a top-level `NOTICE` pointing at WebKit's
LGPL-2.1/BSD and the per-dep licenses. To adopt:

```bash
# when ready (not done automatically — your call):
#   add a LICENSE file (BSD-2-Clause, your copyright)
#   add a NOTICE file crediting WebKit (LGPL-2.1/BSD) + the deps
```

Until a `LICENSE` exists, default copyright applies (all rights reserved) — fine
for a private repo, **not** for public distribution.
