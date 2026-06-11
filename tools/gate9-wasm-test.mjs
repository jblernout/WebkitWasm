// Gate 9 (shim S-A): the guest WebAssembly polyfill works end-to-end —
// engine injects it per window, sync Module/Instance runs Discord's real
// wasmSupported probe module, async instantiate plumbs an import object,
// exported memory is readable, instantiateStreaming fetches through the
// engine network stack. The probe page logs WASMPROBE lines on the guest
// console (forwarded to the host) and a final WASMPROBE-VERDICT.
// No GPU involvement: runs raster (automation default), headless ok.
// Serve first:
//   node tools/dev-server.mjs web --mount /engine=build/webcore/bin
// then:
//   node tools/gate9-wasm-test.mjs [url]
import { chromium } from "playwright";

const url = process.argv[2]
  ?? "http://127.0.0.1:8080/browser.html?url=http://127.0.0.1:8080/probe/wasmprobe.html";
const browser = await chromium.launch({ channel: process.env.BIB_CHANNEL || "chromium" });
const page = await browser.newPage();

let verdict = null;
const probeLines = [];
page.on("console", (m) => {
  const t = m.text();
  if (t.includes("WASMPROBE")) {
    const line = t.slice(t.indexOf("WASMPROBE"));
    probeLines.push(line);
    console.log(line);
    if (line.startsWith("WASMPROBE-VERDICT:"))
      verdict = line.includes("PASS") ? "PASS" : "FAIL";
  }
  if (t.includes("wasm shim unavailable") || t.includes("wasm2js translate failed"))
    console.log("[host] " + t);
});
page.on("pageerror", (e) => console.log("[pageerror]", e.message));

await page.goto(url);

try {
  await page.waitForFunction(() => window.__bib && window.__bib.ready, { timeout: 180000 });
  // verdict arrives via guest console after navigation + guest JS run
  const deadline = Date.now() + 120000;
  while (!verdict && Date.now() < deadline)
    await page.waitForTimeout(500);
  if (verdict === "PASS") {
    console.log("GATE9-WASM: PASS");
  } else {
    console.log(`GATE9-WASM: FAIL (verdict=${verdict ?? "none"}, lines=${probeLines.length})`);
    process.exitCode = 1;
  }
} catch (e) {
  console.log("GATE9-WASM: FAIL (engine never came up)", e.message);
  process.exitCode = 1;
}
// COOP/COEP engine page: browser.close() can hang forever -- race it.
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 5000))]);
process.exit(process.exitCode ?? 0);
