// Gate 2 browser test: WebKit-in-wasm paints its framebuffer onto a <canvas>
// in a real (headless Chromium) tab, and the rAF loop stays alive.
// Loads the hello demo (?demo=hello) — the Phase 2 gate page — and expects
// pixel counts identical to the offscreen node gate.
// Serve first:
//   node tools/dev-server.mjs web --mount /engine=build/webcore/bin
// then:
//   node tools/gate2-browser-test.mjs [url]
import { chromium } from "playwright";

const url = process.argv[2] ?? "http://127.0.0.1:8080/browser.html?demo=hello";
const browser = await chromium.launch();
const page = await browser.newPage();

page.on("console", (m) => {
  const t = m.text();
  if (t.startsWith("[bib] ")) console.log(t);
});
page.on("pageerror", (e) => console.log("[pageerror]", e.message));

await page.goto(url);
console.log("crossOriginIsolated:", await page.textContent("#coi"));

try {
  // First verdict is computed from the engine's own RGBA bytes on frame 0.
  // Generous timeout: the tab fetches + compiles an 87 MB wasm module.
  await page.waitForFunction(
    () => window.__bib && window.__bib.verdict !== "pending",
    { timeout: 180000 }
  );
  const verdict = await page.evaluate(() => window.__bib.verdict);
  if (verdict === "PASS") {
    // Liveness: the rAF loop must keep ticking. (Blit count stays at 1 for
    // a static page by design — clean frames skip putImageData.)
    await page.waitForFunction(() => window.__bib.ticks >= 10, {
      timeout: 30000,
    });
  }
  const bib = await page.evaluate(() => window.__bib);
  console.log(
    `__bib: verdict=${bib.verdict} exactBlue=${bib.exactBlue} redGlyph=${bib.redGlyph} ticks=${bib.ticks} blits=${bib.frames}`
  );
  await page
    .locator("#screen")
    .screenshot({ path: "build/gate2-canvas.png" })
    .catch(() => {});
  if (bib.verdict === "PASS") {
    console.log("GATE2-BROWSER: PASS");
  } else {
    console.log("GATE2-BROWSER: FAIL");
    process.exitCode = 1;
  }
} catch (e) {
  console.log("GATE2-BROWSER: FAIL (timeout)");
  console.log("--- page log ---");
  console.log(await page.textContent("#log").catch(() => "(no log)"));
  process.exitCode = 1;
}
// COOP/COEP engine page: browser.close() can hang forever (2026-06-11,
// zombie probe trees) -- race it against a timeout, then hard-exit.
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 5000))]);
process.exit(process.exitCode ?? 0);
