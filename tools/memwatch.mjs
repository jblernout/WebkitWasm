// Memory-leak watcher: drives the engine through three phases and samples
// memory every few seconds. Run after a "Firefox almost crashed" report to
// localize WHERE the growth is:
//   wasmBytes  — WebAssembly.Memory size (can only grow; the 4 GB ballooning
//                path; growth while idle = engine-side leak)
//   jsHeap     — host-page JS heap (Chromium only; ImageData/GC churn,
//                wisp client buffers live here)
//   logLen     — host #log textContent length (unbounded += suspect)
//   ticks/frames deltas — rAF tick rate and DIRTY-frame (blit) rate; a
//                nonzero dirty rate while idle means putImageData+ImageData
//                runs at rAF cadence forever (allocation churn)
//
// Phases:
//   idle   — page loaded, hands off, N seconds
//   storm  — synthetic mousemove sweeps + wheel over the canvas, N seconds
//   renav  — repeated bib_load_url navigations (cache/leak accumulation)
//
// Serve first (same as smoke):
//   node tools/dev-server.mjs web --mount /engine=build/webcore/bin
//   npm run wisp
// then:
//   node tools/memwatch.mjs [https-url] [--browser firefox] [--idle 60]
//     [--storm 60] [--renavs 6]
import { chromium, firefox } from "playwright";
import { execFileSync } from "node:child_process";

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf("--" + name);
  return i >= 0 ? args[i + 1] : dflt;
};
const target = args.find((a) => a.startsWith("http")) ?? "https://news.ycombinator.com/";
const browserName = opt("browser", "chromium");
const idleSecs = Number(opt("idle", 60));
const stormSecs = Number(opt("storm", 60));
const renavs = Number(opt("renavs", 6));
const SAMPLE_MS = 5000;

const url =
  "http://127.0.0.1:8080/browser.html?demo=hello&url=" +
  encodeURIComponent(target);

const engine = browserName === "firefox" ? firefox : chromium;
const browser = await engine.launch({
  args:
    browserName === "chromium"
      ? ["--enable-precise-memory-info"]
      : undefined,
});
const page = await (await browser.newContext()).newPage();
page.on("pageerror", (e) => console.log("[pageerror]", e.message));

// Host-browser process-tree RSS (linux): catches growth invisible to the
// page (canvas surfaces, websocket buffers, console retention). The browser
// is a descendant of this node process; sum every descendant's RSS.
const rootPid = process.pid;
function treeRSS() {
  try {
    const rows = execFileSync("ps", ["-e", "-o", "pid=,ppid=,rss="], {
      encoding: "utf8",
    })
      .trim()
      .split("\n")
      .map((l) => l.trim().split(/\s+/).map(Number));
    const kids = new Map();
    for (const [pid, ppid] of rows) {
      if (!kids.has(ppid)) kids.set(ppid, []);
      kids.get(ppid).push(pid);
    }
    const rssOf = new Map(rows.map(([pid, , rss]) => [pid, rss]));
    let total = 0;
    const stack = [...(kids.get(rootPid) ?? [])];
    while (stack.length) {
      const pid = stack.pop();
      total += rssOf.get(pid) ?? 0;
      stack.push(...(kids.get(pid) ?? []));
    }
    return total * 1024;
  } catch {
    return -1;
  }
}

const samples = [];
let prev = null;
async function sample(phase) {
  const s = await page.evaluate(() => ({
    wasmBytes: window.Module?.HEAPU8?.buffer?.byteLength ?? -1,
    jsHeap: performance.memory?.usedJSHeapSize ?? -1,
    logLen: document.getElementById("log").textContent.length,
    ticks: window.__bib?.ticks ?? -1,
    frames: window.__bib?.frames ?? -1,
  }));
  s.phase = phase;
  s.t = Date.now();
  s.rss = treeRSS();
  const dt = prev ? (s.t - prev.t) / 1000 : 0;
  const mb = (b) => (b < 0 ? "n/a" : (b / 1048576).toFixed(1) + "M");
  console.log(
    `[${phase.padEnd(6)}] wasm=${mb(s.wasmBytes)} js=${mb(s.jsHeap)} ` +
      `rss=${mb(s.rss)} log=${s.logLen} ` +
      `ticks/s=${prev && dt ? ((s.ticks - prev.ticks) / dt).toFixed(0) : "-"} ` +
      `blits/s=${prev && dt ? ((s.frames - prev.frames) / dt).toFixed(1) : "-"}`
  );
  samples.push(s);
  prev = s;
  return s;
}

