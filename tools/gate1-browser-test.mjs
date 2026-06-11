// Gate 1 browser test: load gate1.html in headless Chromium and assert
// that WebKit's JSC.wasm evaluated JavaScript inside the tab.
//   node tools/gate1-browser-test.mjs [url]
import { chromium } from "playwright";

const url = process.argv[2] ?? "http://127.0.0.1:8080/gate1.html";
const browser = await chromium.launch();
const page = await browser.newPage();

page.on("console", (m) => {
  const t = m.text();
  if (t.startsWith("[gate1] ")) console.log(t);
});
page.on("pageerror", (e) => console.log("[pageerror]", e.message));

await page.goto(url);
console.log("crossOriginIsolated:", await page.textContent("#coi"));

try {
  await page.waitForFunction(
    () => document.getElementById("log").textContent.includes("JSC-in-tab: 42"),
    { timeout: 120000 }
  );
  console.log("GATE1-BROWSER: PASS");
} catch {
  console.log("GATE1-BROWSER: FAIL");
  console.log("--- page log ---");
  console.log(await page.textContent("#log"));
  process.exitCode = 1;
}
// COOP/COEP engine page: browser.close() can hang forever (2026-06-11,
// zombie probe trees) -- race it against a timeout, then hard-exit.
await Promise.race([browser.close(), new Promise((r) => setTimeout(r, 5000))]);
process.exit(process.exitCode ?? 0);
