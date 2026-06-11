// Gate 8 (G3): the engine paints the hello page through Skia-Ganesh/WebGL2
// (?gpu=1) and the G3 readback path (bib_render_readback) returns frames the
// hello judge accepts (tolerance ±2 — readback has been pixel-identical to
// the CPU raster so far). Requires a REAL GPU-capable Chromium: Playwright's
// headless-shell loses WebGL contexts at first composite, so this gate
// launches channel "chromium" (override with BIB_CHANNEL).
// Serve first:
//   node tools/dev-server.mjs web --mount /engine=build/webcore/bin
// then:
//   node tools/gate8-gpu-test.mjs [url]
import { chromium } from "playwright";

const url = process.argv[2] ?? "http://127.0.0.1:8080/browser.html?demo=hello&gpu=1";
const browser = await chromium.launch({ channel: process.env.BIB_CHANNEL || "chromium" });
const page = await browser.newPage();

let gpuOn = false, fellBack = false;
page.on("console", (m) => {
  const t = m.text();
  if (t.startsWith("[bib] ")) console.log(t);
  if (t.includes("EMBEDDER: gpu=on")) gpuOn = true;
  if (t.includes("REQUESTED-BUT-UNAVAILABLE")) fellBack = true;
  // Late fallback: surface/FBO-wrap failure AFTER gpu=on (Codex, G3) —
  // the engine paints CPU raster the GPU-mode host never displays.
  if (t.includes("gpu surface setup failed")) fellBack = true;
  if (t.includes("reloading in raster mode")) fellBack = true;
});
page.on("pageerror", (e) => console.log("[pageerror]", e.message));

await page.goto(url);
console.log("crossOriginIsolated:", await page.textContent("#coi"));

try {
  await page.waitForFunction(
    () => window.__bib && window.__bib.verdict !== "pending",
    { timeout: 180000 }
  );
  const verdict = await page.evaluate(() => window.__bib.verdict);
  if (verdict === "PASS") {
    await page.waitForFunction(() => window.__bib.ticks >= 10, {
      timeout: 30000,
    });
  }
  const bib = await page.evaluate(() => window.__bib);
  // __bib.probe must work under GPU (G3's whole point): blue region pixel,
  // same tolerance as the judge.
  const px = await page.evaluate(() => window.__bib.probe(50, 126));
  const probeOk = !!px && px[0] <= 2 && Math.abs(px[1] - 0x66) <= 2 && Math.abs(px[2] - 0xcc) <= 2;
  const contextLost = await page.evaluate(() => {
    const gl = document.getElementById("screen").getContext("webgl2");
    return gl ? gl.isContextLost() : null;
  });
  console.log(
    `__bib: verdict=${bib.verdict} exactBlue=${bib.exactBlue} redGlyph=${bib.redGlyph} ` +
    `ticks=${bib.ticks} | gpuOn=${gpuOn} fellBack=${fellBack} contextLost=${contextLost} ` +
    `probe=[${px}] probeOk=${probeOk}`
  );
  await page
    .locator("#screen")
    .screenshot({ path: "build/gate8-gpu.png" })
    .catch(() => {});
  if (bib.verdict === "PASS" && gpuOn && !fellBack && !contextLost && probeOk) {
    console.log("GATE8-GPU: PASS");
  } else {
    console.log("GATE8-GPU: FAIL");
    process.exitCode = 1;
  }
} catch (e) {
  console.log("GATE8-GPU: FAIL (timeout)");
  console.log("--- page log ---");
  console.log(await page.textContent("#log").catch(() => "(no log)"));
  process.exitCode = 1;
}
// COOP/COEP engine page: browser.close() can hang forever (2026-06-11,
// zombie probe trees) -- race it against a timeout, then hard-exit.
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 5000))]);
process.exit(process.exitCode ?? 0);
