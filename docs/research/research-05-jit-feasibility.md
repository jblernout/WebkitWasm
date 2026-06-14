# Research 05 — GPT-5.5 Pro deep JIT-feasibility analysis (2026-06-13)

Verbatim consult #2 (10 core files attached: CLAUDE.md, decision-001/006, 00-brief, build-webcore.sh, embedder.cmake, main.cpp, wasm-polyfill.js, engine-pre.js, research-04). Builds on research-04. Synthesis in task #77/#80 + memory discord-freeze-rootcause-nojit + handoff-2026-06-13-discord-nojit-wall.md.

---

1. Executive verdict

This is a real no-JIT wall. With your new measurement, the prior answer’s “maybe payload first” framing should be downgraded: if 0.8 MB compressed becomes about 95 MB of live JS objects and the freeze is 10.8 seconds inside one synchronous WTF::RunLoop::cycle(), the dominant term is not transport, decompression, JSON.parse, canvas, layout, or memory pressure. It is interpreted JavaScript performing huge numbers of semantically visible object operations.

There is no practically achievable “drop in a JIT-to-wasm” path for this architecture that is likely to deliver JIT-like Discord performance in a short or medium spike. A real JSC-to-wasm JIT is possible in the broad computer-science sense, but it is essentially a new JSC execution tier: a wasm backend, runtime ABI, IC system, deopt/OSR machinery, exception protocol, GC barrier integration, and invalidation model. That is a multi-month to year-scale compiler/runtime project, and it still may not amortize on Discord’s object-churn workload.

The only realistic >2x in-tab possibilities are narrower and conditional:

* A CLoop inline-cache/property fast-path spike, if your pinned CLoop path is missing a very basic monomorphic get_by_id/put_by_id fast path.
* A GC/allocator tuning spike, if measured GC or allocator overhead is much higher than assumed.
* Site/protocol shaping that reduces hydrated object count, not just compressed bytes.
* UX/remote-browser fallback for heavyweight SPAs.

My confidence: high, about 0.8, that “JIT-like in-tab speedup for arbitrary Discord app code” is not practically achievable under the stated constraints. Medium, about 0.45, that a targeted CLoop/object fast-path could produce a measurable 1.2x to 1.8x. Low, about 0.15, that an in-tree non-JIT change gets you a robust >2x on the real Discord READY hydration path.

2. Grounding from your actual setup

Your build is exactly the hostile case for guest-JS performance: WebKit/WebCore plus JSC compiled to wasm32, JSC running CLoop, JIT disabled, single-process WebKit1-style embedder, networking through curl/OpenSSL/Wisp, and Emscripten pthreads/PROXY_TO_PTHREAD for keeping the host page alive. The project notes explicitly identify JSC CLoop as the supported no-JIT mode and state the wasm32 4 GB ceiling and COOP/COEP pthread requirement. 

The actual build script passes PORT=Emscripten, ENABLE_JIT=OFF, ENABLE_C_LOOP=ON, ENABLE_STATIC_JSC=ON, with wasm SIMD and pthread flags depending on BIB_PTHREAD.  The project brief also records that JSC-on-wasm runs in-tab and that the WebCore/Skia/curl embedder is already real, not a toy shell. 

The runtime architecture matters: main(), WebCore, and JSC live on the engine pthread under PROXY_TO_PTHREAD; host calls proxy into that thread; bib_tick and bib_pump ultimately run WTF::RunLoop::cycle() on the engine thread. The instrumentation already attributes runloop(JS), rendering, layout, paint, pump, net, heap, and JSC heap separately.  So a 10.8 second synchronous RunLoop::cycle() is not a scheduling bug. It is the engine thread executing guest JS and cannot be painted around from inside WebCore/JSC.

The IPInt path is already killed in the attached decision record: IPInt does not have a C_LOOP lowering, ENABLE_WEBASSEMBLY would hit IPInt::initialize() and release-assert, and the old wasm LLInt is gone; BBQ/OMG are real JITs and structurally unavailable inside wasm32. 

