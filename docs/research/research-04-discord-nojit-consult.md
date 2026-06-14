# Research 04 — GPT-5.5 Pro consult: no-JIT Discord gateway freeze (2026-06-13)

Verbatim consult response. Conversation context + our synthesis live in task #77 and memory discord-freeze-rootcause-nojit. Brief sent: build/gpt55-discord-nojit-brief.md.

---

0. Bottom line

This is mostly the no-JIT wall. Given the brief’s constraints, wasm32, CLoop-only JSC, one VM on one engine thread, and freezes occurring inside one synchronous WTF::RunLoop::cycle(), there is no generic scheduling trick that can make Discord’s in-progress JS handler yield, paint, or accept input mid-handler. The measured 10.48s READY freeze is exactly the kind of object-heavy SPA workload a JIT hides and an interpreter exposes.

The highest-feasibility answer is:

1. Shrink or split Discord’s gateway startup payload.
2. Use host-side UX framing while the engine thread is blocked.
3. Instrument JSC to verify whether a native primitive is actually hot.
4. Only then chase native accelerators or CLoop micro-optimizations.

Everything else is likely incremental or research-project scale.

Feasibility-ranked levers:

1. Gateway Identify and READY payload reduction: large_threshold, capabilities, client-state caching, lazy guild loading.

* Mechanism: make Discord send less initial gateway state, or send less critical state later.
* Expected payoff: potentially 2x to 10x on worst accounts if READY size drops.
* Effort/risk: medium; protocol-fragile.
* Survives constraints: yes. This is the best real wall-time lever because it reduces work before it enters the single no-JIT VM.

2. Prioritized or split READY plus deliberate network pacing.

* Mechanism: split startup state into smaller event deliveries and allow paints between them.
* Expected payoff: may turn one 10s freeze into smaller freezes with visible progress between chunks.
* Effort/risk: medium; depends on what Discord already negotiates and whether events can be delayed safely.
* Survives constraints: yes, if the monolith is actually multiple message events batched into one RunLoop cycle. It cannot split a single already-running JS handler.

3. Host-side freeze overlay plus input quarantine, cancel, and restart controls.

* Mechanism: the host page remains alive while the engine worker is blocked, so show an overlay outside the guest engine.
* Expected payoff: does not reduce CPU time, but makes a 10s stall survivable and legible.
* Effort/risk: low.
* Survives constraints: yes. It does not require touching Discord JS or the JSC VM.

4. JSC-native profiling: opcode, allocation, GC, and host-function timing.

* Mechanism: instrument JSC and WebCore from inside the engine rather than relying on a JS profiler.
* Expected payoff: not a fix by itself, but tells you whether any native accelerator exists.
* Effort/risk: medium.
* Survives constraints: yes. This is required before serious low-level optimization.

5. Native zlib, TextDecoder, or decompression fast path.

* Mechanism: if Discord is doing gateway inflate or decode in interpreted JS, move that primitive to native C++ or WebCore.
* Expected payoff: 0% to 30%, only if inflate or decode is hot.
* Effort/risk: medium-high.
* Survives constraints: conditionally yes. It only helps if the hot work is behind an interceptable API boundary.

6. JSC heap, allocation, and GC tuning.

* Mechanism: reduce collector pressure and allocation overhead during READY object hydration.
* Expected payoff: 5% to 25% if READY triggers GC churn.
* Effort/risk: medium.
* Survives constraints: yes. Memory headroom exists in the trace, but it will not close the JIT gap.

7. C++ fast paths for Object, Array, Map, and Set builtins.

* Mechanism: special-case common builtins and object operations so the interpreter does less slow generic work.
* Expected payoff: 5% to 30% in narrow cases.
* Effort/risk: high.
* Survives constraints: partly. It works only where the operation is self-contained and does not call arbitrary JS.

8. CLoop or wasm interpreter dispatch optimization.

* Mechanism: improve bytecode dispatch, case ordering, inline cache use, and low-level CLoop throughput.
* Expected payoff: usually single digits to maybe 20%.
* Effort/risk: high.
* Survives constraints: yes, but cannot close a 10x to 50x JIT gap.

9. Second JSC VM or pthread offload of Discord logic.

* Mechanism: move heavy JS computation into another VM or thread.
* Expected payoff: near zero for the Flux/store work.
* Effort/risk: very high.
* Survives constraints: no for the main problem. Discord’s live app state, object identity, stores, closures, and DOM-facing state are bound to the single VM.

