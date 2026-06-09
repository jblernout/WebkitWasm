// Static dev server with the COOP/COEP headers required for
// SharedArrayBuffer (Emscripten pthreads). No deps.
//
//   node tools/dev-server.mjs [root-dir]   (default root: web/, port: $PORT or 8080)

import { createServer } from "node:http";
import { createReadStream } from "node:fs";
import { stat, realpath } from "node:fs/promises";
import { join, sep, extname, resolve } from "node:path";

const ROOT = await realpath(resolve(process.argv[2] ?? "web"));
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

const inRoot = (p) => p === ROOT || p.startsWith(ROOT + sep);

const server = createServer(async (req, res) => {
  res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
  res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
  res.setHeader("Cache-Control", "no-store");

  try {
    const url = new URL(req.url, "http://localhost");
    const candidate = resolve(join(ROOT, decodeURIComponent(url.pathname)));
    if (!inRoot(candidate)) {
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
    if (!inRoot(file)) {
      res.writeHead(403).end("403");
      return;
    }

    // stream instead of buffering: .wasm/.data artifacts can be huge
    res.writeHead(200, {
      "Content-Type": MIME[extname(file)] ?? "application/octet-stream",
      "Content-Length": s.size,
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
  console.log(`dev server: http://127.0.0.1:${PORT}  root=${ROOT}  COOP/COEP=on`);
});
