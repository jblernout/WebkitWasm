// W-B0 spike runner (analysis-wb-engine-off-main-thread.md §4).
// Serve first:
//   PORT=8090 node tools/dev-server.mjs web --mount /wbspike=build/wb-spike
// then:
//   node tools/wb-spike-test.mjs [base-url]
// Env: BIB_WB_BROWSERS="chromium,firefox,webkit" (default chromium only;
// chromium honors BIB_CHANNEL like the gates). Only chromium results gate
// the exit code — firefox/webkit runs are Q2/Q3 survey data.
import * as pw from "playwright";

const base = process.argv[2] ?? "http://127.0.0.1:8090/wb-spike.html";
const browsers = (process.env.BIB_WB_BROWSERS || "chromium").split(",").map((s) => s.trim());

async function runOne(browserName, url, { collectMs = 30000, waitVerdict = true } = {}) {
  const launcher = pw[browserName];
  if (!launcher)
    return { skip: `unknown browser ${browserName}` };
  let browser;
  try {
    browser = await launcher.launch(
      browserName === "chromium" ? { channel: process.env.BIB_CHANNEL || "chromium" } : {});
  } catch (e) {
    return { skip: `launch failed: ${e.message.split("\n")[0]}` };
  }
  const page = await browser.newPage();
  const lines = [];
  let verdict = null;
  page.on("console", (m) => {
    const t = m.text();
    if (!t.includes("WBSPIKE")) return;
    const line = t.slice(t.indexOf("WBSPIKE"));
    lines.push(line);
    if (line.startsWith("WBSPIKE-VERDICT:"))
      verdict = line.includes("PASS") ? "PASS" : "FAIL";
  });
  const pageErrors = [];
  page.on("pageerror", (e) => pageErrors.push((e.stack || e.message || String(e)).split("\n").slice(0, 10).join(" | ")));

  await page.goto(url);
  const deadline = Date.now() + collectMs;
  while (Date.now() < deadline) {
    if (waitVerdict && verdict) break;
    await page.waitForTimeout(250);
  }
  await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 5000))]);
  return { lines, verdict, pageErrors };
}

// Ticks during the pthread's block window prove the main thread stayed
// responsive — emscripten_get_now() and performance.now() share a clock.
function blockResponsiveness(lines) {
  const t = (re) => { const m = lines.map((l) => re.exec(l)).find(Boolean); return m ? Number(m[1]) : null; };
  const begin = t(/block-begin t=(\d+)/), end = t(/block-end t=(\d+)/);
  if (begin === null || end === null) return { ok: false, why: "no block window" };
  const ticks = lines.map((l) => /main-tick t=(\d+)/.exec(l)).filter(Boolean).map((m) => Number(m[1]))
    .filter((x) => x > begin && x < end);
  return { ok: ticks.length >= 5, why: `${ticks.length} ticks in ${end - begin}ms block window` };
}

let exitCode = 0;
for (const b of browsers) {
  console.log(`\n=== ${b} ===`);
  const r = await runOne(b, base);
  if (r.skip) { console.log(`WBSPIKE-RUNNER: ${b} SKIP (${r.skip})`); continue; }
  for (const l of r.lines.filter((l) => !l.includes("main-tick"))) console.log(l);
  const resp = blockResponsiveness(r.lines);
  console.log(`WBSPIKE-RUNNER: ${b} block-responsive=${resp.ok ? "PASS" : "FAIL"} (${resp.why})`);
  // Q1 gating (Codex): the wisp dispatcher must have actually routed the
  // socket, and it must land in MAIN scope — production keeps its dispatcher
  // in page scope, so a placement change in a future emsdk must FAIL loudly.
  const wsLines = r.lines.filter((l) => l.includes("ws-construct"));
  const scopes = wsLines.map((l) => /scope=(\w+)/.exec(l)?.[1]);
  const routed = r.lines.some((l) => l.includes("ws-routed"));
  const wsOK = routed && scopes.length > 0 && scopes.every((s) => s === "main");
  console.log(`WBSPIKE-RUNNER: ${b} ws-scope=${scopes.join(",") || "NONE"} routed=${routed} q1=${wsOK ? "PASS" : "FAIL"}`);
  console.log(`WBSPIKE-RUNNER: ${b} verdict=${r.verdict ?? "none"}`);

  // Q5: abort mode — what does the page see, and is the wasm frame named?
  let sym = false;
  const a = await runOne(b, base + "?abort=1", { collectMs: 15000, waitVerdict: false });
  if (!a.skip) {
    sym = [...a.lines, ...a.pageErrors].some((l) => l.includes("wb_crash_inner"));
    console.log(`WBSPIKE-RUNNER: ${b} abort-symbolized=${sym ? "yes" : "no"}`);
    for (const l of a.lines.filter((l) => /onAbort|window-error|aborting/.test(l))) console.log("  " + l);
    for (const e of a.pageErrors.slice(0, 2)) console.log("  [pageerror] " + e);
  }
  if (b === "chromium" && (r.verdict !== "PASS" || !resp.ok || !wsOK || !sym)) exitCode = 1;
}
process.exit(exitCode);