3. JIT-to-wasm: full JSC retarget

A full retarget means making JSC’s Baseline JIT, or a new equivalent tier, emit WebAssembly modules at runtime and then asking the host browser to compile/instantiate them. WebAssembly.compile does compile bytes into a WebAssembly.Module, and instantiate/instantiateStreaming can compile and instantiate modules through the browser’s wasm engine. ([MDN Web Docs][1]) ([MDN Web Docs][2]) That is the one opening that avoids native W+X memory: the browser owns executable code internally.

But in JSC terms, this is not “emit wasm instead of machine code.” It requires all of this:

* A wasm code backend for JSC bytecode or for a JSC IR. You cannot reuse most of the native MacroAssembler-based JIT directly because its model is native registers, native calls, patchable code, near/far branches, inline stubs, and executable memory. WebAssembly is structured, validated, module-based code with no arbitrary patching.

* A runtime ABI between generated wasm modules and the Emscripten-compiled WebKit/JSC instance. The generated module would need to import the same WebAssembly.Memory, likely shared memory under pthreads, and import runtime helper functions for allocation, property lookup, calls, exceptions, barriers, slow paths, and DOM/JSC runtime functions.

* A correct JSValue representation. On wasm32, you likely have JSC’s 32-bit JSValue configuration rather than a native 64-bit NaN-boxed register representation. Generated wasm must speak exactly that representation: tag/payload pairs or the actual EncodedJSValue layout chosen by the port. Every call boundary, stack slot, virtual register, property store, and return value must match JSC’s C++ expectations.

* GC write barriers. Object hydration is mostly allocations and writes. Every store of a JSValue into a JSCell, butterfly, array storage, Map/Set backing storage, or object property must either execute JSC’s write barrier or prove it is unnecessary. If a generated wasm “fast path” simply writes memory, it will corrupt the collector.

* Inline caches. To beat CLoop on Discord, generated wasm must inline get_by_id, put_by_id, get_by_val, new_object, array push, call, construct, and possibly Map/Set operations. But ICs need structure guards, watchpoints, invalidation, polymorphic cases, slow-path fallback, and shape transition handling. In a native JIT, some of this is patched into code. In wasm, you cannot patch compiled code bytes; you would need mutable globals/tables/side metadata or module recompilation.

* Calls and polymorphism. Discord’s hot path is not a numeric loop. It is short calls, closures, store dispatches, object creation, property writes, callbacks, and framework code. If every call/construct or property miss bounces out to a C++ helper, the wasm tier becomes a faster dispatcher around the same slow path, not a JIT.

* OSR entry and exit. If CLoop decides a function is hot, it must enter wasm with a materialized JSC call frame. If a guard fails, if an exception is thrown, if a debug/profiling hook fires, if eval/arguments/with/proxy semantics invalidate assumptions, or if the GC needs precise roots, you must reconstruct state for JSC. That means stack maps, value recovery, and safe-point discipline.

* Exception semantics. JSC exceptions are VM state plus control-flow discipline, not just host JS exceptions. Generated wasm must check exception state after every imported runtime call and unwind or return through a protocol that JSC understands. WebAssembly exception handling does not automatically give you JSC exception semantics.

* DOM/runtime calls. Anything touching WebCore, host functions, getters/setters, proxies, toString/valueOf, promise jobs, iterators, accessors, or arbitrary user callbacks exits the optimized subset. Discord object hydration is full of semantically visible JS operations, so the optimized region is fragmented.

This is why a full Baseline-to-wasm backend is not a clever reuse of your Binaryen bridge. It is a new execution tier.

4. JIT-to-wasm: narrow bespoke hot-function tier

A narrower tier would skip DFG/Baseline retargeting and compile hot JSC bytecode functions to wasm beside CLoop. This sounds cheaper, but the first useful version still becomes a mini baseline JIT.

A naive version would map JSC bytecodes to wasm and call helpers for nearly everything:

