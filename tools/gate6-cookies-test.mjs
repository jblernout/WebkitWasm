// Gate 6 — cookies: document.cookie round-trips through the in-memory
// NetworkStorageSession, navigator.cookieEnabled is true, and a network
// Set-Cookie is stored then re-attached as a Cookie request header on the
// next fetch (the google.com "Cookies are disabled" fix).
//
// Serve first:
//   node tools/dev-server.mjs web --mount /engine=build/webcore/bin
//   npm run wisp
import { chromium } from "playwright";

const url =
  "http://127.0.0.1:8080/browser.html?demo=hello&url=" +
  encodeURIComponent("http://127.0.0.1:8080/gate6/cookies.html");
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
  async () => {
    const p = await window.__bib.probe(50, 126);
    return p && !(p[0] === 0x00 && p[1] === 0x66 && p[2] === 0xcc);
  },
  { timeout: 90000 }
);
// Wait for all four signal divs to leave gray (#777777) — the network
// checks are async (chained fetches through curl/Wisp, one with a 302).
try {
  await page.waitForFunction(
    async () => {
      const gray = (p) => p && p[0] === 0x77 && p[1] === 0x77 && p[2] === 0x77;
      const ps = await Promise.all([52, 152, 252, 352].map((x) => window.__bib.probe(x, 35)));
      return ps.every((p) => !gray(p));
    },
    { timeout: 30000 }
  );
} catch {
  console.log("note: some signals never left gray");
}

const checks = [];
function check(name, ok, detail = "") {
  checks.push(ok);
  console.log(`${ok ? "PASS" : "FAIL"}: ${name}${detail ? " " + detail : ""}`);
}

const px = async (x, y) => page.evaluate(([a, b]) => window.__bib.probe(a, b), [x, y]);
const green = (p) => p && p[0] === 0x00 && p[1] === 0xcc && p[2] === 0x00;

const dom = await px(52, 35);
check("document.cookie round-trip", green(dom), `probe=[${dom}] want=[0,204,0]`);
const enabled = await px(152, 35);
check("navigator.cookieEnabled", green(enabled), `probe=[${enabled}] want=[0,204,0]`);
const net = await px(252, 35);
check("network Set-Cookie stored + Cookie header attached", green(net), `probe=[${net}] want=[0,204,0]`);
const redir = await px(352, 35);
check("3xx Set-Cookie stored + cookies attached on redirected hop", green(redir), `probe=[${redir}] want=[0,204,0]`);

await page
  .locator("#screen")
  .screenshot({ path: "build/gate6-cookies.png" })
  .catch(() => {});

const failed = checks.filter((ok) => !ok).length;
console.log(`GATE6-COOKIES: ${failed === 0 ? "PASS" : "FAIL"} (${checks.length - failed}/${checks.length})`);
if (failed) process.exitCode = 1;
// COOP/COEP engine page: browser.close() can hang forever (2026-06-11,
// zombie probe trees) -- race it against a timeout, then hard-exit.
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 5000))]);
process.exit(process.exitCode ?? 0);
