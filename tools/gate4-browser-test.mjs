// Gate 4 browser test: the engine FETCHES a real http page over the Wisp
// transport (curl -> Emscripten SOCKFS -> WispWebSocket -> wisp server ->
// TCP) and renders it on the canvas.
//
// Serve first (three processes):
//   node tools/dev-server.mjs web --mount /engine=build/webcore/bin
//   node_modules/.bin/wisp-js-server -H 127.0.0.1 -P 5001 \
//     -O '{"allow_loopback_ips": true, "allow_private_ips": true}'
// then:
//   node tools/gate4-browser-test.mjs [url]
//
// The engine boots on the built-in hello page, then navigates to
// web/gate4/page.html THROUGH THE NETWORK STACK. Pixel verdicts from the
// engine's own RGBA bytes (__bib.probe):
//   (50,126)  #00aa44  fetched green div (parse+layout+paint of network bytes)
//   (50,190)  #aa00aa  div created by an inline <script> IN the fetched page
import { chromium } from "playwright";

const url =
  process.argv[2] ??
  "http://127.0.0.1:8080/browser.html?demo=hello&url=" +
    encodeURIComponent("http://127.0.0.1:8080/gate4/page.html");
const browser = await chromium.launch();
const page = await browser.newPage();

let wispStreams = 0;
page.on("console", (m) => {
  const t = m.text();
  if (t.startsWith("[bib] ")) {
    if (t.includes("wisp: stream ->")) wispStreams++;
    console.log(t);
  }
});
page.on("pageerror", (e) => console.log("[pageerror]", e.message));

await page.goto(url);
console.log("crossOriginIsolated:", await page.textContent("#coi"));

const checks = [];
function check(name, ok, detail = "") {
  checks.push({ name, ok });
  console.log(`${ok ? "PASS" : "FAIL"}: ${name}${detail ? " " + detail : ""}`);
}

try {
  await page.waitForFunction(() => window.__bib && window.__bib.ready, {
    timeout: 180000,
  });

  // The navigation needs network round trips through the wisp pipe plus
  // parse/layout/paint, all pumped at rAF cadence.
  await page.waitForFunction(
    async () => {
      const p = await window.__bib.probe(50, 126);
      return p && p[0] === 0x00 && p[1] === 0xaa && p[2] === 0x44;
    },
    { timeout: 60000 }
  );
  check("fetched page painted (green div)", true);

  // Layout: h1 top margin collapses through body -> body at y=20, h1 band
  // 20..96, green div 96..196, script-created magenta div 196..246.
  const js = await page.evaluate(() => window.__bib.probe(50, 215));
  check(
    "script in fetched page ran (magenta div)",
    js && js[0] === 0xaa && js[1] === 0x00 && js[2] === 0xaa,
    `probe=[${js}]`
  );

  check("wisp transport used", wispStreams > 0, `streams=${wispStreams}`);
} catch (e) {
  check("fetched page painted (green div)", false, "(timeout)");
  console.log("--- page log ---");
  console.log(await page.textContent("#log").catch(() => "(no log)"));
}

await page
  .locator("#screen")
  .screenshot({ path: "build/gate4-canvas.png" })
  .catch(() => {});

const failed = checks.filter((c) => !c.ok).length;
console.log(
  `GATE4-BROWSER: ${failed === 0 ? "PASS" : "FAIL"} (${checks.length - failed}/${checks.length})`
);
if (failed) process.exitCode = 1;
// COOP/COEP engine page: browser.close() can hang forever (2026-06-11,
// zombie probe trees) -- race it against a timeout, then hard-exit.
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 5000))]);
process.exit(process.exitCode ?? 0);
