// URL-bar test: typing a URL in #urlbar + Enter navigates the ENGINE in
// place — no host page reload (wasm instance survives), ?url= updated via
// history.replaceState.
//
// Serve first: dev-server + wisp (the engine fetches through the network).
import { chromium } from "playwright";

const browser = await chromium.launch();
const page = await browser.newPage();
page.on("pageerror", (e) => console.log("[pageerror]", e.message));

await page.goto("http://127.0.0.1:8080/browser.html?demo=hello");
await page.waitForFunction(() => window.__bib && window.__bib.ready, {
  timeout: 180000,
});
// Marker that dies if the host page reloads.
await page.evaluate(() => (window.__noReloadMarker = 42));

await page.fill("#urlbar", "http://127.0.0.1:8080/gate4/page.html");
await page.press("#urlbar", "Enter");

// Boot page replaced by the fetched gate4 page (green div at (50,126)).
await page.waitForFunction(
  () => {
    const p = window.__bib.probe(50, 126);
    return p && p[0] === 0x00 && p[1] === 0xaa && p[2] === 0x44;
  },
  { timeout: 60000 }
);

const checks = [];
function check(name, ok, detail = "") {
  checks.push(ok);
  console.log(`${ok ? "PASS" : "FAIL"}: ${name}${detail ? " " + detail : ""}`);
}

const green = await page.evaluate(() => window.__bib.probe(50, 126));
check("engine navigated via URL bar", green && green[1] === 0xaa, `probe=[${green}]`);
const marker = await page.evaluate(() => window.__noReloadMarker);
check("host page did NOT reload", marker === 42, `marker=${marker}`);
const search = await page.evaluate(() => location.search);
check(
  "?url= updated via replaceState",
  search.includes("url=") && search.includes("gate4"),
  search
);

const failed = checks.filter((ok) => !ok).length;
console.log(`URLBAR: ${failed === 0 ? "PASS" : "FAIL"} (${checks.length - failed}/${checks.length})`);
if (failed) process.exitCode = 1;
// COOP/COEP engine page: browser.close() can hang forever (2026-06-11,
// zombie probe trees) -- race it against a timeout, then hard-exit.
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 5000))]);
process.exit(process.exitCode ?? 0);
