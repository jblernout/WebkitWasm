// Companion worker script for web/probe/worker.html (test 2): served by the
// dev server, loaded through the engine's curl/wisp network stack.
onmessage = function (e) {
  postMessage("pong:" + e.data);
};
