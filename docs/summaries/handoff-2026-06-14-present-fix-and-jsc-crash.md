# Handoff — present-depth fix shipped + JSC `materializePropertyTable` crash (fix plan)

**Written 2026-06-14 ~00:20 EDT. ACTIVE HANDOFF.** Supersedes
`handoff-2026-06-13-discord-nojit-wall.md` (still valid for the no-JIT/JIT verdict
background; this one is the live thread). Read THIS first.

## TL;DR
- **Shipped this session (host-side, NO rebuild):** GPU present default `maxInFlight`
  **3 → 1** in `web/browser.html`. Depth-3 flooded the page main thread under load and
  caused a **random delivery freeze** (the "can't replicate" stutter). Depth-1 is
  steady. Codex-reviewed clean. Also: freeze overlay → small corner badge.
- **THE THING TO FIX NEXT (priority):** an intermittent **JSC engine crash** on Discord —
  `materializePropertyTable` offset inconsistency (off by exactly 1). Localized; full
  fix plan in Part A. **Leans hard** (cheap escapes ruled out — see below).
- **Open present refinement:** `tick-on-ack` (needs a relink) to close a hard-graphics
  dead-gap before depth-1 is fully production-safe (Part B).
- The score↔smoothness conflict is a **single-thread bound** — two modes, not one knob.

---

## Part A — THE CRASH (fix plan, priority)

### Symptom / repro
Intermittent engine abort during **Discord initial JS load** (`ticks ~1800-1900`, after
the wisp streams are fetched, mid script-execution). The `#bibfreeze` overlay shows
"Engine stopped — Reload." User reports it's frequent/annoying. Occurs at **rcap=0 AND
rcap=dynamic, inflight=1 AND 3** → independent of the present settings (it's JSC, and the
engine binary was NOT rebuilt this session).
- Repro: `localhost:8080/browser.html?url=https://discord.com` (dev server on :8080), logged in. Reload a few times.

### The abort (verbatim shape, two samples)
```
Detected offset inconsistency: numberOfSlotsForMaxOffset doesn't match totalSize!
  maxOffset=96  m_inlineCapacity=6  numberOfSlotsForMaxOffset=39  totalSize=38
  inlineOverflowAccordingToTotalSize=32  numberOfOutOfLineSlotsForMaxOffset=33
  Detected in materializePropertyTable.  -> Aborted()
(earlier sample: maxOffset=64, numberOfSlotsForMaxOffset=7, totalSize=6)
```
**Off by EXACTLY 1, every time.** Decode: `maxOffset` implies 33 out-of-line slots, but
the table has 32 live (`totalSize 38 − inlineCapacity 6 = 32`). → **one "phantom"
out-of-line slot**: an offset reserved by `maxOffset` with no live property entry. The
consistency (always +1) argues a **logic bug**, not random memory corruption.

### Localized (exact code)
- Abort site: `Structure::materializePropertyTable` → `checkOffsetConsistency`,
  `third_party/WebKit/Source/JavaScriptCore/runtime/Structure.cpp:483`.
- The check: `StructureInlines.h:433-464` — `ALWAYS_INLINE`, **NOT** `#if ASSERT_ENABLED`
  (that guard starts at line 474 for the *next* fn). It only early-returns on
  `isCompilationThread()`. We have **no JIT → no compilation thread → it runs fully on the
  main thread**, so this is a real main-thread inconsistency (not a concurrent-read
  false positive).
- `materializePropertyTable` (`Structure.cpp:419-496`) rebuilds the table by **replaying
  the structure transition chain** (the long `structures = 0x...` list in the dump):
  - PropertyAddition → `table->add` (`:452`)
  - **PropertyDeletion → `table->take` + `table->addDeletedOffset(transitionOffset)` (`:462-468`)  ← PRIME SUSPECT**
  - AttributeChange / SetBrand.
- Discord deletes object properties constantly → the phantom out-of-line slot smells like
  **deleted-offset / `maxOffset` rollback** bookkeeping.

### Difficulty: medium-HARD (cheap escapes ruled out)
- ❌ **Build-flag dodge** (assertions off): the check is **always-on** and our build is
  already `CMAKE_BUILD_TYPE=Release` (tools/build-webcore.sh:91) — it still fires. No flag.
- ❌ **Backport a known upstream fix**: our WebKit pin is **`aec9d2ad958e716ab4bca4bf03007e6edac7323f`
  dated 2026-06-07** (near head; HEAD is itself a cherry-pick of bug 316190). Only a
  ~1-week upstream window (Jun 7→14) could contain a newer fix. Worth a quick check (Step 3)
  but don't count on it.
- ⚠️ **Band-aid (do NOT ship):** patch out `checkOffsetConsistency` → crash vanishes but the
  property table is *genuinely* wrong → silent wrong property reads / worse corruption later.