* get_by_id becomes helper_get_by_id(vm, callFrame, ident)
* put_by_id becomes helper_put_by_id(...)
* call becomes helper_call(...)
* construct becomes helper_construct(...)
* new_object becomes helper_new_object(...)
* array operations become helper calls

That is easy-ish to prototype, but it will not speed Discord by 2x. It removes some interpreter dispatch overhead, but every meaningful operation still bounces through runtime helpers. For object hydration, the interpreter dispatch is only one component; the expensive part is dynamic semantics, allocation, property layout, call polymorphism, barriers, and structure transitions.

A useful version must inline at least:

* monomorphic get_by_id
* monomorphic put_by_id with direct storage offset
* object literal allocation with known Structure
* array push / dense array growth fast path
* int32/boolean/string constant moves
* direct calls to known JSFunctions
* fast return and branch bytecodes
* write barrier fast path
* exception checks after slow calls
* IC update/invalidation

At that point it is no longer “bespoke.” It is a baseline JIT with a wasm backend.

The per-call cost is the killer. You should measure it in your exact host, but the useful orders of magnitude are:

* WebAssembly module compile plus instantiate: usually sub-millisecond to several milliseconds for very small modules, and much more for larger modules. One module per hot function is probably dead unless functions are very hot and reused.
* JS-to-wasm or wasm-to-JS boundary: often tens to hundreds of nanoseconds for trivial optimized calls in good browser engines, but with Emscripten wrappers, shared-memory views, exception checks, and C++ runtime helper plumbing, assume hundreds of nanoseconds to low microseconds.
* Wasm-to-engine-runtime helper call: if it goes through JS imports or Emscripten-exported wrappers, assume roughly 0.5 to 5 microseconds until measured otherwise.

That last number destroys helper-heavy designs. One million helper calls at 1 microsecond is already 1 second. Ten million helper calls is 10 seconds. Discord hydration plausibly contains millions of property writes/calls/allocations. So a wasm tier that bounces per operation cannot deliver JIT-like speed. It must inline the hot operations.

5. Can Discord’s workload amortize JIT-to-wasm?

For numeric loops, yes. For Discord READY hydration, mostly no.

The ideal wasm-JIT workload is one hot function with simple loops and stable typed values. A generated wasm module can run for many thousands or millions of iterations before re-entering the engine. That amortizes compile/instantiate and boundary cost.

Discord’s READY hydration is the opposite:

* many short functions
* many object allocations
* many property writes
* many polymorphic calls
* store/action dispatch
* framework callbacks
* structure transitions
* JS arrays/maps/sets
* closures and module state
* observable semantics everywhere

If a generated wasm function cannot own the whole hot region, it re-enters runtime constantly. If it can own the whole hot region, you have implemented a serious JS JIT.

The new 95 MB live-object number actually strengthens the wall verdict. The problem is not compressed bytes; it is materializing a large JS object graph through JSC semantics. A wasm module can only help if it writes that same JSC heap graph faster and correctly. That requires inlining JSC object mechanics, not just compiling bytecode shape.

6. Can the existing Binaryen bridge be inverted?

Use it for measurement, not as a real compiler.

Your shipped wasm shim already proves a lot of infrastructure: guest WebAssembly API injection, a native __bibWasm2js host function, base64 round-trip, Binaryen in the host/worker, translated source returning to the guest, and eval before author script.  The worker pre-js also loads Binaryen, exposes Module.bibWasm2js, reads module bytes, calls binaryen.readBinary, sets features, emits asm.js, rewrites imports, and returns a factory string. 

But that bridge is the inverse of what you need. It currently translates guest wasm to JS that executes under CLoop. That intentionally solves compatibility, not speed. The decision record explicitly notes that translated modules execute at CLoop-JS speed and expand guest heap usage. 

For JIT-to-wasm, Binaryen can help you construct and optimize wasm modules. The host/worker bridge can help you compile/instantiate them using the browser’s real wasm engine. It does not provide:

* a JS bytecode frontend
* JSC bytecode semantics
* JSValue layout
* GC barriers
* ICs
* OSR exits
* exception protocol
* runtime helper ABI
* call-frame reconstruction