10. JIT-to-wasm or host-JS acceleration sidecar.

* Mechanism: compile hot JS to wasm modules or host-browser JS and call back into the engine.
* Expected payoff: could be large in theory.
* Effort/risk: extreme.
* Survives constraints: not as a practical mitigation. This is a research browser-engine project, not a tactical fix.

1. Native-accelerable sub-primitives

What can actually be accelerated?

The only native accelerators worth chasing are primitives where Discord’s code currently spends meaningful time in interpreted JavaScript but whose semantics can be preserved behind a standard browser or JSC API boundary.

Good candidates to measure:

* Gateway decompression.
  Discord gateway transport compression supports zlib-stream and zstd-stream. zlib-stream uses a shared zlib context and messages are complete only after the 00 00 ff ff sync-flush suffix. If Discord’s web client is inflating gateway bytes with a JS library such as pako, replacing that with native zlib could be real. But if the cost is already native, or if inflate is only a small fraction of READY handling, it will not move the 10.5s number much.

Mechanism:
Intercept the WebSocket gateway byte path before Discord JS sees the message. Maintain the zlib-stream inflate state natively in C++ or in a native WebCore path. Deliver already-inflated text or binary messages to the guest. If Discord currently does JS-side inflate, this removes a large interpreted loop.

Expected payoff:
Possibly meaningful if JS inflate is hot, maybe 10% to 30% in the optimistic case. If the trace already shows JSON.parse is native and object hydration dominates, decompression may be only noise.

Risk/effort:
Medium-high. You must exactly preserve Discord gateway compression semantics, frame boundaries, close behavior, reconnect behavior, and error behavior. A subtle mismatch can break gateway state.

Survives constraints:
Yes, if this is pre-JS native work. It does not require a second VM, JIT, wasm64, or modifying Discord JS. But it only helps if the decompressor is actually hot.

* TextDecoder and UTF-8 decode.
  Worth timing, but likely not dominant. If WebCore already routes TextDecoder through native code, there is little left. If Discord receives ArrayBuffer frames and explicitly decodes them in JS, there may be a fast path.

Mechanism:
Ensure TextDecoder.decode, WebSocket text-frame conversion, and any UTF-8 path are native and not falling into slow interpreted JavaScript or avoidable copies.

Expected payoff:
Usually small, maybe 0% to 10%. Larger only if Discord does repeated JS decode of large gateway payloads.

Risk/effort:
Low to medium for instrumentation; medium for patching WebCore/JSC if a slow path exists.

Survives constraints:
Yes, but it is probably not the main wall.

* JSON.parse.
  The brief already says JSON.parse is native and not the primary issue. Do not spend time here except to confirm decoded byte size and parse duration.

Mechanism:
Time JSON.parse around each gateway message and verify it is a small fraction of the long RunLoop cycle.

Expected payoff:
Probably near zero, because this is already native.

Risk/effort:
Low for measurement, not worth deep optimization unless measurement contradicts the brief.

Survives constraints:
Yes, but likely irrelevant.

* RegExp.
  Probably not hot in READY hydration, but instrument it. JSC RegExp without JIT can still hurt in pathological code, but Discord store hydration is more likely property access, allocation, arrays, maps, and callbacks.

Mechanism:
Count and time RegExp compilation and execution during the long cycle.

Expected payoff:
Probably 0% to 5%, unless a specific minified parser path is unexpectedly regex-heavy.

Risk/effort:
Low for counters; medium for optimizing if hot.

Survives constraints:
Yes, but unlikely to dominate.

* Array, Object, Map, and Set builtins.
  These are the most plausible non-obvious primitive targets. Discord, React, and Flux code often does huge amounts of Object.keys, Object.values, object spread, Array.prototype.map/filter/reduce/forEach, array iteration, Map.set, Map.get, Set.add, shallow copies, and normalized-store construction. Some of that is implemented as native JSC builtins; some falls back into user callbacks and normal bytecode. The native fast path only helps where the operation is self-contained. It cannot accelerate arbitrary JS callback bodies.

Mechanism:
Instrument builtin entrypoints and bytecodes. If Object.keys, Object.assign, array copying, spread lowering, Map or Set operations, property enumeration, or structure transitions dominate, patch the corresponding JSC C++ slow path or add a wasm-friendly CLoop fast path.

