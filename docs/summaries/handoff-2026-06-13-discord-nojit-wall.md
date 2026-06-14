# Handoff — Discord freeze = the no-JIT wall (root-caused, measured, UX-mitigated)

**Written 2026-06-13 ~19:40 EDT. ACTIVE HANDOFF.** Supersedes
`handoff-2026-06-13-timer-js-pacing.md` (banner'd; its premise was falsified).
Read THIS first.

## TL;DR
Discord's "smooth then freeze" is the **no-JIT CLoop tax on object hydration** —
NOT timer JS (task #77's old premise) and NOT memory. Proven by direct
instrumentation this session. Steady state is fine (~50fps). The freeze is a
single synchronous `WTF::RunLoop::cycle()` of up to **~10.8s** building Discord's
gateway-READY object graph (**~0.8MB compressed → ~95MB live JS objects**) at
**531MB / 4GB** (memory is fine). Payload-reduction levers are weak (payload is
tiny); the real fix is a JIT, which is practically impossible in wasm32. Shipped a
host-side freeze overlay (UX). A GPT-5.5 deep JIT-feasibility consult (#2) is
**PENDING** (task #80) — likely returns post-compact.

## Root-cause correction chain (each FALSIFIED by instrumentation — do not re-tread)
1. ❌ **Timer/microtask JS in `bib_pump`** ("pumpGap saturation" — old handoff/#77).
   FALSE: `bib_pump` cycle time is tiny (max ~30ms); the old `pumpGap=700–933ms/s`
   was the page-side perf-watch formula `gap=elapsed−Σ(tick phases)` mislabeling
   **`bib_pump_network`** (network-completion) cycles as timer JS.
2. ❌ **Memory / OOM.** FALSE: the freeze is at heap=531MB (13% of the 4GB ceiling),
   jsc~95–102MB. The earlier `RangeError: Out of memory` was **corrupt-persisted-
   profile bloat** from a hard crash, cleared via `?persist=clear`. IDB is
   **in-memory** (main.cpp:2041); only the OPFS blob `bib-state-v1.json`
   (cookies+localStorage) persists. `MAXIMUM_MEMORY` already 4GB.
3. ✅ **No-JIT object hydration.** A single synchronous `RunLoop::cycle()` of up to
   10.8s creates ~95MB of JS objects from the ~0.8MB gateway READY. Compute-bound.
   A JIT would do it in ~300ms. This is the brief's *accepted* no-JIT tax.

## Decisive measurements (this session — direct BIBPERF instrumentation)
- **Steady-state GPU Discord**: ~50fps, heap 307MB, jsc~15MB, ~50% busy. Healthy.
- **The freeze**: `pump=11064(max10811 n47)`, also `runloop(JS)=2693`, `=2995` —
  single cycles of 3–10.8s, `painted=0`.
- **Memory at freeze**: heap=531MB, jsc=95–102MB. NOT memory-bound.
- **Gateway payload**: `gateway.discord.gg = 0.78–0.84MB` total (compressed; these
  are TLS-encrypted stream bytes ≈ compressed size). **TINY** → payload-size
  reduction has a low ceiling. (discord.com app bundles = ~10MB; that's a one-time
  parse, separate from the gateway hydration.)
- **Engine baseline** (headless landing, 60s): 369MB flat, NO leak; present path
  leak-clean (1200+ frames, no growth).

## Levers (GPT-5.5 consult #1 [docs/research/research-04] ∩ measurement)
- **Payload-size reduction (`large_threshold`)**: WEAK — payload already 0.8MB;
  can't dent a *compute*-bound 10.8s. Deprioritized. NOTE GPT-5.5 said "proxy-
  rewrite the Identify" — WRONG layer for us: **TLS terminates in-engine**, so a
  rewrite must hook WebCore's WebSocket *send* path, not the Wisp proxy.
- **Persistent IndexedDB → Discord `client_state` caching**: the ONE remaining
  "do-less-work" bet, but UNCERTAIN. Helps only if Discord *skips re-hydrating*
  cached guilds (avoids object creation), not merely receives fewer bytes. IDB is
  in-memory now (main.cpp:2041); OPFS-back it like the cookie jar (#67). **Spike +
  validate Discord actually skips hydration before investing.**
- **UX framing**: ✅ DONE this session (host-side overlay).
- **JIT**: the real fix, but NOT practically achievable in-tab (GPT-5.5 deep consult
  #2 — `docs/research/research-05-jit-feasibility.md`, confidence 0.8). Native JIT
  impossible (wasm W^X). JIT-to-wasm = a 6–18-month NEW JSC execution tier (JSValue
  layout, GC write barriers, inline caches, OSR/deopt, exception ABI, runtime helper
  ABI, cross-module calls) — NOT a spike. A helper-heavy bespoke "hot-function→wasm"
  tier is *likely dead*: per-helper-call boundary cost ~0.5–5µs × millions of object
  ops = seconds (1M calls @1µs = 1s). Copy-and-patch (needs W+X) and wasm-GC (would
  require replacing JSC's object representation) both ruled out. The 10.8s→300ms gap
  is ~36× — dispatch reduction alone can't touch it; you'd have to inline object
  access/ICs/alloc across the whole hot graph = a real JIT.
- **Escape hatch**: remote browser w/ JIT, streamed. Against project spirit.

## Shipped this session — UNCOMMITTED (HEAD still 820316b; `commit only when asked`)
`git status --short`:
- **M src/embedder/main.cpp** — BIBPERF instrumentation, perflog-gated (zero default
  cost): `PerfAccum` pump/net per-cycle fields; timing in `bib_pump` +
  `bib_pump_network`; heap gauge (`emscripten_get_heap_size()` +
  `commonVM().heap.size()`) → line now has `heap=MB jsc=MB … pump=T(maxM nN)
  net=T(maxM nN)`. Added `#include "CommonVM.h"` + `<emscripten/heap.h>`. **Built
  into the live binary** (relink 17:54). The `!g_perfLog` paths are byte-identical
  to the originals.
- **M web/browser.html** — (#78) per-host wisp byte accounting
  (`window.__bibStreamBytes`, `Object.create(null)`) + perflog `wisp bytes/host:`
  dump; (#79) host-side freeze overlay (`#screenwrap`/`#bibfreeze` + spinner CSS +
  `bibFreezeUX` watchdog on `__bib.frames`, document-level input gating, generic
  wording "Loading…"/"Working…"/"Engine stopped"). **Codex-clean** (2 findings
  fixed: document-level input listeners; null-proto map). **gate2 + gate8 PASS.**
  Host-side — served live, no rebuild.
- **M docs/summaries/handoff-2026-06-13-timer-js-pacing.md** — SUPERSEDED banner.
- **?? docs/research/research-04-discord-nojit-consult.md** — GPT-5.5 consult #1 verbatim.
- **?? web/probe/wasmrepro/** — pre-existing, LEAVE ALONE.
- build/ (gitignored): `pump-shape.mjs` (raw BIBPERF dumper), `gpt55-discord-nojit-brief.md`,
  `gpt55-jit-*` brief; logs `/tmp/gpt55-*.log`.
- Memory (outside repo): `discord-freeze-rootcause-nojit.md` (NEW),
  `rendering-update-throttle.md` (corrected), `MEMORY.md` (index).

When the user asks to commit: stage main.cpp + browser.html + the banner + research-04
(NOT wasmrepro). Theme: "Discord no-JIT freeze: BIBPERF diagnostics (pump/net/heap)
+ host freeze-overlay UX; root-cause corrected (compute-bound hydration)."

## Open / next steps
1. **THE one in-tree spike worth doing (GPT-5.5 #2's actionable lead).** Hypothesis:
   our pinned wasm32/CLoop config may be missing/under-using a basic monomorphic
   `get_by_id`/`put_by_id`/object-alloc inline-cache fast path (CLoop *should* have
   ICs — verify they're actually active under wasm32). Falsifiable, cheap-ish:
   (a) add CLoop opcode/object-op sampling to see where the 10.8s actually goes at the
   bytecode level; (b) build a **synthetic READY-hydration benchmark** (many
   guild/channel/member-shaped objects, Map/Set, spreads) as a regression harness —
   validate CLoop is ~20–40× slower than JIT without Discord noise; (c) implement ONE
   fast path (monomorphic `put_by_id` + write barrier) — if it moves the synthetic
   **≥1.5×**, continue to `get_by_id`/object-literal alloc; if **<1.2×**, the no-JIT
   wall is DEFINITIVE. Realistic payoff: 5–20% if ICs already decent; **1.2–1.8× on
   real Discord only if the CLoop path is accidentally generic/slow for wasm32**
   (conf 0.45). PARALLEL cheap kill-test: a JIT-to-wasm "boundary microbench" (measure
   helper-call + module instantiate cost in our exact PROXY_TO_PTHREAD/shared-mem
   setup) — if helper calls >0.5µs, the wasm-tier idea is dead in days. (GC/allocator
   tuning is measurement-gated: only if `JSC_logGC`/alloc counters show >20% in the
   READY window; 5–25% ceiling.) Full detail: research-05 §10 (5 experiments).
2. **Commit decision** (user). Now also stage `docs/research/research-05-*`.
3. **IDB-persistence spike** — the one uncertain *work-reduction* lever (reduce
   hydrated object COUNT, not bytes; payload-size reduction is weak). Validate Discord
   actually skips re-hydration with cached `client_state` first.
4. **#57 toObjectSlow** abort still open (intermittent, mitigated by recovery).

## Tools / commands
- Measure: `DISPLAY=:0 BIB_HEADED=1 BIB_CHANNEL=chromium node build/pump-shape.mjs https://discord.com 1`
  (raw BIBPERF incl. heap/pump/net). Raster: gpu arg `0`, headless.
- Live logged-in: `localhost:8080/browser.html?gpu=1&perflog=1&url=https://discord.com`
  → `wisp bytes/host:` sizes payload; overlay shows on freeze. `?persist=clear` resets.
- Build (relink): `systemd-run --user --scope -p MemoryMax=12G -p MemorySwapMax=0 --collect -- env BIB_JOBS=6 bash tools/build-webcore.sh`
- Gates: `node tools/gate2-browser-test.mjs`; `DISPLAY=:0 node tools/gate8-gpu-test.mjs`.

## Honest bottom line
This is the no-JIT wall the brief accepts. Discord IS usable: one-time ~10s sync
(now shown as "Loading…"), then ~50fps with sub-second channel-switch stutters.
Real speedup needs a JIT (pending #80's verdict) or accepting the tax. **Do not
chase timer-JS pacing or memory shedding — both falsified by measurement.**
