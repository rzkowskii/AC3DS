// Runs the build off the page's main thread. Receives the two files, posts
// progress, and hands the finished file back without copying it.
"use strict";

importScripts("bzip2.js", "sha256.js", "package.js");

self.onmessage = async (event) => {
    try {
        const archive = new Uint8Array(await event.data.archive.arrayBuffer());
        const engine = new Uint8Array(await event.data.engine.arrayBuffer());
        const out = await self.AC3DS.build(archive, engine, {
            onProgress: (step, fraction) => self.postMessage({ type: "progress", step, fraction }),
        });
        self.postMessage({ type: "done", buffer: out.buffer }, [out.buffer]);
    } catch (error) {
        const expected = error instanceof self.AC3DS.BuildError;
        self.postMessage({ type: "error", message: expected ? error.message : "Unexpected error: " + error });
    }
};
