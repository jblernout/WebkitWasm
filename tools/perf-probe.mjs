// Perf probe — measures the engine's async-hop latency and network wall
// time from INSIDE the guest (via __bib.eval), so before/after numbers for
// the RunLoop pump work are apples-to-apples.
//
// Probes (results arrive as guest console lines, marker "BIBPERF:"):
//   timer-hops : 60 chained setTimeout(0) — frame-quantized RunLoop shows
//                ~16.7ms/hop (~1000ms total); event-driven pump should
//                collapse this to single-digit ms/hop.
//   mc-hops    : 60 chained MessageChannel posts (the React-scheduler path).
//   raf-rate   : rAF callbacks observed in 1000ms.
//   fetch      : same-origin fetch('/') wall time + body bytes (curl pump
//                cadence; includes connection reuse, not a fresh handshake).
//
// Serve first:
//   node tools/dev-server.mjs web --mount /engine=build/webcore/bin
//   npm run wisp
// Run:  node tools/perf-probe.mjs [target-url]
import { chromium } from "playwright";

const target = process.argv[2] ?? "https://example.com/";
const url =
  "http://127.0.0.1:8080/browser.html?demo=hello&url=" +
  encodeURIComponent(target);

const lines = [];
const browser = await chromium.launch();
const page = await browser.newPage();
page.on("console", (m) => {
  const t = m.text();
  if (t.includes("BIBPERF:")) {
    lines.push(t.slice(t.indexOf("BIBPERF:")));
    console.log(" ", t.slice(t.indexOf("BIBPERF:")));
  }
});
page.on("pageerror", (e) => console.log("[pageerror]", e.message));

await page.goto(url);
await page.waitForFunction(() => window.__bib && window.__bib.ready, {
  timeout: 180000,
});
// Boot page replaced (hello blue div gone) = target navigation committed.
await page.waitForFunction(
  () => {
    const p = window.__bib.probe(50, 126);
    return p && !(p[0] === 0x00 && p[1] === 0x66 && p[2] === 0xcc);
  },
  { timeout: 90000 }
);
// Let the page settle so probe timings aren't polluted by load work.
await page.waitForTimeout(3000);

const HOPS = 60;
const probes = [
  `(function(){var n=0,t0=performance.now();function hop(){if(++n<${HOPS})setTimeout(hop,0);else{var dt=performance.now()-t0;console.log('BIBPERF: timer-hops ${HOPS} in '+dt.toFixed(1)+'ms ('+(dt/${HOPS}).toFixed(2)+'ms/hop)');}}setTimeout(hop,0);})()`,
  `(function(){try{var ch=new MessageChannel(),n=0,t0=performance.now();ch.port1.onmessage=function(){if(++n<${HOPS})ch.port2.postMessage(0);else{var dt=performance.now()-t0;console.log('BIBPERF: mc-hops ${HOPS} in '+dt.toFixed(1)+'ms ('+(dt/${HOPS}).toFixed(2)+'ms/hop)');}};ch.port2.postMessage(0);}catch(e){console.log('BIBPERF: mc-hops ERROR '+e.message);}})()`,
  `(function(){var n=0,t0=performance.now();function tick(){if(performance.now()-t0<1000){n++;requestAnimationFrame(tick);}else console.log('BIBPERF: raf-rate '+n+' frames in 1000ms');}requestAnimationFrame(tick);})()`,
  `(function(){var t0=performance.now();fetch('/').then(function(r){return r.text();}).then(function(b){console.log('BIBPERF: fetch '+b.length+' bytes in '+(performance.now()-t0).toFixed(1)+'ms');}).catch(function(e){console.log('BIBPERF: fetch ERROR '+e.message);});})()`,
];
for (const src of probes)
  await page.evaluate((code) => window.__bib.eval(code), src);

// All four probes report, or 30s cap (poll Node-side line collection).
const deadline = Date.now() + 30000;
while (
  Date.now() < deadline &&
  !["timer-hops", "mc-hops", "raf-rate", "fetch"].every((k) =>
    lines.some((l) => l.includes(k))
  )
)
  await page.waitForTimeout(500);

console.log(
  `PERF-PROBE: ${lines.length >= 4 ? "COMPLETE" : "INCOMPLETE"} (${lines.length}/4) target=${target}`
);
if (lines.length < 4) process.exitCode = 1;
await browser.close();
