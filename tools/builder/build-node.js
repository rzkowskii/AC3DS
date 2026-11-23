#!/usr/bin/env node
// Runs the page's builder from a terminal, for testing it against
// scripts/make-3dsx.py:
//   node build-node.js ARCHIVE ENGINE.3dsx OUTPUT.3dsx [--no-webcrypto]
"use strict";

const fs = require("fs");
const { build, BuildError } = require("./package.js");
const { Bzip2Decoder, Bzip2Error } = require("./bzip2.js");

// node build-node.js --bunzip2 IN OUT: decoder only, for the decoder tests.
if (process.argv[2] === "--bunzip2") {
    const decoder = new Bzip2Decoder(new Uint8Array(fs.readFileSync(process.argv[3])));
    const parts = [];
    try {
        for (let chunk = decoder.next(); chunk; chunk = decoder.next()) parts.push(Buffer.from(chunk));
    } catch (error) {
        if (!(error instanceof Bzip2Error)) throw error;
        console.error("build-node: " + error.message);
        process.exit(1);
    }
    fs.writeFileSync(process.argv[4], Buffer.concat(parts));
    process.exit(0);
}

const args = process.argv.slice(2).filter((arg) => !arg.startsWith("--"));
const plainSha256 = process.argv.includes("--no-webcrypto");
if (args.length !== 3) {
    console.error("usage: node build-node.js ARCHIVE ENGINE.3dsx OUTPUT.3dsx [--no-webcrypto]");
    process.exit(64);
}

(async () => {
    try {
        const archive = new Uint8Array(fs.readFileSync(args[0]));
        const engine = new Uint8Array(fs.readFileSync(args[1]));
        const out = await build(archive, engine, { plainSha256 });
        fs.writeFileSync(args[2], out);
        console.log("build-node: wrote " + args[2] + " (" + out.length + " bytes)");
    } catch (error) {
        if (!(error instanceof BuildError)) throw error;
        console.error("build-node: " + error.message);
        process.exit(1);
    }
})();
