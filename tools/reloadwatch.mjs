// Reload-spike watcher: reloads browser.html N times in a real browser and
// samples the browser process-tree RSS at high frequency, catching the
// TRANSIENT memory spike of re-downloading + re-compiling the ~88 MB wasm
// module on every reload (dev-server sends Cache-Control: no-store, which
// also defeats the browsers' wasm machine-code caches).
//
// This reproduces the real-world "testing" pattern — reload, poke, reload —
// that steady-state memwatch phases don't cover.
//
//   node tools/reloadwatch.mjs [--browser firefox] [--reloads 5]
import { chromium, firefox } from "playwright";
import { execFileSync } from "node:child_process";

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf("--" + name);
  return i >= 0 ? args[i + 1] : dflt;
};
const browserName = opt("browser", "firefox");
const reloads = Number(opt("reloads", 5));
const settleSecs = Number(opt("settle", 3));
const url = "http://127.0.0.1:8080/browser.html?demo=hello";

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
    const stack = [...(kids.get(process.pid) ?? [])];
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

const engine = browserName === "firefox" ? firefox : chromium;
const browser = await engine.launch();
const page = await (await browser.newContext()).newPage();
const mb = (b) => (b / 1048576).toFixed(0) + "M";

console.log(
  `reloadwatch: ${browserName}, ${reloads} reloads of ${url}, settle=${settleSecs}s`
);
let globalPeak = 0;
for (let i = 0; i <= reloads; i++) {
  let peak = 0;
  let sampling = true;
  const sampler = (async () => {
    while (sampling) {
      peak = Math.max(peak, treeRSS());
      await new Promise((r) => setTimeout(r, 500));
    }
  })();
  await page.goto(url, { waitUntil: "domcontentloaded" });
  try {
    await page.waitForFunction(() => window.__bib && window.__bib.ready, {
      timeout: 180000,
    });
  } catch {
    console.log(`reload ${i}: engine never became ready`);
  }
  // Did this cycle hit the HTTP cache? transferSize ~hundreds of bytes
  // means a 304/cache hit; ~89 MB means a full re-download (and the
  // browsers' wasm machine-code caches key off the cache entry, so a full
  // download usually also means a full recompile).
  const wasmFetch = await page.evaluate(() => {
    const e = performance
      .getEntriesByType("resource")
      .find((r) => r.name.endsWith("embedder.wasm"));
    return e ? { transfer: e.transferSize, decoded: e.decodedBodySize } : null;
  });
  // Long settle windows expose lazy reclamation: sample once a second and
  // keep the minimum, so "held for a while then freed" is distinguishable
  // from "retained forever".
  let settleMin = Infinity;
  const settleEnd = Date.now() + settleSecs * 1000;
  while (Date.now() < settleEnd) {
    await page.waitForTimeout(1000);
    settleMin = Math.min(settleMin, treeRSS());
  }
  sampling = false;
  await sampler;
  const settled = treeRSS();
  globalPeak = Math.max(globalPeak, peak);
  console.log(
    `${i === 0 ? "boot  " : "reload"} ${String(i).padStart(2)}: ` +
      `peak=${mb(peak)} settleMin=${mb(settleMin)} settled=${mb(settled)} ` +
      `wasmFetch=${wasmFetch ? `${mb(wasmFetch.transfer)} transferred / ${mb(wasmFetch.decoded)} decoded` : "n/a"}`
  );
}
console.log(`\nglobal peak RSS: ${mb(globalPeak)}`);
await browser.close();