Expected payoff:
Potentially 5% to 30% in narrow cases. The payoff is limited because many Array methods call arbitrary JS callbacks, and callbacks still run interpreted.

Risk/effort:
High. JSC object semantics are complex: prototypes, accessors, holes, symbols, dictionary mode, structure transitions, enumeration order, and observable side effects all matter.

Survives constraints:
Partly. It stays inside JSC and does not require changing Discord JS, but it is hard to make broad enough to crush the 10.5s wall.

What probably cannot be accelerated?

The main cost described in the brief is Discord’s JS constructing application store objects and dispatching them through Flux. That means millions of semantically visible JS operations: allocations, property puts, structure transitions, map updates, array pushes, function calls, and React or Flux subscriptions. A native C++ entry point cannot replace that unless Discord calls it or you rewrite Discord’s code path. Since you cannot modify Discord’s minified JS, the viable interception surfaces are standard APIs and WebKit/JSC internals, not making Flux native.

How to find the hot primitive without a JS profiler:

Add JSC-side sampling and counters, not browser devtools.

Minimum useful instrumentation:

1. Gateway event accounting before JS dispatch.
   For each WebSocket message, record compressed bytes, decompressed bytes, event name, object counts if cheaply skimmed, and timestamp. This gives facts like READY was X MB decoded, READY_SUPPLEMENTAL was Y MB, and GUILD_CREATE total was Z.

2. RunLoop long-cycle attribution.
   On entry to RunLoop::cycle(), mark cycle kind: tick, pump, or network. On every WebSocket message dispatch, push a current gateway event tag into thread-local state. When the cycle exits, attribute the whole span to READY, READY_SUPPLEMENTAL, GUILD_CREATE, or whatever event was being dispatched.

3. CLoop opcode sampling.
   Every N bytecodes or every N backedges, sample current CodeBlock, bytecode opcode, source URL, function name if available, and bytecode offset, line, and column.

You do not need exact profiling. If 70% of samples are in one minified Discord chunk doing get_by_id, put_by_id, call, construct, and new_object, the answer is payload and object churn, not zlib.

4. Allocation and structure counters.
   Count JSCell allocations by type, object allocations, butterfly growth, property table growth, structure transitions, dictionary-mode transitions, and GC pause time during the 10s window.

5. Host-function timing.
   Time native calls into JSON.parse, TextDecoder.decode, RegExp, Object.keys, Object.assign, array builtins, Map and Set methods, and WebSocket message construction.

Decision rule:

* If JS decompression is more than 20% of the long cycle, native decompression is worth it.
* If GC is more than 20%, heap and GC tuning is worth it.
* If native builtins dominate, patch those builtins.
* If samples are mostly Discord functions plus property/object opcodes, the only large win is reducing the data Discord asks the interpreter to process.

2. Offloading to a second thread

What can be offloaded:

* Native pre-work can be offloaded.
  Gateway decompression, byte buffering, zstd or zlib, UTF-8 validation, maybe even a native skim of event names can happen on another pthread before WebCore posts a WebSocket message. That keeps the engine responsive during decompression, but only until the JS onmessage handler starts.

Mechanism:
Create a gateway preprocessing worker or pthread. It receives raw WebSocket tunnel bytes, performs inflate or decode, identifies event boundaries, optionally tags event type, then schedules delivery to WebCore on the engine thread.

Expected payoff:
Useful only for native pre-JS work. If inflate and decode are hot, this can remove that part from engine-thread stalls. If object hydration dominates, payoff is small.

Risk/effort:
Medium. You must preserve ordering, backpressure, gateway close semantics, and error propagation.

Survives constraints:
Yes. It does not touch the single JSC VM except to deliver final events.

* Network pacing can be offloaded or controlled.
  If hostPump currently delivers multiple gateway frames into WebCore before one RunLoop::cycle(), enforce one high-cost gateway dispatch per paint opportunity for READY-adjacent frames. This does not help one monolithic READY handler, but it matters for READY plus READY_SUPPLEMENTAL plus many GUILD_CREATE events.

Mechanism:
Queue gateway events outside WebCore or at the WebSocket delivery layer. Deliver one event, let RunLoop cycle complete, then allow a bib_tick paint before delivering the next heavy event.

