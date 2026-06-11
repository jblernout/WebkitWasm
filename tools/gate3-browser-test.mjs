// Gate 3 browser test: Phase 3 interactivity — real Playwright input on the
// canvas drives WebKit-in-wasm's EventHandler, JSC runs inside the page,
// and the engine repaints through ChromeClient invalidation.
//
// Asserts via window.__bib.probe(x, y) — the engine's own RGBA bytes:
//   1. initial colors (hoverbox #00aa00, result #444444, keyresult #222222)
//   2. hover     -> hoverbox turns #ff8800     (mouse + :hover restyle)
//   3. click     -> result turns #ff00ff       (mouse + in-page JS onclick)
//   4. 'h' key   -> keyresult turns #00ffff    (keyboard reaches page JS)
//   5. type "hi" -> keyresult turns #ffff00    (editor inserts into <input>,
//                                               page JS sees input event)
//   6. wheel     -> marker #123456 scrolls into view (wheel + scroll repaint)
//
// Serve first:
//   node tools/dev-server.mjs web --mount /engine=build/webcore/bin
// then:
//   node tools/gate3-browser-test.mjs [url]
import { chromium } from "playwright";

const url = process.argv[2] ?? "http://127.0.0.1:8080/browser.html";
const browser = await chromium.launch();
const page = await browser.newPage();

page.on("console", (m) => {
  const t = m.text();
  if (t.startsWith("[bib] ")) console.log(t);
});
page.on("pageerror", (e) => console.log("[pageerror]", e.message));

await page.goto(url);
console.log("crossOriginIsolated:", await page.textContent("#coi"));

let failures = 0;
const probe = (x, y) => page.evaluate(([px, py]) => window.__bib.probe(px, py), [x, y]);
const hex = (rgb) => (rgb ? "#" + rgb.map((v) => v.toString(16).padStart(2, "0")).join("") : "null");
async function expectPixel(name, x, y, want) {
  const got = hex(await probe(x, y));
  const ok = got === want;
  if (!ok) failures++;
  console.log(`${ok ? "ok " : "FAIL"} ${name}: probe(${x},${y}) = ${got} (want ${want})`);
}

try {
  await page.waitForFunction(() => window.__bib && window.__bib.ready, { timeout: 180000 });
  await page.waitForFunction(() => window.__bib.ticks >= 5, { timeout: 30000 });

  // Canvas position in host-page coordinates; engine coords are 1:1 inside.
  const box = await page.locator("#screen").boundingBox();
  const at = (x, y) => [box.x + x, box.y + y];

  // 1. Initial state.
  await expectPixel("initial hoverbox", 70, 70, "#00aa00");
  await expectPixel("initial result", 350, 70, "#444444");
  await expectPixel("initial keyresult", 490, 70, "#222222");

  // 2. Hover over the hoverbox -> :hover restyle.
  await page.mouse.move(...at(70, 70));
  await page.waitForTimeout(100);
  await expectPixel("hover restyle", 70, 70, "#ff8800");

  // 3. Click the button -> in-page JS paints the result box.
  await page.mouse.click(...at(210, 70));
  await page.waitForTimeout(100);
  await expectPixel("click via JS", 350, 70, "#ff00ff");

  // 4+5. Focus the input field and type. The 'h' keydown flips keyresult to
  // #00ffff via the page's document listener; once the field's value is
  // "hi", its input listener flips it to #ffff00 (proves editor insertion).
  await page.mouse.click(...at(655, 46));
  await page.waitForTimeout(100);
  await page.keyboard.type("hi", { delay: 60 });
  await page.waitForTimeout(150);
  await expectPixel("typed text reaches input", 490, 70, "#ffff00");

  // 6. Wheel-scroll down 300px: marker (content y 700..800) should appear
  // at viewport y 400..500; the hover box scrolls off into white.
  await page.mouse.move(...at(400, 300));
  await page.mouse.wheel(0, 300);
  await page.waitForTimeout(150);
  await expectPixel("scroll marker visible", 70, 450, "#123456");
  await expectPixel("old content scrolled away", 70, 70, "#ffffff");

  await page
    .locator("#screen")
    .screenshot({ path: "build/gate3-canvas.png" })
    .catch(() => {});

  if (failures === 0) {
    console.log("GATE3-BROWSER: PASS");
  } else {
    console.log(`GATE3-BROWSER: FAIL (${failures} assertion(s))`);
    process.exitCode = 1;
  }
} catch (e) {
  console.log("GATE3-BROWSER: FAIL (timeout/error)", e.message);
  console.log("--- page log ---");
  console.log(await page.textContent("#log").catch(() => "(no log)"));
  process.exitCode = 1;
}
// COOP/COEP engine page: browser.close() can hang forever (2026-06-11,
// zombie probe trees) -- race it against a timeout, then hard-exit.
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 5000))]);
process.exit(process.exitCode ?? 0);
