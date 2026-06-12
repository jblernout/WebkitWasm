// M-A acceptance: a guest <audio> actually PLAYS through the host bridge.
//
// Boots browser.html?url=<engine-served audio.html>&media=1, then drives
// the guest page's play() via bib_eval and asserts the guest element's
// lifecycle (canPlayType truthful, playing fired, clock advanced, ended).
// Also asserts the DEFAULT path is unchanged: a second boot WITHOUT
// ?media=1 must still answer canPlayType "" (A2 zero-engine behavior).
//
// Run: node tools/media-test.mjs   (serve :8080 + wisp :5001 first)
import { chromium } from "playwright";

const GUEST_URL = "http://127.0.0.1:8080/media/audio.html";
const pageURL = (media) =>
  `http://127.0.0.1:8080/browser.html?url=${encodeURIComponent(GUEST_URL)}${media ? "&media=1" : ""}`;

const browser = await chromium.launch({
  channel: process.env.BIB_CHANNEL || "chromium",
  args: ["--autoplay-policy=no-user-gesture-required"],
});

const consoleLines = [];
let page;

const fail = async (msg) => {
  console.log(`MEDIA-TEST: FAIL ${msg}`);
  console.log("--- last console ---");
  for (const line of consoleLines.slice(-25)) console.log("  " + line.slice(0, 200));
  await browser.close().catch(() => {});
  process.exit(1);
};

const waitConsole = async (pred, timeoutMs) => {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    const hit = consoleLines.find(pred);
    if (hit) return hit;
    await page.waitForTimeout(250);
  }
  return null;
};

const boot = async (media, label) => {
  consoleLines.length = 0;
  page = await browser.newPage();
  page.on("console", (m) => consoleLines.push(m.text()));
  await page.goto(pageURL(media));
  await page.waitForFunction(() => window.__bib && window.__bib.ready === true && !window.__bib.dead,
    null, { polling: 250, timeout: 120000 }).catch(() => fail(`${label}: engine never ready`));
  // audio.html logs its canPlayType line as soon as it parses.
  const canPlay = await waitConsole((l) => l.includes("MEDIAGATE canPlayType"), 60000);
  if (!canPlay) await fail(`${label}: guest page never reported canPlayType`);
  return canPlay;
};

// --- Phase 1: ?media=1 — audio must actually play ---------------------------
console.log("MEDIA-TEST: phase 1 (media=1)");
const canPlayOn = await boot(true, "phase 1");
console.log(`MEDIA-TEST: ${canPlayOn.trim().slice(0, 120)}`);
if (/wav=\[\]/.test(canPlayOn))
  await fail("phase 1: canPlayType('audio/wav') still empty with ?media=1");

await page.evaluate(() =>
  Module.ccall("bib_eval", "number", ["string"], ["window.mediaGatePlay()"]));

const playing = await waitConsole((l) => l.includes("MEDIAGATE event playing"), 15000);
if (!playing) await fail("phase 1: 'playing' never fired on the guest element");
console.log("MEDIA-TEST: guest 'playing' fired");

const ended = await waitConsole((l) => l.includes("MEDIAGATE event ended"), 15000);
if (!ended) await fail("phase 1: 'ended' never fired (1s beep)");
const tMatch = ended.match(/t=([\d.]+)/);
const clockAdvanced = tMatch && parseFloat(tMatch[1]) > 0.5;
console.log(`MEDIA-TEST: guest 'ended' fired (${ended.trim().slice(ended.indexOf("MEDIAGATE"), ended.indexOf("MEDIAGATE") + 60)})`);
if (!clockAdvanced) await fail("phase 1: guest clock never advanced past 0.5s");

// Host-side witness: the bridge elements are DETACHED Audio()s, reachable
// only through the diagnostics map browser.html exposes.
const hostState = await page.evaluate(() => {
  const out = [];
  for (const [, p] of window.__bibMedia || new Map())
    out.push({ src: p.el.currentSrc.slice(-8), ended: p.el.ended, t: p.el.currentTime });
  return out;
});
console.log(`MEDIA-TEST: host elements: ${JSON.stringify(hostState)}`);
if (!hostState.some((a) => a.ended)) await fail("phase 1: no host element reached ended");
await page.close();

// --- Phase 2: default (no flag) — A2 behavior unchanged ---------------------
console.log("MEDIA-TEST: phase 2 (default, no media flag)");
const canPlayOff = await boot(false, "phase 2");
console.log(`MEDIA-TEST: ${canPlayOff.trim().slice(0, 120)}`);
if (!/wav=\[\] mp3=\[\]/.test(canPlayOff))
  await fail("phase 2: canPlayType not empty WITHOUT ?media=1 — A2 default regressed");
await page.close();

console.log("MEDIA-TEST-VERDICT: PASS");
await browser.close().catch(() => {});
process.exit(0);
