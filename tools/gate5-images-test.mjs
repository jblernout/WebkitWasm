// Gate 5a — image rendering: the engine fetches a page containing a network
// PNG, a network GIF, and a data: PNG, decodes all three, and paints them.
// Isolates the image-decode pipeline from general networking (gate 4 already
// proves HTML/CSS fetch+paint).
//
// Serve first:
//   node tools/dev-server.mjs web --mount /engine=build/webcore/bin
//   npm run wisp
import { chromium } from "playwright";

const url =
  "http://127.0.0.1:8080/browser.html?demo=hello&url=" +
  encodeURIComponent("http://127.0.0.1:8080/gate5/images.html");
const browser = await chromium.launch();
const page = await browser.newPage();
page.on("console", (m) => {
  const t = m.text();
  if (t.startsWith("[bib] ") && !t.includes("wisp: stream"))
    console.log(" ", t);
});
page.on("pageerror", (e) => console.log("[pageerror]", e.message));

await page.goto(url);
await page.waitForFunction(() => window.__bib && window.__bib.ready, {
  timeout: 180000,
});
// Wait for the boot page to be replaced (hello blue div gone).
await page.waitForFunction(
  () => {
    const p = window.__bib.probe(50, 126);
    return p && !(p[0] === 0x00 && p[1] === 0x66 && p[2] === 0xcc);
  },
  { timeout: 90000 }
);
// Let image subresources land and repaint.
await page.waitForTimeout(8000);

const checks = [];
function check(name, ok, detail = "") {
  checks.push(ok);
  console.log(`${ok ? "PASS" : "FAIL"}: ${name}${detail ? " " + detail : ""}`);
}

const px = async (x, y) => page.evaluate(([a, b]) => window.__bib.probe(a, b), [x, y]);
const eq = (p, r, g, b) => p && p[0] === r && p[1] === g && p[2] === b;

const netpng = await px(50, 50);
check("network PNG decoded+painted", eq(netpng, 0xcc, 0x22, 0x11), `probe=[${netpng}] want=[204,34,17]`);
const netgif = await px(150, 50);
check("network GIF decoded+painted", eq(netgif, 0x11, 0xaa, 0x33), `probe=[${netgif}] want=[17,170,51]`);
const datapng = await px(250, 50);
check("data: PNG decoded+painted", eq(datapng, 0xcc, 0x22, 0x11), `probe=[${datapng}] want=[204,34,17]`);

// Diagnostic signals painted by the page's own script: green = the <img>
// fired load, red = it fired error, gray #777 = neither yet.
const signal = (p) =>
  !p ? "?" : eq(p, 0x00, 0xcc, 0x00) ? "load" : eq(p, 0xcc, 0x00, 0x00) ? "ERROR" : eq(p, 0x77, 0x77, 0x77) ? "no-event" : `[${p}]`;
console.log(
  `img events: netpng=${signal(await px(50, 135))} netgif=${signal(await px(150, 135))} datapng=${signal(await px(250, 135))}`
);

await page
  .locator("#screen")
  .screenshot({ path: "build/gate5-images.png" })
  .catch(() => {});

const failed = checks.filter((ok) => !ok).length;
console.log(`GATE5-IMAGES: ${failed === 0 ? "PASS" : "FAIL"} (${checks.length - failed}/${checks.length})`);
if (failed) process.exitCode = 1;
await browser.close();