- → Most likely a **real bug exposed by our config** (no-JIT CLoop + wasm32 + pthread) OR
  **one of our `src/patches/`** OR (less likely, given the consistent off-by-one) memory
  corruption. Any real fix = WebKit-source edit → `.patch` → **full WebCore/JSC rebuild**
  (~tens of min) per iteration.

### Step-by-step
1. **Capture 3-5 abort dumps** (logged-in Discord, reload). Confirm always off-by-exactly-1,
   always `materializePropertyTable`; record `maxOffset`/`totalSize` each time. `?inflight=3`
   to re-confirm present-independence.
2. **Rule out our patches first.** `ls src/patches/` — review any touching
   `JavaScriptCore` / `Structure` / `PropertyTable` / `JSObject` / property deletion. A local
   patch is the fastest-to-confirm culprit.
3. **Quick upstream check (cheap, read-only).** WebKit Bugzilla + git for
   `materializePropertyTable` / `numberOfSlotsForMaxOffset` / "offset inconsistency" /
   deleted-offset fixes landed **after 2026-06-07**. If found → cherry-pick → `src/patches/`
   → rebuild → done (the only "easy" outcome).
4. **If not upstream: pin the mechanism.** Read `materializePropertyTable` (`Structure.cpp:419`)
   + the delete path (`:462-468`) + `PropertyTable::take` / `addDeletedOffset` /
   `propertyStorageSize` + wherever delete transitions are *created* and where `maxOffset` is
   set on deletion. The phantom slot = an offset in `maxOffset` not in the live table → find
   where a deletion fails to roll back `maxOffset` (or `addDeletedOffset` double-counts). Add a
   targeted `dataLog` at the delete-transition CREATE site to catch the divergence moment.
5. **Decide corruption vs logic.** If it isn't a clean logic bug, build with
   `-fsanitize=address` to catch an OOB write stomping structures (Skia/curl/font/embedder).
   Consistent off-by-one makes this less likely — do it only if Step 4 stalls.
6. **Fix + validate.** Patch JSC → `bash tools/export-webkit-patches.sh` → rebuild → repro
   gone + `gate2`/`gate8` green + 5-site sweep + repeated Discord loads. **Codex-review the
   patch.**

### Probable link: #57
Task **#57** (`toObjectSlow` via for-in / `slow_path_get_property_enumerator` abort) is the
SAME family — JSC structure/property handling under CLoop. Fixing one may fix both; treat
them together.

### Key files + lines
- `third_party/WebKit/Source/JavaScriptCore/runtime/Structure.cpp:419` (materializePropertyTable),
  `:462-468` (delete path), `:483` (checkOffsetConsistency call).
- `.../runtime/StructureInlines.h:433-464` (checkOffsetConsistency — always-on).
- `.../runtime/PropertyTable.h` (take / addDeletedOffset / propertyStorageSize);
  `numberOfSlotsForMaxOffset` (PropertyOffset.h / StructureInlines.h).
- `src/patches/` (our WebKit patches — check for JSC/Structure ones).

### Tools / commands
- Repro: `localhost:8080/browser.html?url=https://discord.com`
- Rebuild: `systemd-run --user --scope -p MemoryMax=12G -p MemorySwapMax=0 --collect -- env BIB_JOBS=6 bash tools/build-webcore.sh`
- Patch export (after editing the WebKit tree): `bash tools/export-webkit-patches.sh`
- Gates: `node tools/gate2-browser-test.mjs` ; `DISPLAY=:0 node tools/gate8-gpu-test.mjs`
- WebKit pin: `aec9d2ad958e716ab4bca4bf03007e6edac7323f` (2026-06-07).

---

## Part B — present / throttle state

### Shipped this session
- **`web/browser.html` present default `maxInFlight` 3 → 1** (the receiver always sends
  `{t:"config", maxInFlight:1}`; `?inflight=N` overrides, 1-8). Host-side, no rebuild.
- **Overlay → corner badge** (bottom-right, no canvas dim; `#bibfreeze` CSS). Host-side.
- `src/embedder/main.cpp` BIBPERF pump/net/heap instrumentation — from the PRIOR session,
  already in the live binary (17:54 relink), still uncommitted.

