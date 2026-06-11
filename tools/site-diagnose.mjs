// Site diagnosis: load one or more real sites in the engine and capture
// EVERYTHING that goes wrong — wasm aborts (with reason), engine stderr
// (printErr; includes the permanent "BIB: load failed curl=N" lines),
// uncaught page errors, wisp stream counts, and paint coverage. Use to
// classify why a site "doesn't work" before reaching for a fix.
//
//   node tools/site-diagnose.mjs https://site1/ [https://site2/ ...]
//   (default list exercises images / cookies / JS-heavy / WS-heavy paths)
import { chromium } from "playwright";

const sites = process.argv.slice(2).length
  ? process.argv.slice(2)
  : [
      "https://en.wikipedia.org/wiki/Main_Page",
      "https://news.ycombinator.com/",
      "https://lite.duckduckgo.com/lite/",
      "https://old.reddit.com/",
    ];

// BIB_CHANNEL=chromium routes around the headless-shell V8 SEGV that heavy
// CLoop workloads trigger (~1/3 of loads on multi-MB JS bundles, task #38).
const browser = await chromium.launch(
  process.env.BIB_CHANNEL ? { channel: process.env.BIB_CHANNEL } : {}
);

for (const target of sites) {
  console.log(`\n=== ${target} ===`);
  const page = await browser.newPage();
  let streams = 0;
  const errLines = [];
  let aborted = null;
  page.on("console", (m) => {
    const t = m.text();
    if (t.startsWith("[bib] wisp: stream")) streams++;
    else if (t.startsWith("[bib] err:")) errLines.push(t.slice(6));
    else if (t.startsWith("[bib] abort:")) aborted = t.slice(6);
  });
  page.on("pageerror", (e) => {
    errLines.push("pageerror: " + e.message);
    // With --profiling-funcs the wasm frames carry function names — the
    // abort stack is the whole diagnosis.
    if (e.stack) for (const l of e.stack.split("\n").slice(0, 18)) errLines.push("  " + l.trim());
  });

  const url =
    "http://127.0.0.1:8080/browser.html?demo=hello&url=" +
    encodeURIComponent(target);
  await page.goto(url);
  try {
    await page.waitForFunction(() => window.__bib && window.__bib.ready, {
      timeout: 180000,
    });
  } catch {
    console.log("VERDICT: engine never became ready (boot abort?)");
    for (const l of errLines.slice(-20)) console.log("  " + l);
    await page.close();
    continue;
  }
  // Give the site a generous load+settle window, but bail early on abort.
  for (let t = 0; t < 25 && !aborted; t++) await page.waitForTimeout(1000);

  const stats = await page
    .evaluate(() => {
      // Render ONCE and sample the framebuffer directly — __bib.probe()
      // forces a full engine repaint per call, which is minutes of work
      // for a grid on a heavy page.
      const ptr = window.Module._bib_render(1);
      if (!ptr) return null;
      const w = window.Module._bib_frame_width();
      const heap = window.Module.HEAPU8;
      let nonWhite = 0,
        samples = 0;
      const colors = new Set();
      for (let y = 2; y < 600; y += 6)
        for (let x = 2; x < 800; x += 10) {
          const i = ptr + (y * w + x) * 4;
          const r = heap[i], g = heap[i + 1], b = heap[i + 2];
          samples++;
          if (!(r === 255 && g === 255 && b === 255)) nonWhite++;
          colors.add((r << 16) | (g << 8) | b);
        }
      return { nonWhite, samples, colors: colors.size, ticks: window.__bib.ticks };
    })
    .catch(() => null);

  const slug = target.replace(/[^a-z0-9]+/gi, "-").replace(/^-|-$/g, "").slice(0, 60);
  await page
    .locator("#screen")
    .screenshot({ path: `build/diagnose-${slug}.png` })
    .catch(() => {});

  if (aborted) console.log(`ABORTED: ${aborted}`);
  console.log(
    stats
      ? `paint: nonWhite=${stats.nonWhite}/${stats.samples} colors=${stats.colors} wispStreams=${streams}`
      : `paint: PROBE FAILED (engine dead) wispStreams=${streams}`
  );
  const loadFails = errLines.filter((l) => l.includes("BIB: load failed"));
  const guestConsole = errLines.filter((l) => l.includes("BIB: console"));
  const other = errLines.filter(
    (l) => !l.includes("BIB: load failed") && !l.includes("BIB: console")
  );
  if (loadFails.length) {
    console.log(`load failures (${loadFails.length}):`);
    for (const l of loadFails.slice(0, 10)) console.log("  " + l);
  }
  if (guestConsole.length) {
    // The guest page's own console.log/warn/error + uncaught exceptions —
    // errors first (they explain blank pages), then the rest.
    const errs = guestConsole.filter((l) => l.includes("BIB: console error"));
    console.log(`guest console (${guestConsole.length}, ${errs.length} errors):`);
    for (const l of errs.slice(0, 12)) console.log("  " + l);
    // Print head AND tail of the non-error lines — a head-only cap once
    // disguised a passing probe as a hang (the verdict lines were last).
    const rest = guestConsole.filter((l) => !errs.includes(l));
    const head = rest.slice(0, 12);
    const tail = rest.length > 24 ? rest.slice(-12) : rest.slice(head.length);
    for (const l of head) console.log("  " + l);
    if (rest.length > head.length + tail.length)
      console.log(`  … ${rest.length - head.length - tail.length} more …`);
    for (const l of tail) console.log("  " + l);
  }
  if (other.length) {
    console.log(`other stderr/page errors (${other.length}):`);
    for (const l of other.slice(0, 25)) console.log("  " + l);
  }
  console.log(`screenshot: build/diagnose-${slug}.png`);
  await page.close();
}

// COOP/COEP engine page: browser.close() can hang forever (2026-06-11,
// zombie probe trees) -- race it against a timeout, then hard-exit.
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 5000))]);
process.exit(process.exitCode ?? 0);
