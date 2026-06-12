/*
 * W-B0 spike pre-js — emitted into wb-spike.js, so it executes in EVERY
 * thread scope the module runs in: the main page AND each pthread pool
 * worker. This is the W-B1 plan for the wisp dispatcher (browser.html's
 * page-scope WebSocket override can't see sockets SOCKFS constructs inside
 * the pthread's worker scope — if that's where they land; this spike's
 * ws-construct lines settle it).
 *
 * Worker-scope lines buffer in self.__wbLog; the C side drains them through
 * out() so they reach the page console via the proxied print path.
 *
 * Leading ";": emcc splices pre-js mid-stream after an unterminated
 * expression statement; a bare "(" would continue it as a call (ASI trap).
 */
;(function () {
  var wbScope = (typeof window !== "undefined") ? "main" : "worker";
  var wbLog = self.__wbLog = self.__wbLog || [];
  function wbOut(s) {
    if (wbScope === "main")
      console.log(s);
    else
      wbLog.push(s); // drained by wb_flush_js_log() on the pthread
  }
  wbOut("WBSPIKE: prejs scope=" + wbScope
    + " sab=" + (typeof SharedArrayBuffer !== "undefined")
    + " isolated=" + (typeof crossOriginIsolated !== "undefined" ? crossOriginIsolated : "n/a"));

  // The wisp client library: the page loads it via <script>; a worker scope
  // has to pull it in itself. Same-origin, COEP-safe.
  if (wbScope === "worker" && typeof wisp_client === "undefined" && typeof importScripts === "function") {
    try {
      importScripts("/vendor/wisp-client.js");
      wbOut("WBSPIKE: prejs worker wisp_client=" + (typeof wisp_client));
    } catch (e) {
      wbOut("WBSPIKE: prejs worker importScripts FAIL " + (e && e.message));
    }
  }

  var WbNative = self.WebSocket;
  if (!WbNative) {
    wbOut("WBSPIKE: prejs scope=" + wbScope + " has NO WebSocket global");
    return;
  }
  if (WbNative.__wbWrapped)
    return;

  // Same dispatcher as browser.html INCLUDING the bib-sockfs subprotocol
  // discriminator (Codex: without it the spike could pass where production
  // routing would not): SOCKFS builds exactly "ws://<ip>:<port>/" with
  // subprotocol "bib-sockfs" from Module.websocket; reroute ONLY those.
  var wrapped = function (url, protocols) {
    var m = /^ws:\/\/(\d{1,3}(?:\.\d{1,3}){3}):(\d+)\/$/.exec(String(url));
    var isBib = protocols === "bib-sockfs"
      || (Array.isArray(protocols) && protocols.indexOf("bib-sockfs") !== -1);
    wbOut("WBSPIKE: ws-construct scope=" + wbScope + " url=" + url
      + " protocols=" + JSON.stringify(protocols === undefined ? null : protocols)
      + " bib=" + isBib + " wisp=" + (typeof wisp_client !== "undefined"));
    if (m && isBib && typeof wisp_client !== "undefined") {
      wbOut("WBSPIKE: ws-routed scope=" + wbScope + " -> " + m[1] + ":" + m[2]);
      return new wisp_client.client.WispWebSocket("ws://127.0.0.1:5001/" + m[1] + ":" + m[2]);
    }
    return new WbNative(url, protocols);
  };
  wrapped.prototype = WbNative.prototype;
  wrapped.CONNECTING = 0;
  wrapped.OPEN = 1;
  wrapped.CLOSING = 2;
  wrapped.CLOSED = 3;
  wrapped.__wbWrapped = true;
  self.WebSocket = wrapped;
})();