Expected payoff:
Potentially large for perceived responsiveness if the current 10s apparent monolith is actually multiple message events drained in one cycle. Zero for a single READY handler that itself takes 10s.

Risk/effort:
Medium. Pacing can perturb Discord’s expected startup sequence. It may also interact with heartbeat timing, ack timing, and sequence numbers.

Survives constraints:
Yes, but only between JS event dispatches. It cannot interrupt a running JS handler.

* Heartbeat protection can be offloaded.
  If Discord JS becomes blocked long enough to miss heartbeats, a native gateway shim could maintain sequence and heartbeat ACK state and keep the gateway alive. Discord’s gateway requires heartbeats and ACKs to maintain the connection.

Mechanism:
Have native gateway code observe sequence numbers and heartbeat interval, and ensure heartbeats continue even if the JS VM is blocked. Alternatively, avoid delivering work in a pattern that causes reconnect storms.

Expected payoff:
Does not fix UI freezes, but can prevent long stalls from cascading into reconnects, re-IDENTIFY, and another READY burst.

Risk/effort:
High if you actually spoof or own gateway heartbeat state, because you are now partially implementing a Discord client transport. Lower if you only delay delivery safely and preserve outgoing JS heartbeats.

Survives constraints:
Yes technically, but it is protocol-fragile.

What cannot realistically be offloaded:

* The Discord store and Flux work cannot move to another JSC VM.
  The handler closes over the live webpack module graph, stores, DOM-facing state, callbacks, and object identity in the main VM. A second VM cannot mutate those objects. Even if it could compute a transformed store snapshot, serializing it back would recreate the same object graph on the main VM.

Mechanism attempted:
Run READY handling or store construction in another JSC VM on another pthread.

Expected payoff:
Near zero for the real problem.

Risk/effort:
Very high. You would need to mirror huge parts of Discord’s JS state across VMs, preserve identity, and replay mutations into the main VM.

Survives constraints:
No. It violates the practical single-VM state constraint, even if pthreads exist.

* Guest Web Workers do not help unless Discord uses them for this path.
  You can make workers real instead of main-thread-emulated, but Discord’s READY hydration path is not magically moved there. The app would have to have been architected to send the data to a worker.

Mechanism attempted:
Implement real guest workers and hope Discord moves heavy startup work.

Expected payoff:
Near zero unless Discord already has a worker path disabled by your environment.

Risk/effort:
High if WebKit worker support inside this embedding is incomplete.

Survives constraints:
Not for the current path. It does not modify Discord JS and therefore cannot move Discord’s chosen main-thread handler.

* Rendering cannot be moved around the blocked JS stack.
  WebCore, JSC, and DOM state are not safely renderable from another thread while an event handler is mid-mutation. Asyncify-style suspension would make this a reentrancy minefield: half-mutated stores, pending microtasks, JSLock state, DOM invariants, and RunLoop nesting.

Mechanism attempted:
Paint from another thread or force a yield inside the interpreter.

Expected payoff:
Low to negative.

Risk/effort:
Extreme. Reentrancy bugs would be brutal.

Survives constraints:
No for generic UI interactivity. The host page can animate an overlay, but the guest engine cannot safely continue.

Verdict:
Offload native pre/post work, not Discord’s actual computation.

3. Reducing the payload

This is the highest-leverage path.

Discord’s official gateway model sends READY after Identify and includes the initial state required to interact with the platform. Gateway intents are explicitly designed to lower computational burden, and large_threshold is an Identify field that controls when offline guild members stop being sent. User-client-specific behavior, documented unofficially, includes capabilities, client_state, default large_threshold behavior, deduped users, Ready Supplemental, passive guild updates, and client-state caching.

The key point: because you cannot make CLoop process a huge object graph like a JIT, the best way to win is to prevent Discord from sending that huge object graph to the no-JIT VM in the first place.

Concrete experiments, in order:

A. Capture what Discord currently sends in Identify.

Mechanism:
Log the outgoing gateway Identify payload and the gateway URL. Specifically capture encoding, compress, large_threshold, capabilities, client_state, presence, properties, and whether the connection receives READY, READY_SUPPLEMENTAL, GUILD_CREATE bursts, and passive guild updates.

Expected payoff:
This is diagnostic, not directly a fix. It tells you whether Discord already uses modern payload-reduction features or whether your environment is causing a cold, heavy identify.

