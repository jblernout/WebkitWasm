// Gate 7 — IndexedDB: window.indexedDB is touchable (root cause #13 — the
// empty-clients DatabaseProvider RELEASE_ASSERTed and killed the page's
// script), an open() round-trips through the in-process IDBServer, put/get
// works inside transactions, and a second connection reads the same
// MemoryIDBBackingStore.
//
// Serve first:
//   node tools/dev-server.mjs web --mount /engine=build/webcore/bin
//   npm run wisp
import { chromium } from "playwright";

const url =
  "http://127.0.0.1:8080/browser.html?demo=hello&url=" +
  encodeURIComponent("http://127.0.0.1:8080/gate7/idb.html");
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
// Wait for all four signal divs to leave gray (#777777) — the IDB
// lifecycle is async (every request bounces through the deferred queue).
try {
  await page.waitForFunction(
    () => {
      const gray = (p) => p && p[0] === 0x77 && p[1] === 0x77 && p[2] === 0x77;
      return [52, 152, 252, 352].every((x) => !gray(window.__bib.probe(x, 35)));
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

const glob = await px(52, 35);
check("window.indexedDB touchable", green(glob), `probe=[${glob}] want=[0,204,0]`);
const open = await px(152, 35);
check("open() + upgrade + success round-trip", green(open), `probe=[${open}] want=[0,204,0]`);
const put = await px(252, 35);
check("transaction put/get round-trip", green(put), `probe=[${put}] want=[0,204,0]`);
const reopen = await px(352, 35);
check("second connection reads same backing store", green(reopen), `probe=[${reopen}] want=[0,204,0]`);

await page
  .locator("#screen")
  .screenshot({ path: "build/gate7-idb.png" })
  .catch(() => {});

const failed = checks.filter((ok) => !ok).length;
console.log(`GATE7-IDB: ${failed === 0 ? "PASS" : "FAIL"} (${checks.length - failed}/${checks.length})`);
if (failed) process.exitCode = 1;
await browser.close();
