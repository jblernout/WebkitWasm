// Gate 8 (G3 + W-B2/G4): the engine paints the hello page through
// Skia-Ganesh/WebGL2 (?gpu=1) with the engine on its pthread driving the
// page canvas via a transferred OffscreenCanvas. Asserts:
//   1. gpu=on boot + hello verdict via the async readback path (±2).
//   2. __bib.probe works under GPU (blue region pixel).
//   3. G4: WEBGL_lose_context lose→restore recovers IN PLACE (no reload):
//      "gpu context restored" line, then the probe sees blue again.
// NOTE: the page canvas is a PLACEHOLDER under pthreads — getContext on it
// throws, and webglcontextlost fires worker-side. Context state is
// asserted through engine console lines, not page-side GL.
// Requires a REAL GPU-capable Chromium: Playwright's headless-shell loses
// WebGL contexts at first composite, so this gate launches channel
// "chromium" (override with BIB_CHANNEL).
// Serve first:
//   node tools/dev-server.mjs web --mount /engine=build/webcore/bin
// then:
//   node tools/gate8-gpu-test.mjs [url]
import { chromium } from "playwright";

const url = process.argv[2] ?? "http://127.0.0.1:8080/browser.html?demo=hello&gpu=1";
const browser = await chromium.launch({ channel: process.env.BIB_CHANNEL || "chromium" });
const page = await browser.newPage();

let gpuOn = false, fellBack = false, lostSeen = false, restoredSeen = false;
page.on("console", (m) => {
  const t = m.text();
  if (t.startsWith("[bib] ")) console.log(t);
  if (t.includes("EMBEDDER: gpu=on")) gpuOn = true;
  if (t.includes("REQUESTED-BUT-UNAVAILABLE")) fellBack = true;
  // Late fallback: surface/FBO-wrap failure AFTER gpu=on (Codex, G3) —
  // the engine paints CPU raster the GPU-mode host never displays.
  if (t.includes("gpu surface setup failed")) fellBack = true;
  if (t.includes("reloading in raster mode")) fellBack = true;
  if (t.includes("gpu restore REBUILD FAILED")) fellBack = true;
  if (t.includes("gpu restore TIMED OUT")) fellBack = true;
  if (t.includes("gpu context LOST")) lostSeen = true;
  if (t.includes("gpu context restored")) restoredSeen = true;
});
page.on("pageerror", (e) => console.log("[pageerror]", e.message));

const probeBlue = async () => {
  const px = await page.evaluate(() => window.__bib.probe(50, 126));
  return { px, ok: !!px && px[0] <= 2 && Math.abs(px[1] - 0x66) <= 2 && Math.abs(px[2] - 0xcc) <= 2 };
};

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
  const probe1 = await probeBlue();

  // G4 exercise: lose the engine's context via WEBGL_lose_context, restore
  // 500ms later (engine-side setTimeout) — expect in-place recovery, no
  // page reload.
  let g4Ok = false, probe2 = { px: null, ok: false };
  if (bib.verdict === "PASS") {
    await page.evaluate(() => window.Module._bib_gpu_test_lose_restore());
    await page
      .waitForEvent("console", {
        predicate: (m) => m.text().includes("gpu context restored"),
        timeout: 15000,
      })
      .catch(() => {});
    // Full-frame damage was queued on restore; the probe forces its own
    // repaint anyway — a short settle then re-probe.
    await page.waitForTimeout(500);
    probe2 = await probeBlue();
    g4Ok = lostSeen && restoredSeen && !fellBack && probe2.ok;
  }

  console.log(
    `__bib: verdict=${bib.verdict} exactBlue=${bib.exactBlue} redGlyph=${bib.redGlyph} ` +
    `ticks=${bib.ticks} | gpuOn=${gpuOn} fellBack=${fellBack} ` +
    `probe=[${probe1.px}] probeOk=${probe1.ok} | ` +
    `g4: lost=${lostSeen} restored=${restoredSeen} probeAfter=[${probe2.px}] g4Ok=${g4Ok}`
  );
  await page
    .locator("#screen")
    .screenshot({ path: "build/gate8-gpu.png" })
    .catch(() => {});
  if (bib.verdict === "PASS" && gpuOn && !fellBack && probe1.ok && g4Ok) {
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