Risk/effort:
Low. It is mostly gateway logging.

Survives constraints:
Yes. It requires no Discord JS modification if done at the proxy/WebSocket layer.

B. Force large_threshold down.

Mechanism:
Proxy-rewrite the Identify payload so large_threshold is 25 instead of the default or current value. Then compare decoded READY size, member count, presence count, user count, channel count, and RunLoop cycle wall time.

Expected payoff:
Potentially large if the account is in many medium or large guilds. If time scales roughly with members plus presences plus users plus channels, this is your main wall-time knob. It could be a 2x to 10x win on pathological accounts.

Risk/effort:
Medium. Discord JS might assume the threshold it requested, although the client should already support lazy member and guild loading. You must test member lists, search, channel open, unread state, presence display, and guild navigation.

Survives constraints:
Yes. It reduces work before JS execution, so it directly survives wasm32, no-JIT, single VM, and no Discord JS editing.

C. Verify or force payload-splitting capabilities.

Mechanism:
Check whether Discord’s Identify capabilities include payload-splitting and dedupe features such as PRIORITIZED_READY_PAYLOAD and DEDUPE_USER_OBJECTS. PRIORITIZED_READY_PAYLOAD splits startup into READY and READY_SUPPLEMENTAL, with non-critical data sent later. DEDUPE_USER_OBJECTS moves user objects into a users array and replaces nested copies with IDs. If not enabled, experiment with enabling or preserving them. If enabled, pace the supplemental delivery.

Expected payoff:
Best case, first paint and first interactive shell happen after a much smaller READY, then supplemental data is delayed until after at least one bib_tick paint. This may not reduce total CPU but can significantly improve perceived responsiveness.

Risk/effort:
Medium-high. Capability bitfields are not stable public API for user clients. If Discord already opted into this and still freezes, the supplemental work may be the real 10s wall.

Survives constraints:
Yes if used as protocol shaping. It cannot split a single JS handler, but it can reduce or defer the handler’s input.

D. Persist and replay client_state.

Mechanism:
Make sure Discord’s persisted profile and gateway client_state survive reloads. If your profile persistence is imperfect, Discord may cold-identify every run and receive full state every time. Compare fresh profile, persisted profile, persisted profile with captured client_state, and Resume instead of Identify.

Expected payoff:
Potentially huge for repeat launches. Resume avoids a full cold READY path when valid. Good client_state may reduce unnecessary state delivery on re-identify.

Risk/effort:
Medium. Requires correct persistence of local storage, IndexedDB, cookies, session ID, resume gateway URL, sequence number, and any state Discord expects.

Survives constraints:
Yes. Avoiding cold READY is even better than optimizing it.

E. Gate op 14 and lazy guild subscriptions carefully.

Mechanism:
Discord client sends guild subscription updates for visible or relevant guild/channel state. The ideal no-JIT behavior is to only subscribe to the currently visible guild/channel at startup, then lazy-load others when the user navigates. If Discord JS sends broad subscriptions too early, delay them or batch them.

Expected payoff:
Potentially large for startup responsiveness and channel-switch freezes. It can move work out of the critical startup path.

Risk/effort:
High. More fragile than large_threshold. If Discord JS believes it subscribed but the proxy suppresses the op, UI state can silently go stale. Safer version: delay, do not drop. Let READY paint, then release guild subscriptions in small batches.

Survives constraints:
Partly. It does not require editing Discord JS, but it depends on protocol behavior and correctness.

F. Try browser/client-shape negotiation.

Mechanism:
Since you control WebKit and request properties, test whether Discord selects a lighter feature set under different User-Agent, device/browser properties in Identify, viewport/mobile layout, locale/accessibility settings, and experiment/capability combinations.

Expected payoff:
Unknown but cheap to test. If one variant naturally sends fewer capabilities, fewer subscriptions, or a lighter initial route, it can be a large win.

Risk/effort:
Low to medium. It may fail because Discord web may simply serve the same app or block mobile web.

Survives constraints:
Yes. This is input shaping, not runtime acceleration.

G. Intents.

Mechanism:
For bot gateway connections, intents are the standard way to reduce event classes. For a logged-in user web client, this is more constrained and not necessarily exposed in the same clean way. Still, inspect whether the user-client Identify includes intent-like selection or capability equivalents, and whether your environment is causing Discord to request more than normal.