The cheap reuse is a spike:

* generate a tiny wasm module with Binaryen
* import the engine’s shared memory
* import a few raw engine exports or JS wrappers
* measure compile/instantiate time
* measure pure wasm loop speed
* measure wasm-to-runtime helper call overhead
* measure a simple direct memory write loop into a scratch region

That spike can kill the helper-heavy JIT idea quickly. It will not prove the full tier viable.

7. Copy-and-patch JIT

Copy-and-patch is attractive for low-latency JITs, but its core trick is copying precompiled native machine-code stencils and patching holes in executable code. Xu and Kjolstad describe it as stitching together binary implementation variants and show very fast compilation; their paper reports a high-level compiler much faster than LLVM and generated code an order of magnitude faster than interpretation, and a WebAssembly compiler faster than Chrome Liftoff on their benchmarks. ([arXiv][3])

That does not survive your browser-wasm constraint as a native JIT technique:

* You cannot allocate native executable memory.
* You cannot copy native x86/ARM stencils into a code page.
* You cannot patch browser-owned wasm machine code.
* WebAssembly modules are validated and compiled through WebAssembly.compile/instantiate, not mutated after the fact.
* IC patching becomes metadata mutation or module recompilation, not native code patching.

A wasm-compatible variant exists only in a weaker form:

* Prebuild wasm function stencils.
* Patch wasm binary immediates or indexes.
* Compile the resulting wasm module through WebAssembly.compile.
* Use mutable globals/tables/imports as “holes” instead of patching code.

That is no longer copy-and-patch in the performance-critical sense. The browser still validates and compiles the wasm. It may make module generation faster, but module generation is not the hard part. The hard part is making generated code own JSC object operations without bouncing through helpers.

Verdict: copy-and-patch is not a practical escape hatch here. It is useful conceptually if you eventually build a wasm baseline compiler, but it does not remove the need for a JSC wasm backend and runtime integration.

8. Wasm GC

Wasm GC does not make JSC’s object hydration fast.

Wasm GC is designed to let languages targeting WebAssembly use GC-managed struct/array/reference types in the host wasm engine. The proposal and browser work are about supporting GC languages compiled to wasm, not accelerating a separate C++ JavaScript engine’s internal JSCell heap. ([GitHub][4]) ([V8][5])

Your JSC objects live in JSC’s heap and are exposed through JSC/WebCore/DOM bindings as JSCell/JSValue objects. To use Wasm GC for actual guest JS objects, you would need to retarget JSC’s object representation, collector, barriers, stack scanning, DOM wrappers, property storage, object identity, and API surface onto wasm-GC references. That is basically a new JavaScript engine architecture.

A shadow representation also fails: if a wasm-GC object mirrors a JSC object, you still have to materialize the real JSC object graph before Discord/WebCore can use it. That is the 10.8 second wall again.

Verdict: not tactical, not a >2x path for this project. It is research-engine territory.

9. Non-JIT speedups that might still matter

A. CLoop inline-cache/property fast paths

This is the only in-tab spike I would prioritize.

The prior answer said “CLoop tuning probably cannot close the gap.” With the new data, I would refine that: generic CLoop tuning cannot close the gap, but one specific missing fast path could plausibly matter. If your pinned wasm/CLoop configuration is failing to exploit monomorphic Structure-based get_by_id/put_by_id metadata, object hydration will be much worse than it needs to be.

The target is not “optimize the interpreter.” The target is “avoid C++ slow paths for the common direct own-property case.”

The fast path shape:

* bytecode has get_by_id or put_by_id
* IC metadata has a cached StructureID and offset
* base is a JSObject with matching Structure
* property is direct data property, no accessor, no proxy, no dictionary mode
* storage location is inline or butterfly offset
* value is written directly
* write barrier fast path runs
* on any mismatch, fall back to existing slow path

This might hit Discord because minified store hydration tends to allocate many objects of recurring shapes. It may also fail because object literals, spread, Object.assign, arrays, and store update paths produce many transitions and polymorphic shapes.

