// W-B1 acceptance (analysis-wb-engine-off-main-thread.md): discord.com/login
// boots while the HOST TAB stays responsive. Pre-W-B1 the single-threaded
// CLoop boot pegged the browser main thread for tens of seconds (Chrome's
// "wait or close" dialog) — the host rAF/timer loop froze with it.
//
// Measures: a 50ms host setInterval tick log during the whole boot window;
// the metric is the LONGEST tick gap. Pegged main thread => multi-second
// gaps. Acceptance: max gap under 500ms across a 75s discord boot window
// AND the engine demonstrably progresses (wisp streams + console activity).
//
// Run: node tools/wb1-acceptance.mjs [url] (serve :8080 + wisp :5001 first)
import { chromium } from "playwright";

const target = process.argv[2] ?? "https://discord.com/login";
const url = `http://127.0.0.1:8080/browser.html?url=${encodeURIComponent(target)}`;
const BOOT_WINDOW_MS = 75000;

const browser = await chromium.launch({ channel: process.env.BIB_CHANNEL || "chromium" });
const page = await browser.newPage();

let wispStreams = 0;
let engineLines = 0;
let aborted = false;
page.on("console", (m) => {
  const t = m.text();
  if (t.includes("wisp: stream ->")) wispStreams++;
  if (t.includes("out: ") || t.includes("err: ")) engineLines++;
  if (t.includes("abort: ")) { aborted = true; console.log("[abort] " + t.slice(0, 200)); }
});

// Host-side responsiveness witness, installed before any engine code runs.
await page.addInitScript(() => {
  window.__wbTicks = [];
  setInterval(() => window.__wbTicks.push(performance.now()), 50);
});

console.log(`WB1-ACCEPT: loading ${target} (window=${BOOT_WINDOW_MS}ms)`);
await page.goto(url);
const t0 = Date.now();
while (Date.now() - t0 < BOOT_WINDOW_MS && !aborted)
  await page.waitForTimeout(1000);

const stats = await page.evaluate(() => {
  const ticks = window.__wbTicks;
  let maxGap = 0, over100 = 0, over500 = 0;
  for (let i = 1; i < ticks.length; i++) {
    const gap = ticks[i] - ticks[i - 1];
    if (gap > maxGap) maxGap = gap;
    if (gap > 100) over100++;
    if (gap > 500) over500++;
  }
  const bib = window.__bib || {};
  return { ticks: ticks.length, maxGap: Math.round(maxGap), over100, over500,
    engineTicks: bib.ticks, frames: bib.frames, ready: bib.ready, dead: bib.dead };
});

console.log(`WB1-ACCEPT: host ticks=${stats.ticks} maxGap=${stats.maxGap}ms ` +
  `gaps>100ms=${stats.over100} gaps>500ms=${stats.over500}`);
console.log(`WB1-ACCEPT: engine ready=${stats.ready} dead=${stats.dead} ` +
  `ticks=${stats.engineTicks} frames=${stats.frames} wispStreams=${wispStreams} consoleLines=${engineLines}`);

const responsive = stats.maxGap < 500;
const progressing = wispStreams >= 1 && stats.ready && !stats.dead;
console.log(`WB1-ACCEPT-VERDICT: ${responsive && progressing ? "PASS" : "FAIL"} ` +
  `(responsive=${responsive} progressing=${progressing})`);
process.exitCode = responsive && progressing ? 0 : 1;

await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 5000))]);
process.exit(process.exitCode);