Expected payoff:
High in principle, but probably limited for the official logged-in web client unless Discord exposes a compatible knob through capabilities/client state.

Risk/effort:
Medium. Easy to log, fragile to alter.

Survives constraints:
Yes if legitimate; otherwise protocol-fragile.

Overall verdict for payload reduction:
This is the best path. If the 10.5s is proportional to hydrated gateway state size, payload reduction is the only lever with plausible multi-x improvement while respecting all constraints.

4. Faster interpretation

This is the low-ceiling bucket.

Likely not worth much:

* SIMD.
  Mechanism:
  Compile with msimd128 or add vectorized helpers.

Expected payoff:
Very low for the main path. The hot path is object allocation, property lookup, function calls, branches, and pointer chasing. SIMD does not help that.

Risk/effort:
Low to medium, depending on build system.

Survives constraints:
Yes, but it will not materially fix Discord READY.

* Tail calls.
  Mechanism:
  Use wasm tail-call support or restructure interpreter dispatch around tail calls.

Expected payoff:
Low. A JS interpreter loop does not become a JIT because wasm tail calls exist. It may shave dispatch overhead in carefully designed interpreters, but Discord’s cost is not just dispatch.

Risk/effort:
High for uncertain gain.

Survives constraints:
Maybe technically, but not worth betting on.

* Threaded-code tricks.
  Mechanism:
  Use direct-threaded or token-threaded interpreter dispatch to reduce switch overhead.

Expected payoff:
Low to moderate. CLoop direct-threaded dispatch usually depends on compiler extensions and native indirect branches. In wasm, this typically becomes structured control flow or br_table-like dispatch. You are unlikely to get more than incremental wins.

Risk/effort:
High.

Survives constraints:
Partly. Wasm limits the traditional advantage.

* PGO and case ordering.
  Mechanism:
  Profile the CLoop interpreter on Discord traces and reorder hot opcode cases, inline hot helpers, and improve branch layout.

Expected payoff:
Single digits to low teens. Worth testing only if build infrastructure allows.

Risk/effort:
Medium to high, especially with your build-memory constraints.

Survives constraints:
Yes, but the ceiling is low.

* Bytecode caching.
  Mechanism:
  Cache parsed JavaScript bytecode for Discord chunks across reloads.

Expected payoff:
Useful for reload/startup parse and compile, not for the 10.5s READY processing. It may reduce earlier 3s app boot cycles, but not the gateway hydration wall.

Risk/effort:
Medium-high in JSC/WebKit.

Survives constraints:
Yes, but it targets the wrong phase.

* Wasm GC.
  Mechanism:
  Retarget JSC objects to wasm-GC or use wasm-GC features to accelerate object representation.

Expected payoff:
Not tactical. This is a different engine architecture.

Risk/effort:
Extreme.

Survives constraints:
No as a near-term mitigation.

Maybe worth testing:

* LLInt and CLoop opcode cache improvements.
  Mechanism:
  If sampling shows most time in get_by_id, put_by_id, get_by_val, new_object, construct, and call, investigate whether CLoop is using the same useful inline-cache metadata as other LLInt tiers. A C++-level monomorphic or polymorphic property fast path could matter.

Expected payoff:
10% to 30% in a very good case. Still not enough to turn 10.5s into 300ms.

Risk/effort:
High. JSC inline cache machinery is complicated and may assume JIT tiers for the best paths.

Survives constraints:
Yes, but with limited payoff.

* Allocation and GC tuning.
  Mechanism:
  If the READY wall includes frequent GC, spend memory to reduce collector pressure: larger initial heap, higher growth threshold during gateway startup, pre-reserved allocator arenas, or temporarily suppress aggressive collection while total memory is safe.

Expected payoff:
5% to 25% if GC shows up in traces. If GC is not visible, skip.

Risk/effort:
Medium. You have memory headroom in the measured trace, but wasm32’s 4GB ceiling still matters.

Survives constraints:
Yes.

* Research-scale JIT-to-wasm.
  Mechanism:
  In theory, a JIT inside wasm could generate new wasm modules and ask the host browser to compile them. In practice, making JSC’s Baseline or DFG emit wasm instead of native code is a major compiler backend project.

Expected payoff:
Could be large in theory.