Realistic payoff:

* If CLoop already has decent IC fast paths: 5% to 20%.
* If CLoop path is accidentally generic/slow for wasm32: 1.5x to 2.5x on a synthetic hydration benchmark, maybe 1.2x to 1.8x on real Discord.
* If Discord is dominated by call overhead and framework dispatch rather than property stores: small.

B. Allocation and GC tuning

The project already measures heap and JSC heap in perf logs, and the risk register says memory headroom exists but wasm32 stays capped.  With 531 MB used out of 4 GB, temporary startup heap generosity is reasonable.

Only pursue if gclog/allocation counters show real pause or allocator cost. Good experiments:

* raise collection thresholds during gateway READY
* pre-reserve JSC heap/marked blocks
* bias toward fewer collections during the known hydration phase
* verify bmalloc/system malloc choice and per-cell allocation path under wasm
* count JSCell allocations by type and allocation slow paths

Expected payoff:

* 0% if GC is not active.
* 5% to 25% if READY triggers repeated collections.
* Possibly higher only if allocator path is pathological under Emscripten/system malloc.

C. Builtin fast paths

Object.keys, Object.values, Object.assign, array spread/copy, Array.prototype methods, Map.set/get, and Set.add are worth counting. The prior report already recommends timing builtin entrypoints and bytecodes and treating native builtins as worth patching only if they dominate. 

The ceiling is limited because array methods frequently call arbitrary JS callbacks. Native code can speed the envelope but not callback bodies.

Expected payoff:

* 5% to 30% if one builtin dominates.
* Low if samples are mostly Discord functions plus get_by_id/put_by_id/call/construct/new_object.

D. Threaded dispatch / tail calls / SIMD / PGO

These are not first-order for object hydration.

* SIMD is mostly irrelevant to pointer-heavy object work.
* Tail calls do not turn an interpreter into a JIT.
* Direct-threaded dispatch loses much of its native advantage when compiled through wasm structured control flow/br_table.
* PGO/case ordering might give single digits to low teens.
* Wasm exception handling/JSPI do not help synchronous object hydration. JSPI is about letting wasm code interact with promise-based async APIs, not accelerating JS object stores. ([WebKit][6])

10. Minimal experiments I would run

Experiment 1: JIT-to-wasm boundary kill test

Goal: prove or kill helper-heavy JIT-to-wasm.

Build a tiny generated wasm module in the engine worker using Binaryen or prebuilt bytes. Import the engine’s memory. Import three functions:

* a raw no-op wasm export from the engine module, if accessible as a WebAssembly function
* a JS wrapper around an engine export
* a real JSC helper-like export that touches a dummy JS object or at least crosses into C++

Measure:

* WebAssembly.compile time
* instantiate time
* JS-to-wasm export call time
* wasm-to-import no-op call time
* wasm-to-engine helper call time
* pure wasm loop memory store bandwidth into imported memory
* same module with 1 function, 10 functions, 100 functions

Expected signal:

* If helper calls are above about 0.5 microseconds and your simulated property-write helper needs one call per JS operation, a helper-heavy wasm tier cannot get >2x on Discord.
* If module compile/instantiate is above a few milliseconds per small function, one-module-per-hot-function is dead. You would need batched modules or long-lived compiled chunks.
* If pure wasm loops are very fast but helper calls dominate, that confirms wasm only helps code that stays inside wasm.

Experiment 2: Synthetic READY hydration benchmark inside JSC

Goal: create a controlled Discord-like benchmark without Discord JS variability.

Run a guest script that creates, normalizes, and stores a generated object graph roughly matching the measured live-object profile:

* many guild-like objects
* nested channel/member/user/role arrays
* repeated shapes
* Map/Set updates
* Object.assign/spread-like copies
* property writes
* short function dispatch wrappers

Run in direct Brave, your JSC CLoop build, and maybe native jsc with JIT off/on if available.

Expected signal:

