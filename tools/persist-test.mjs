// Persistence acceptance: cookies + guest localStorage survive a host-page
// reload via the OPFS profile (web/browser.html persist block + main.cpp
// bibMaybePersist/seed).
//
// Needs a PERSISTENT browser profile: OPFS lives in the profile dir, and the
// usual chromium.launch() gives a throwaway one per run (which is exactly why
// the regular gates are immune to persistence leakage).
//
// Phase A: boot engine on gate6/cookies.html, set a cookie + localStorage key
//          from GUEST script (bib_eval), force a flush, wait for the OPFS
//          blob to contain them.
// Phase B: full host reload, wait for the engine to re-seed, read
//          document.cookie + localStorage from guest script again.
//
// Run: node tools/persist-test.mjs   (serve :8080 + wisp :5001 first)
import { chromium } from "playwright";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const GUEST_URL = "http://127.0.0.1:8080/gate6/cookies.html";
const PAGE_URL = `http://127.0.0.1:8080/browser.html?url=${encodeURIComponent(GUEST_URL)}`;
const COOKIE = "bibpersist=alive";
const LS_KEY = "bibPersistLS";
const LS_VALUE = "lsAlive";

const userDataDir = fs.mkdtempSync(path.join(os.tmpdir(), "bib-persist-"));
const ctx = await chromium.launchPersistentContext(userDataDir, {
  channel: process.env.BIB_CHANNEL || "chromium",
});
const page = ctx.pages()[0] ?? (await ctx.newPage());

const consoleLines = [];
page.on("console", (m) => consoleLines.push(m.text()));

const fail = async (msg) => {
  console.log(`PERSIST-TEST: FAIL ${msg}`);
  console.log("--- last console ---");
  for (const line of consoleLines.slice(-25)) console.log("  " + line.slice(0, 200));
  await ctx.close().catch(() => {});
  fs.rmSync(userDataDir, { recursive: true, force: true });
  process.exit(1);
};

// Sync-predicate polling only (waitForFunction does NOT await async
// predicates under interval polling — the W-B1 vacuous-pass trap).
const waitReady = async (label) => {
  await page.waitForFunction(() => window.__bib && window.__bib.ready === true && !window.__bib.dead,
    null, { polling: 250, timeout: 120000 }).catch(() => fail(`${label}: engine never ready`));
};

const evalInGuest = (code) =>
  page.evaluate((c) => {
    if (!window.Module || !Module.ccall) return 0;
    return Module.ccall("bib_eval", "number", ["string"], [c]);
  }, code);

const waitConsole = async (marker, timeoutMs) => {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    const hit = consoleLines.find((l) => l.includes(marker));
    if (hit) return hit;
    await page.waitForTimeout(250);
  }
  return null;
};

// The guest navigates to GUEST_URL only after onEngineReady — poll the guest
// location until the target document is the one our evals run in.
const waitGuestAt = async (urlPrefix, label) => {
  for (let i = 0; i < 60; i++) {
    const marker = `BIBLOC${i}`;
    await evalInGuest(`console.log('${marker} ' + location.href)`);
    const line = await waitConsole(marker, 2000);
    if (line && line.includes(urlPrefix)) return;
    await page.waitForTimeout(750);
  }
  await fail(`${label}: guest never reached ${urlPrefix}`);
};

const readOPFS = () =>
  page.evaluate(async () => {
    try {
      const root = await navigator.storage.getDirectory();
      const file = await (await root.getFileHandle("bib-state-v1.json")).getFile();
      return await file.text();
    } catch {
      return null;
    }
  });

// --- Phase A: write state, flush, verify OPFS blob -------------------------
console.log("PERSIST-TEST: phase A boot");
await page.goto(PAGE_URL);
await waitReady("phase A");
await waitGuestAt(GUEST_URL, "phase A");

await evalInGuest(
  `document.cookie='${COOKIE}; max-age=86400; path=/';` +
  `localStorage.setItem('${LS_KEY}','${LS_VALUE}');` +
  `console.log('BIBSET cookie=' + document.cookie + ' ls=' + localStorage.getItem('${LS_KEY}'))`);
const setLine = await waitConsole("BIBSET", 5000);
if (!setLine || !setLine.includes(COOKIE) || !setLine.includes(LS_VALUE))
  await fail(`phase A: in-session state not set (${setLine})`);
console.log("PERSIST-TEST: in-session state set OK");

await page.evaluate(() => Module._bib_persist_now());
let blob = null;
for (let i = 0; i < 40 && !blob; i++) {
  const text = await readOPFS();
  if (text && text.includes("bibpersist") && text.includes(LS_VALUE)) blob = text;
  else await page.waitForTimeout(500);
}
if (!blob) await fail("phase A: OPFS blob never contained the test state");
console.log(`PERSIST-TEST: OPFS blob written (${blob.length} bytes)`);

// --- Phase B: reload, verify restore ---------------------------------------
console.log("PERSIST-TEST: phase B reload");
consoleLines.length = 0;
await page.goto(PAGE_URL);
await waitReady("phase B");

const seedLine = await waitConsole("persist seed:", 30000);
if (!seedLine) await fail("phase B: engine never logged a persist seed");
console.log(`PERSIST-TEST: ${seedLine.trim()}`);

await waitGuestAt(GUEST_URL, "phase B");
await evalInGuest(
  `console.log('BIBCHECK cookie=' + document.cookie + ' ls=' + localStorage.getItem('${LS_KEY}'))`);
const checkLine = await waitConsole("BIBCHECK", 5000);
const cookieOK = !!checkLine && checkLine.includes(COOKIE);
const lsOK = !!checkLine && checkLine.includes(`ls=${LS_VALUE}`);
console.log(`PERSIST-TEST: after reload: cookie=${cookieOK} localStorage=${lsOK} (${checkLine})`);

if (!cookieOK || !lsOK) await fail("phase B: state did not survive the reload");
console.log("PERSIST-TEST-VERDICT: PASS");
await ctx.close().catch(() => {});
fs.rmSync(userDataDir, { recursive: true, force: true });
process.exit(0);
