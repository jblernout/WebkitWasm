// Static dev server with the COOP/COEP headers required for
// crossOriginIsolated wasm hosting. No deps.
//
//   node tools/dev-server.mjs [root-dir] [--mount /prefix=dir]...
//   (default root: web/, port: $PORT or 8080)
//
// --mount maps a URL prefix to a directory OUTSIDE the root, e.g.
//   --mount /engine=build/webcore/bin
// so multi-GB build artifacts are served in place instead of being copied
// into web/. Each mount gets the same realpath+containment guard as the root.

import { createServer } from "node:http";
import { createReadStream } from "node:fs";
import { stat, realpath } from "node:fs/promises";
import { join, sep, extname, resolve } from "node:path";

const positional = [];
const mounts = []; // [{ prefix: "/engine", root: "/abs/dir" }]
for (let i = 2; i < process.argv.length; i++) {
  const arg = process.argv[i];
  if (arg === "--mount") {
    const spec = process.argv[++i] ?? "";
    const eq = spec.indexOf("=");
    if (eq < 1 || !spec.startsWith("/")) {
      console.error(`dev server: bad --mount "${spec}" (want /prefix=dir)`);
      process.exit(1);
    }
    mounts.push({
      prefix: spec.slice(0, eq).replace(/\/+$/, ""),
      root: await realpath(resolve(spec.slice(eq + 1))),
    });
  } else {
    positional.push(arg);
  }
}
// Longest prefix wins so /engine/sub can coexist with /engine.
mounts.sort((a, b) => b.prefix.length - a.prefix.length);

const ROOT = await realpath(resolve(positional[0] ?? "web"));
const PORT = Number(process.env.PORT ?? 8080);

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript",
  ".mjs": "text/javascript",
  ".css": "text/css",
  ".json": "application/json",
  ".map": "application/json",
  ".wasm": "application/wasm",
  ".data": "application/octet-stream",
  ".png": "image/png",
  ".jpg": "image/jpeg",
  ".svg": "image/svg+xml",
  ".ico": "image/x-icon",
  ".woff2": "font/woff2",
};

const inRoot = (p, root) => p === root || p.startsWith(root + sep);

const server = createServer(async (req, res) => {
  res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
  res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
  // no-cache (NOT no-store): the browser may store but must revalidate, so
  // unchanged artifacts answer 304 and the browsers' wasm machine-code
  // caches stay usable. no-store forced a full re-download AND a full
  // recompile of the ~88 MB engine module on EVERY reload — in Firefox
  // that's a multi-GB transient compile spike per reload.
  res.setHeader("Cache-Control", "no-cache");

  try {
    const url = new URL(req.url, "http://localhost");
    const pathname = decodeURIComponent(url.pathname);

    // gate6 cookie endpoints — static files can't Set-Cookie. The engine
    // (through curl/Wisp) hits /set, then /echo must see the cookie back.
    // Path=/cookie-test keeps the test cookies off every other dev path;
    // /echo reflects ONLY bib* test cookies, not the whole Cookie header
    // (Codex 2026-06-10).
    if (pathname === "/cookie-test/set") {
      res
        .writeHead(200, {
          "Content-Type": "text/plain",
          "Set-Cookie": "bibtest=42; Path=/cookie-test",
        })
        .end("set");
      return;
    }
    // 302 leg that ALSO sets a cookie — proves Set-Cookie on a 3xx is
    // stored and re-attached on the redirected hop (gate6 check 4).
    if (pathname === "/cookie-test/redirect-set") {
      res
        .writeHead(302, {
          "Set-Cookie": "bibredir=9; Path=/cookie-test",
          Location: "/cookie-test/echo",
        })
        .end();
      return;
    }
    if (pathname === "/cookie-test/echo") {
      const bibOnly = (req.headers.cookie ?? "")
        .split(";")
        .map((c) => c.trim())
        .filter((c) => c.startsWith("bib"))
        .join("; ");
      res
        .writeHead(200, { "Content-Type": "text/plain" })
        .end(bibOnly);
      return;
    }

    let root = ROOT;
    let rel = pathname;
    const mount = mounts.find(
      (m) => pathname === m.prefix || pathname.startsWith(m.prefix + "/")
    );
    if (mount) {
      root = mount.root;
      rel = pathname.slice(mount.prefix.length) || "/";
    }

    const candidate = resolve(join(root, rel));
    if (!inRoot(candidate, root)) {
      res.writeHead(403).end("403");
      return;
    }

    // realpath resolves symlinks, so a link placed inside the root can't
    // serve files from outside it
    let file = await realpath(candidate);
    let s = await stat(file);
    if (s.isDirectory()) {
      file = await realpath(join(file, "index.html"));
      s = await stat(file);
    }
    if (!inRoot(file, root)) {
      res.writeHead(403).end("403");
      return;
    }

    // Weak validator from size+mtime — enough for local build artifacts.
    const etag = `W/"${s.size}-${Math.floor(s.mtimeMs)}"`;
    if (req.headers["if-none-match"] === etag) {
      res.writeHead(304, { ETag: etag }).end();
      return;
    }

    // stream instead of buffering: .wasm/.data artifacts can be huge
    res.writeHead(200, {
      "Content-Type": MIME[extname(file)] ?? "application/octet-stream",
      "Content-Length": s.size,
      ETag: etag,
    });
    const stream = createReadStream(file);
    stream.on("error", () => res.destroy());
    stream.pipe(res);
  } catch {
    res.writeHead(404, { "Content-Type": "text/plain" }).end("404");
  }
});

server.on("error", (err) => {
  console.error(
    err.code === "EADDRINUSE"
      ? `dev server: port ${PORT} is already in use`
      : `dev server: ${err.message}`
  );
  process.exit(1);
});

server.listen(PORT, "127.0.0.1", () => {
  const mountDesc = mounts.map((m) => ` ${m.prefix}=>${m.root}`).join("");
  console.log(
    `dev server: http://127.0.0.1:${PORT}  root=${ROOT}${mountDesc}  COOP/COEP=on`
  );
});
