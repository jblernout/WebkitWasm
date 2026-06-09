// Static dev server with the COOP/COEP headers required for
// SharedArrayBuffer (Emscripten pthreads). No deps.
//
//   node tools/dev-server.mjs [root-dir]   (default root: web/, port: $PORT or 8080)

import { createServer } from "node:http";
import { stat, readFile } from "node:fs/promises";
import { join, sep, extname, resolve } from "node:path";

const ROOT = resolve(process.argv[2] ?? "web");
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

const server = createServer(async (req, res) => {
  res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
  res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
  res.setHeader("Cache-Control", "no-store");

  try {
    const url = new URL(req.url, "http://localhost");
    let file = resolve(join(ROOT, decodeURIComponent(url.pathname)));
    if (file !== ROOT && !file.startsWith(ROOT + sep)) {
      res.writeHead(403).end("403");
      return;
    }
    const s = await stat(file).catch(() => null);
    if (s?.isDirectory()) file = join(file, "index.html");
    const body = await readFile(file);
    res.writeHead(200, {
      "Content-Type": MIME[extname(file)] ?? "application/octet-stream",
    });
    res.end(body);
  } catch {
    res.writeHead(404, { "Content-Type": "text/plain" }).end("404");
  }
});

server.listen(PORT, "127.0.0.1", () => {
  console.log(`dev server: http://127.0.0.1:${PORT}  root=${ROOT}  COOP/COEP=on`);
});