Risk/effort:
Extreme. It must preserve JSValue representation, GC barriers, OSR, calls into JSC runtime, exception semantics, stack maps, inline caches, and DOM calls. For Discord’s object-heavy dynamic code, a naive compile-JS-to-wasm layer would still bounce through runtime calls constantly.

Survives constraints:
Not as a practical mitigation. It avoids native executable memory but becomes a huge new compiler/runtime project.

Verdict for faster interpretation:
There are incremental wins here, but nothing credible that turns a 10.5s interpreted Discord READY handler into a 300ms JIT-like handler. If the same object graph reaches the same CLoop VM, physics wins.

5. Perception and UX framing

Do this even if payload reduction succeeds.

Because the engine runs in a pthread or worker and the host page remains alive, the host page can animate DOM/CSS overlays while the engine canvas is frozen. That is your cleanest responsiveness story.

A. Freeze watchdog.

Mechanism:
Have the engine post a lightweight heartbeat after each successful paint or tick. If the host sees no paint acknowledgment for 250 to 500ms while Discord is starting or a large gateway frame was received, show an overlay over the canvas. The overlay should be host DOM/CSS, not engine-rendered.

Suggested plain text:
Discord is loading a large account state. This can take several seconds in no-JIT mode.

Expected payoff:
No CPU reduction, but a large perceived reliability improvement. Users understand the app is busy, not dead.

Risk/effort:
Low.

Survives constraints:
Yes. Host page is outside the blocked engine thread.

B. Input quarantine.

Mechanism:
While blocked, do not let clicks look lost. Capture them at the host overlay. Offer controls such as Cancel, Reload, Open performance log, and Continue waiting.

Expected payoff:
No faster computation, but avoids user frustration and repeated inputs queued into a frozen guest.

Risk/effort:
Low to medium.

Survives constraints:
Yes. You cannot make Discord respond during the JS handler, but you can keep the outer product responsive.

C. Pre-start hidden, reveal after READY.

Mechanism:
For Discord specifically, start the engine behind a loading shell. Do not show the frozen Discord canvas until after the first heavy READY or READY_SUPPLEMENTAL phase is done.

Expected payoff:
Turns app froze into app is loading. This is especially important for the first 10s startup freeze.

Risk/effort:
Low.

Survives constraints:
Yes.

D. Progress by phase, not fake percent.

Mechanism:
Display real milestones: connecting gateway, receiving READY, processing server/member state, rendering Discord. If you instrument decoded gateway bytes and event type, you can give honest hints like large server list detected.

Expected payoff:
Improves trust. Makes long waits tolerable.

Risk/effort:
Low to medium, depending on how much gateway instrumentation you expose to the host.

Survives constraints:
Yes.

E. Last-frame dimming.

Mechanism:
When blocked after the app is already interactive, dim the last canvas frame and overlay Processing Discord update so the user understands the app is busy, not dead.

Expected payoff:
Good perceived responsiveness for recurring 0.8s to 1.5s freezes.

Risk/effort:
Low.

Survives constraints:
Yes.

F. Hard timeout kill switch.

Mechanism:
If one cycle exceeds 20 to 30s, allow host-side termination of the engine worker. Since the engine thread is blocked, cooperative cancellation may not run. Terminating and restarting the worker is the reliable control.

Expected payoff:
Recovery from pathological stalls.

Risk/effort:
Medium. Needs clean profile/session recovery and a good user-facing explanation.

Survives constraints:
Yes.

Verdict for UX:
This is mandatory. It does not solve the CPU wall, but it lets the product feel intentionally slow instead of broken.

6. Structural things you may be missing

A. Measure slope against account size.

Mechanism:
Create a scaling curve across test accounts and states: 0 guilds, 1 small guild, 10 guilds, the same account after leaving/muting/hiding large guilds, large_threshold 250 vs 50 vs 25, with and without persisted client_state.

Plot decoded startup gateway bytes, member count, presence count, channel count, and JS cycle time.

Expected payoff:
This tells you whether the dominant term is payload size or fixed interpreter/runtime overhead.

Risk/effort:
Medium. Requires test accounts or controlled state.

Survives constraints:
Yes.

Interpretation:
If the slope is linear, the answer is payload reduction. If the intercept dominates, look at interpreter/runtime.

B. Make sure bib_pump_network is not batching multiple dispatches into one apparent monolith.

