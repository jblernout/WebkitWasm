// Repro for the google-login bugs: (1) typed email invisible/half-covered,
// (2) post-Enter gray "loading" that never resolves. Boots the engine on
// accounts.google.com, screenshots each step, types into the email field,
// hits Enter, then screenshots the aftermath on an interval.
//
//   node tools/google-login-repro.mjs [click-x] [click-y]
// (coords are engine-canvas pixels for the email field; default guesses)
import { chromium } from "playwright";

const clickX = Number(process.argv[2] ?? 400);
const clickY = Number(process.argv[3] ?? 300);

const url =
  "http://127.0.0.1:8080/browser.html?url=" +
  encodeURIComponent("https://accounts.google.com/");
// BIB_CHANNEL=chromium routes around the headless-shell V8 SEGV (task #38).
const browser = await chromium.launch(
  process.env.BIB_CHANNEL ? { channel: process.env.BIB_CHANNEL } : {}
);
const page = await browser.newPage();
page.on("console", (m) => {
  const t = m.text();
  if (t.startsWith("[bib] ") && !t.includes("wisp: stream"))
    console.log(" ", t.slice(0, 300));
});
page.on("pageerror", (e) => {
  console.log("[pageerror]", e.message);
  if (e.stack) for (const l of e.stack.split("\n").slice(0, 14)) console.log("  " + l.trim());
});

await page.goto(url);
await page.waitForFunction(() => window.__bib && window.__bib.ready, { timeout: 180000 });
console.log("engine ready, waiting for login page to settle...");
await page.waitForTimeout(20000);

const shot = (name) =>
  page.locator("#screen").screenshot({ path: `build/google-login-${name}.png` }).catch(() => {});
await shot("1-loaded");

// Click the email field, type, screenshot.
const canvas = page.locator("#screen");
const box = await canvas.boundingBox();
const sx = box.width / (await page.evaluate(() => window.Module._bib_frame_width()));
const sy = box.height / (await page.evaluate(() => window.Module._bib_frame_height()));
await page.mouse.click(box.x + clickX * sx, box.y + clickY * sy);
await page.waitForTimeout(1500);
await shot("2-clicked");

await page.keyboard.type("bibtest@example.com", { delay: 80 });
await page.waitForTimeout(2500);
await shot("3-typed");

await page.keyboard.press("Enter");
console.log("Enter pressed; sampling aftermath...");
let elapsed = 0;
for (const delta of [3, 5, 7, 10]) {
  await page.waitForTimeout(delta * 1000);
  elapsed += delta;
  await shot(`4-after-enter-${elapsed}s`);
  console.log(`  screenshot at +${elapsed}s`);
}
await browser.close();
console.log("done — see build/google-login-*.png");