console.log(`memwatch: ${browserName} -> ${target}`);
await page.goto(url);
await page.waitForFunction(() => window.__bib && window.__bib.ready, {
  timeout: 180000,
});
// Wait for the boot page to be replaced by the fetched site.
try {
  await page.waitForFunction(
    () => {
      const p = window.__bib.probeSync(50, 126);
      return p && !(p[0] === 0x00 && p[1] === 0x66 && p[2] === 0xcc);
    },
    { polling: 250, timeout: 90000 }
  );
} catch {
  console.log("note: boot page never replaced");
}
await page.waitForTimeout(10000); // subresources settle
await sample("loaded");

// --- Phase 1: idle ---------------------------------------------------------
for (let t = 0; t < idleSecs; t += SAMPLE_MS / 1000) {
  await page.waitForTimeout(SAMPLE_MS);
  await sample("idle");
}

// --- Phase 2: input storm --------------------------------------------------
const box = await page.locator("#screen").boundingBox();
const stormEnd = Date.now() + stormSecs * 1000;
let lastSample = Date.now();
while (Date.now() < stormEnd) {
  // Sweep the pointer across the page (hover restyles -> dirty frames).
  for (let i = 0; i < 20 && Date.now() < stormEnd; i++) {
    await page.mouse.move(
      box.x + 10 + ((i * 37) % (box.width - 20)),
      box.y + 10 + ((i * 53) % (box.height - 20)),
      { steps: 4 }
    );
  }
  await page.mouse.wheel(0, 120);
  await page.mouse.wheel(0, -120);
  if (Date.now() - lastSample >= SAMPLE_MS) {
    await sample("storm");
    lastSample = Date.now();
  }
}
await sample("storm");

// --- Phase 3: repeated navigations ------------------------------------------
const navTargets = [target, "https://example.com/"];
for (let i = 0; i < renavs; i++) {
  const t = navTargets[i % navTargets.length];
  await page.evaluate(
    (u) => window.Module.ccall("bib_load_url", null, ["string"], [u]),
    t
  );
  await page.waitForTimeout(9000);
  await sample(`nav-${i + 1}`);
}

// --- Verdicts ----------------------------------------------------------------
const phaseOf = (p) => samples.filter((s) => s.phase === p || s.phase.startsWith(p));
const growth = (list, key) =>
  list.length >= 2 ? list[list.length - 1][key] - list[0][key] : 0;
const perMin = (bytes, list) =>
  list.length >= 2
    ? (bytes / ((list[list.length - 1].t - list[0].t) / 60000) / 1048576).toFixed(2)
    : "0";

console.log("\n--- memwatch verdicts ---");
const idle = phaseOf("idle");
const storm = phaseOf("storm");
const nav = phaseOf("nav");
const idleWasm = growth(idle, "wasmBytes");
const stormWasm = growth(storm, "wasmBytes");
const navWasm = growth(nav, "wasmBytes");
console.log(
  `idle : wasm ${perMin(idleWasm, idle)} MB/min, js ${perMin(growth(idle, "jsHeap"), idle)} MB/min, rss ${perMin(growth(idle, "rss"), idle)} MB/min, log +${growth(idle, "logLen")} chars`
);
console.log(
  `storm: wasm ${perMin(stormWasm, storm)} MB/min, js ${perMin(growth(storm, "jsHeap"), storm)} MB/min, rss ${perMin(growth(storm, "rss"), storm)} MB/min, log +${growth(storm, "logLen")} chars`
);
console.log(
  `renav: wasm +${(navWasm / 1048576).toFixed(1)} MB over ${renavs} navs (${(navWasm / 1048576 / Math.max(renavs, 1)).toFixed(1)} MB/nav), rss +${(growth(nav, "rss") / 1048576).toFixed(1)} MB, log +${growth(nav, "logLen")} chars`
);
const idleBlits = idle.length >= 2 ? (idle[idle.length - 1].frames - idle[0].frames) : 0;
console.log(`idle dirty-frame blits: ${idleBlits} over ${idleSecs}s (should be ~0)`);

// COOP/COEP engine page: browser.close() can hang forever (2026-06-11,
// zombie probe trees) -- race it against a timeout, then hard-exit.
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 5000))]);
process.exit(process.exitCode ?? 0);