Mechanism:
Even though the brief says the worst freeze is one RunLoop::cycle(), verify whether that one cycle contains multiple queued WebSocket message events. If WebCore dispatches READY, READY_SUPPLEMENTAL, and GUILD_CREATE messages all in one cycle, change WebSocket delivery policy to one gateway event per pump and force a paint between them.

Expected payoff:
Potentially large for perceived responsiveness if the monolith is actually a batch of events. Zero if one READY event handler alone is 10s.

Risk/effort:
Medium.

Survives constraints:
Yes between event dispatches, no inside one handler.

C. Consider Discord-specific lite mode as a product feature.

Mechanism:
If gateway thinning works, expose a Discord no-JIT mode: lower large_threshold, delay non-visible guild subscriptions, use persisted client_state aggressively, and suppress or delay nonessential startup features where safe.

Expected payoff:
High for Discord specifically.

Risk/effort:
Medium. Site-specific policies are maintenance work.

Survives constraints:
Yes. Do not frame it as generic browser optimization. It is a site-specific policy for one pathological SPA.

D. Native mirror client is a different product.

Mechanism:
Build a native C++ gateway client that renders a simplified Discord UI instead of running Discord’s actual web app.

Expected payoff:
Huge performance improvement in theory.

Risk/effort:
Extreme and product-changing. It would no longer be Discord running in WebKit; it would be a Discord-compatible client.

Survives constraints:
Technically yes, but it violates the spirit of the project.

E. Remote-browser fallback is the honest escape hatch.

Mechanism:
For heavy JIT-dependent SPAs, run a real remote browser with JIT and stream the result.

Expected payoff:
High for Discord and similar apps.

Risk/effort:
Medium to high depending on your remote stack. It changes the architecture and cost model.

Survives constraints:
It sidesteps them rather than surviving them.

F. Check if your WebKit environment makes Discord choose a worse path.

Mechanism:
Discord may select code paths based on browser features, UA, storage availability, worker availability, wasm support, compression support, IndexedDB behavior, notification APIs, or visibility/focus state. If your environment lacks or misreports one of these, Discord may disable a lazy-loading or dedupe path.

Expected payoff:
Potentially high if a single missing API causes full cold startup state or disables client_state.

Risk/effort:
Medium. Requires comparing Identify payloads and startup event sequences against direct Brave or Chrome.

Survives constraints:
Yes. This is a compatibility-correction path.

G. Validate persistent profile health.

Mechanism:
The brief says an earlier OOM was a corrupt-persisted-profile red herring. For this issue, persistence still matters because Discord client_state, IndexedDB caches, local storage, and session resume can radically affect cold-start gateway payloads.

Expected payoff:
High if your current login path is cold every time.

Risk/effort:
Medium.

Survives constraints:
Yes.

7. Recommended execution plan

8. Capture baseline gateway Identify and decoded READY, READY_SUPPLEMENTAL, and GUILD_CREATE sizes.

9. Run the large_threshold 25 experiment. If 10.5s drops materially, keep going on gateway knobs.

10. Verify capabilities, especially DEDUPE_USER_OBJECTS, PRIORITIZED_READY_PAYLOAD, CLIENT_STATE_V2, PASSIVE_GUILD_UPDATE, and NO_AFFINE_USER_IDS.

11. Persist and replay client_state, and prefer Resume over cold Identify wherever possible.

12. Throttle gateway event delivery so READY-adjacent events cannot all execute before one paint.

13. Add JSC sampling counters to prove whether decompression, GC, builtins, or pure Discord bytecode dominates.

14. Only implement native zlib, TextDecoder, or builtin fast paths if the profiler shows they are hot.

15. Ship host-side freeze UX regardless.

16. Honest final bottom line

If the account’s READY cost is truly proportional to hydrated guild, member, presence, channel, and store size, payload work can plausibly take 10.5s down to a few seconds or split it into tolerable chunks.

If Discord still hands JSC the same object graph, CLoop tuning will not get you from 10.5s to 300ms. That gap is the missing JIT.

So yes: this is fundamentally the no-JIT wall for object-heavy SPA startup. The viable escapes are reducing the amount of Discord state delivered to the interpreter, splitting or pacing the state before handlers run, preserving session/client cache so cold READY is avoided, and framing unavoidable stalls from the host UI. Native accelerators and interpreter tweaks are worth measuring, but they are secondary unless instrumentation proves a specific primitive like JS zlib inflate or GC is unexpectedly dominant.
