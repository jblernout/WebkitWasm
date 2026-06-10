// Modern-site smoke test: the engine fetches a REAL https site over Wisp,
// applies its CSS, and paints. Not a pixel-exact gate (live content drifts);
// asserts structure-level invariants instead.
//
// Default target: https://news.ycombinator.com/ — light, modern-ish, stable
// brand color (#ff6600 header bar from its fetched news.css).
//
// Serve first:
//   node tools/dev-server.mjs web --mount /engine=build/webcore/bin
//   node_modules/.bin/wisp-js-server -H 127.0.0.1 -P 5001 \
//     -O '{"allow_loopback_ips": true, "allow_private_ips": true}'
// then:
//   node tools/smoke-modern-site.mjs [https-url]
import { chromium } from "playwright";

const target = process.argv[2] ?? "https://news.ycombinator.com/";
const url =
  "http://127.0.0.1:8080/browser.html?demo=hello&url=" +
  encodeURIComponent(target);
const browser = await chromium.launch();
const page = await browser.newPage();

let wispStreams = 0;
page.on("console", (m) => {
  const t = m.text();
  if (t.startsWith("[bib] ") && t.includes("wisp: stream ->")) wispStreams++;
});
page.on("pageerror", (e) => console.log("[pageerror]", e.message));

await page.goto(url);
await page.waitForFunction(() => window.__bib && window.__bib.ready, {
  timeout: 180000,
});

const checks = [];
function check(name, ok, detail = "") {
  checks.push(ok);
  console.log(`${ok ? "PASS" : "FAIL"}: ${name}${detail ? " " + detail : ""}`);
}

// Wait until the canvas stops looking like the boot page: the hello page's
// blue div sits at (50,126); the fetched site replaces it.
try {
  await page.waitForFunction(
    () => {
      const p = window.__bib.probe(50, 126);
      return p && !(p[0] === 0x00 && p[1] === 0x66 && p[2] === 0xcc);
    },
    { timeout: 90000 }
  );
} catch {
  console.log("note: boot page never replaced");
}
// Let subresources (CSS) land and restyle.
await page.waitForTimeout(15000);

const sampled = await page.evaluate(() => {
  let nonWhite = 0,
    samples = 0;
  const colors = new Set();
  for (let y = 2; y < 600; y += 4)
    for (let x = 2; x < 800; x += 8) {
      const p = window.__bib.probe(x, y);
      if (!p) continue;
      samples++;
      if (!(p[0] === 255 && p[1] === 255 && p[2] === 255)) nonWhite++;
      colors.add((p[0] << 16) | (p[1] << 8) | p[2]);
    }
  return { nonWhite, samples, distinctColors: colors.size };
});

check("wisp streams opened (TLS to real host)", wispStreams >= 1, `streams=${wispStreams}`);
check(
  "page painted substantial content",
  sampled.nonWhite > 200,
  `nonWhite=${sampled.nonWhite}/${sampled.samples}`
);
check(
  "styled output (multiple colors -> CSS applied)",
  sampled.distinctColors > 10,
  `distinct=${sampled.distinctColors}`
);
if (target.includes("news.ycombinator.com")) {
  // The #ff6600 header bar comes from the FETCHED news.css subresource.
  const hdr = await page.evaluate(() => window.__bib.probe(400, 10));
  check(
    "HN header bar color from fetched CSS",
    hdr && hdr[0] === 0xff && hdr[1] === 0x66 && hdr[2] === 0x00,
    `probe=[${hdr}]`
  );
}

await page
  .locator("#screen")
  .screenshot({ path: "build/smoke-modern-site.png" })
  .catch(() => {});

const failed = checks.filter((ok) => !ok).length;
console.log(
  `SMOKE-MODERN-SITE: ${failed === 0 ? "PASS" : "FAIL"} (${checks.length - failed}/${checks.length}) target=${target}`
);
if (failed) process.exitCode = 1;
await browser.close();
