// Phase 2 gate runner: execute the embedder under node, pre-create
// fontconfig's cache dir in MEMFS, and extract /out.ppm to build/ after exit.
// CommonJS on purpose — Emscripten's non-modularized output breaks under ESM
// (the package.json "type":"module" lesson from Phase 1).
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const jsPath = path.join(root, 'build/webcore/bin/embedder.js');
const outPath = path.join(root, 'build/out.ppm');

global.Module = {
    preRun: [() => {
        // fontconfig's compiled-in cache dir; MEMFS starts without /var.
        global.Module.FS.mkdirTree('/var/cache/fontconfig');
    }],
    onExit: (code) => {
        try {
            const data = global.Module.FS.readFile('/out.ppm');
            fs.writeFileSync(outPath, Buffer.from(data));
            console.log(`RUNNER: wrote ${outPath} (${data.length} bytes)`);
        } catch (e) {
            console.log(`RUNNER: no /out.ppm (${e.message})`);
        }
        console.log(`RUNNER: exit code ${code}`);
    },
};

require(jsPath);