* If CLoop is 20x to 40x slower than JIT on this synthetic too, the problem is fundamental.
* If a tiny subset of operations dominates, you have a tractable CLoop fast-path target.
* If direct JIT does ~300 ms and CLoop does ~10 s on the synthetic, it validates the Discord measurement without protocol noise.

Experiment 3: CLoop get_by_id/put_by_id sampling and one fast path

Goal: find whether a narrow non-JIT >2x is possible.

Add counters/sampling in CLoop for:

* get_by_id
* put_by_id
* get_by_val
* new_object
* construct
* call
* create_this
* array push/growth
* Map/Set operations
* slow-path entry counts
* StructureID hit/miss counts
* dictionary-mode transitions
* butterfly growth

Then implement only one fast path: monomorphic direct own-property put_by_id with cached structure and offset, including write barrier. Guard aggressively; fallback on everything else.

Expected signal:

* On synthetic hydration, >1.5x means continue to get_by_id and object literal allocation.
* On synthetic hydration, <1.2x means the real win is not property put ICs.
* On Discord, even a 20% drop in the 10.8 s READY window would be meaningful evidence, but still not JIT-like.

Experiment 4: GC/allocator gating

Goal: separate interpreter semantics from heap management.

During READY only, enable:

* JSC_logGC
* JSCell allocation counts by type
* GC pause total
* allocation slow-path count
* heap growth count
* malloc/free sampling if feasible

Then temporarily make startup GC very permissive while under a safe heap threshold.

Expected signal:

* If GC pause is below 5% of 10.8 s, stop.
* If GC plus allocation slow paths are above 20%, pursue allocator/heap tuning.
* If suppressing collections changes 10.8 s by less than 10%, stop.

Experiment 5: module batching and IC metadata model

Goal: test the only plausible shape of a wasm tier.

Generate one wasm module containing many tiny functions with mutable globals representing IC metadata. Compare:

* function-per-module
* 100 functions per module
* mutable-global IC constants
* imported table of runtime helpers
* recompile-on-invalidation versus metadata mutation

Expected signal:

* If compile/instantiate and metadata plumbing are already painful before JS semantics, full JIT-to-wasm is not worth staffing.
* If the mechanics are clean, it only says “infrastructure is possible,” not “Discord will speed up.”

11. Specific answer to “can the object-churn workload ever amortize wasm JIT boundary cost?”

Only if the wasm tier inlines object operations. Otherwise, no.

For Discord hydration, a wasm tier has three regimes:

* Pure helper-call lowering: likely not worth it. Dispatch overhead falls, boundary/runtime helper overhead rises, semantics stay slow. Expected real speedup: 0.8x to 1.3x.
* Partial IC lowering: maybe useful. Inline direct property get/put, array dense paths, direct calls, object literal allocation. Expected real speedup if well done: 1.3x to 2.5x, but this is already a serious baseline tier.
* Full baseline JIT semantics: useful. Expected speedup: potentially multi-x, maybe enough to approach native no-DFG browser baseline on some code. Cost: very high, long-term fork.

The 10.8 s to 300 ms gap is about 36x. You do not get that from reducing interpreter dispatch alone. You get it from optimizing object access, calls, allocations, ICs, and type-specialized fast paths across the whole hot graph.

12. Structural verdict by avenue

* Native JSC JIT: impossible inside your wasm module because you cannot allocate and execute native code.

* IPInt: no-go in your pinned CLoop configuration; the attached decision already proves the boot assert and lack of CLoop lowering. 

* Full JSC JIT-to-wasm backend: theoretically possible, practically not a near-term project. Treat as 6 to 18 months of serious compiler/runtime work, not a spike.

* Bespoke hot-function-to-wasm beside CLoop: useful only if it grows inline ICs and barriers. Helper-only lowering is likely dead. A minimal boundary microbench should kill or constrain it in days.

* Copy-and-patch: no native-code version survives. A wasm-stencil variant is just a template wasm emitter and still needs browser compile/instantiate plus JSC runtime integration.

