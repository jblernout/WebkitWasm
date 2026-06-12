// THE probe gate8 was missing: do GPU-mode frames actually reach the
// VISIBLE placeholder canvas? Engine-side readbacks can't see this —
// screenshot the on-screen canvas during a guest animation and count
// distinct frames. Headed (real GPU): DISPLAY=:0 BIB_HEADED=1 node ...
import { chromium } from "playwright";
import { createHash } from "node:crypto";

const browser = await chromium.launch({
  channel: process.env.BIB_CHANNEL || "chromium",
  headless: process.env.BIB_HEADED !== "1",
});
const page = await browser.newPage();
page.on("console", (m) => {
  const t = m.text();
  if (t.includes("present-bench") || t.includes("gpu=")) console.log(" ", t.slice(t.indexOf("EMBEDDER:")));
});

await page.goto("http://127.0.0.1:8080/browser.html?gpu=1&demo=hello");
await page.waitForFunction(() => window.__bib && window.__bib.ready, undefined, { timeout: 120000 });
await page.waitForTimeout(1500);

// Guest animation: rAF loop cycling the body background hue.
await page.evaluate(() => window.__bib.eval(`
  (() => { let h = 0;
    const step = () => { h = (h + 7) % 360;
      document.body.style.background = "hsl(" + h + ",90%,50%)";
      requestAnimationFrame(step); };
    requestAnimationFrame(step); })();`));
await page.waitForTimeout(1000);

const hashes = [];
for (let i = 0; i < 8; i++) {
  const shot = await page.locator("#screen").screenshot();
  hashes.push(createHash("sha1").update(shot).digest("hex").slice(0, 10));
  await page.waitForTimeout(400);
}
const distinct = new Set(hashes).size;
console.log("frames:", hashes.join(" "));
console.log(`GPU-VISIBLE: ${distinct}/8 distinct -> ${distinct >= 6 ? "PASS (screen is live)" : "FAIL (commits not reaching screen)"}`);
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 5000))]);
process.exit(distinct >= 6 ? 0 : 1);