### Why (measured — `build/present-cadence.mjs`, Discord, headed real Intel GPU; research-06)
inflight A/B, `cv` = per-bucket fps coefficient of variation (lower=smoother):
- **rcap=dynamic:** inflight=1 → cv 0.76/0.78/0.78 (STEADY); inflight=3 → cv 0.62/0.73/**4.26**
  (BIMODAL, one near-frozen) = the random stutter.
- **rcap=0:** inflight=1 → 17-38fps; inflight=3 → 0.1-3fps (near-frozen). Depth-3 floods.
- Mechanism: early-credit (page acks `{t:"ready"}` BEFORE `transferFromImageBitmap`,
  `browser.html:170`-before-`:173`) + depth-3 lets the engine post faster than the page drains
  → collapse. Depth-1 self-paces.

### Validated next step — `tick-on-ack` (relink) [GPT-5.5 + Codex Q5 both flagged]
Depth-1 has a residual **dead-gap** on hard-graphics: the `{t:"ready"}` handler decrements
credit but does NOT reschedule a tick, so after a credit-exhausted skip the engine idles
until the next host rAF (~16ms/frame). Likely also part of the user's "rcap=0 still stuttery."
- Fix in `web/engine-pre.js`: in `bibBitmapPresentReady` (`:181-184`) set a `creditStalled`
  flag when it returns false; in the `{t:"ready"}` handler (`:212`) after `p.inFlight--`, if
  `creditStalled`, clear it and schedule **one** `_bib_tick` (e.g. `setTimeout(_bib_tick,0)` /
  queueMicrotask) so the pending dirty frame presents without waiting for rAF. Gate on the flag
  so it does NOT free-run.
- Also set `engine-pre.js` `maxInFlight` default `3 → 1` (`:157`) in the same relink (browser.html
  currently overrides it; make the engine default match).
- Needs a **relink** (engine-pre.js is `--pre-js`). Then re-measure with present-cadence.mjs +
  eyeball hard-graphics for the lock-to-1fps.

### Future — delivery-first throttle controller (GPT-5.5 rec #2, the real "better dynamics")
Replace `rcap = EMA(updateRendering)/0.33` (budgets updateRendering ONLY) with a controller
that budgets the **whole frame** — `targetInterval − EWMA(layout+paint+present) − safety`,
present-before-update when overdue, raise target when cv low & busy<80%, cut on
ack-latency/inflight pressure. All those terms are already in BIBPERF. Medium effort.

### The bound (don't re-litigate)
Score↔smoothness is a single-engine-thread tradeoff. **Two modes:** `rcap=0` = benchmark
(high MotionMark, stuttery — saturation), `rcap=dynamic` = daily-driver (smooth, lower score).
MotionMark score = guest internal rAF, decoupled from host delivery. Full reasoning +
all GPT-5.5 recs (present-worker = low payoff, LTO later, INITIAL_MEMORY 256→768/1024 knob,
CLoop IC spike, APZ-lite scroll) in `docs/research/research-06-perf-present-consult.md`.

---

## Part C — uncommitted state + commit guidance (commit ONLY when asked)
`git status` highlights:
- **M `web/browser.html`** — overlay corner-badge (#79) + present default inflight=1 + comments. Codex-clean.
- **M `src/embedder/main.cpp`** — BIBPERF pump/net/heap (prior session, in live binary).
- **?? `docs/research/research-04…`/`-05…`/`-06…`** — GPT-5.5 consults #1 (Discord no-JIT),
  #2 (JIT feasibility), #3 (perf+present) verbatim.
- **?? `build/motionmark-cadence.mjs`**, other `build/*.mjs` — gitignored, leave.
- **?? `web/probe/*`** (pacifico.woff2, probe-serif.ttf, webfont.html, wasmrepro/) — pre-existing, leave.
- `docs/summaries/handoff-2026-06-13-timer-js-pacing.md` (superseded banner, prior session).

When asked to commit: Codex-review, then stage `web/browser.html`, `src/embedder/main.cpp`,
`docs/research/research-04/05/06`, the handoffs. NOT `build/`, NOT `web/probe/wasmrepro`.
Suggested split: (1) "present: default to strict one-in-flight (kill depth-3 flood
non-determinism) + corner-badge overlay"; (2) "BIBPERF pump/net/heap diagnostics"; (3) docs.

---

## Part D — other open items
- **#57** `toObjectSlow` abort — same JSC structure family as Part A (link them).
- **#29 / #30** — perf + site-support epics (umbrella).
- No-JIT wall (research-05): the CLoop monomorphic inline-cache fast-path spike is the one
  remaining JS-perf lever — SEPARATE from the crash; gate on a synthetic hydration benchmark.
- **#77** Discord one-time ~10s freeze = no-JIT gateway-READY hydration (accepted tax; distinct
  from both the stutter and the crash).

## Honest bottom line
The present non-determinism is **fixed** (depth-1, shipped). The real blocker for daily
Discord is the **`materializePropertyTable` JSC crash** — localized, probably a deleted-offset
logic bug, leaning hard (no cheap escape). Start at Part A Step 1. `tick-on-ack` is the small
present follow-up; the delivery-first controller is the bigger perf win after.