* Wasm GC: not a tactical accelerator for JSC’s existing JS heap. It would require replacing or shadowing JSC object representation.

* CLoop IC/property fast paths: best local in-engine bet. Could plausibly give a meaningful but not JIT-like win. Worth one focused spike.

* GC/allocator tuning: worth a measurement gate only. Memory headroom makes it safe to try, but it will not fix interpreter semantics.

* Threading/offload: cannot move Discord’s live JS state to another VM/thread. Native pre-work and gateway pacing remain useful only outside one running JS handler. The prior answer’s offload limitation still stands. 

* Payload/protocol shaping: compressed payload size is no longer the right metric. But object-count reduction is still real. A 0.8 MB compressed payload can encode a huge normalized state graph. large_threshold/capability/client_state work is weak only if it fails to reduce hydrated object count.

13. What I would actually prioritize

14. Implement CLoop opcode/object-operation sampling first. You need proof of where the 10.8 s goes at the bytecode/operation level.

15. In parallel, run the JIT-to-wasm boundary kill test. Do not start a wasm tier until you know helper-call and instantiate costs in your exact PROXY_TO_PTHREAD/shared-memory setup.

16. Build the synthetic READY hydration benchmark. This becomes your regression harness for every CLoop/GC/object fast-path experiment.

17. Try exactly one JSC fast path: monomorphic direct put_by_id with write barrier, then get_by_id if put_by_id moves the synthetic benchmark.

18. Gate GC/allocator tuning on measured GC/allocation overhead.

19. Keep protocol shaping only if it reduces object count, not bytes. The metric should be live JSCell count, Structure count, property writes, and RunLoop wall time.

20. Keep host freeze UX regardless. Your host page remains alive while the engine pthread is blocked, and your architecture is already designed around that separation.

21. Final answer

There is no practically achievable JIT or JIT-like path that gets Discord’s object hydration from 10.8 seconds to roughly 300 ms while staying in-tab, wasm32, one live JSC VM, no native executable memory, and no Discord JS modification.

The only “real JIT” route is a JSC Baseline/DFG-to-wasm execution tier. That is possible only as a major compiler/runtime project. It must solve JSValue layout, imported shared memory, JSC heap mutation, write barriers, inline caches, runtime helper ABI, exceptions, OSR/deopt, invalidation, and cross-module calls. Your existing Binaryen bridge is valuable for experiments but does not make this cheap; it proves byte/string transport and host-side translation plumbing, not live JSC-state acceleration.

The best near-term technical bet is not JIT-to-wasm. It is to prove whether CLoop lacks one or two critical object fast paths. If a monomorphic get_by_id/put_by_id/object-allocation fast path can hit Discord’s shapes, you may get a real but bounded speedup. If that spike does not move a synthetic READY benchmark by at least 1.5x, the honest conclusion is definitive: this is the no-JIT wall, and the product strategy should be object-count reduction, startup pacing where possible, host-side loading/freeze UX, and remote-browser fallback for Discord-class SPAs.

[1]: https://developer.mozilla.org/en-US/docs/WebAssembly/Reference/JavaScript_interface/compile_static?utm_source=chatgpt.com "WebAssembly.compile() - MDN Web Docs"
[2]: https://developer.mozilla.org/en-US/docs/WebAssembly/Reference/JavaScript_interface/instantiate_static?utm_source=chatgpt.com "WebAssembly.instantiate() - MDN Web Docs"
[3]: https://arxiv.org/abs/2011.13127?utm_source=chatgpt.com "Copy-and-Patch Compilation: A fast compilation algorithm for high-level languages and bytecode"
[4]: https://github.com/webassembly/gc?utm_source=chatgpt.com "WebAssembly/gc: Branch of the spec repo scoped to ..."
[5]: https://v8.dev/blog/wasm-gc-porting?utm_source=chatgpt.com "A new way to bring garbage collected programming ..."
[6]: https://webkit.org/blog/17818/announcing-interop-2026/?utm_source=chatgpt.com "Announcing Interop 2026"

