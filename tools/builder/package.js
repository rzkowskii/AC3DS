// Turns the official AssaultCube archive and the engine-only assaultcube.3dsx
// into one assaultcube.3dsx with the game data attached as RomFS. This is the
// same procedure as scripts/make-3dsx.py and must produce the same bytes;
// scripts/qa/check-make-3dsx.py compares the two.
(function (root) {
    "use strict";

    const lib = typeof require === "function" && typeof module === "object"
        ? Object.assign({}, require("./bzip2.js"), require("./sha256.js"))
        : root.AC3DS;

    const RELEASE_NAME = "AssaultCube_v1.3.0.2_LockdownEdition_RC1.tar.bz2";
    const RELEASE_SIZE = 52071427;
    const RELEASE_SHA256 = "9b3118e7929253f0114fa06cad8310faab646d177b4660406011152f34086b55";
    const DOWNLOAD_PAGE = "https://assault.cubers.net/download.html";
    // Extraction rules shared with scripts/stage-assets.py.
    const ROOTS = new Set(["config", "packages", "bot", "mods", "demos", "docs", "screenshots"]);
    const REQUIRED = [
        "config/font.cfg", "config/configtemplates.zip", "config/mapmodelattributes.cfg",
        "packages/misc/notexture.jpg", "packages/misc/icon.png", "docs/license.html",
    ];
    // The RomFS image built from the release is the same for every engine build.
    const ROMFS_SIZE = 59588032;
    const ROMFS_SHA256 = "91fc1f0f890ad4f3f5abe0971e07ef148845d0961e3234cfb6c78dd8d07e916d";

    const EXT_HEADER_SIZE = 44;   // base header plus smdhOffset, smdhSize, fsOffset
    const FS_OFFSET_FIELD = 0x28;
    const ENGINE_SIGNATURE = "/3ds/assaultcube";
    const ROMFS_NONE = 0xffffffff;

    class BuildError extends Error {}

    const align4 = (value) => (value + 3) & ~3;
    const grouped = (value) => String(value).replace(/\B(?=(\d{3})+$)/g, ",");

    // Streaming tar reader. Calls onMember(name, kind, size) for each member,
    // kind being "file", "dir" or "other"; a Uint8Array returned for a file is
    // filled with its contents.
    class TarReader {
        constructor(onMember) {
            this.onMember = onMember;
            this.header = new Uint8Array(512);
            this.headerFill = 0;
            this.target = null;
            this.fileLeft = 0;
            this.skip = 0;
            this.ended = false;
            this.decoder = new TextDecoder("utf-8", { fatal: true });
        }

        push(chunk) {
            let position = 0;
            while (position < chunk.length && !this.ended) {
                const available = chunk.length - position;
                if (this.fileLeft > 0) {
                    const take = Math.min(this.fileLeft, available);
                    if (this.target) {
                        this.target.set(chunk.subarray(position, position + take), this.target.length - this.fileLeft);
                    }
                    this.fileLeft -= take;
                    position += take;
                } else if (this.skip > 0) {
                    const take = Math.min(this.skip, available);
                    this.skip -= take;
                    position += take;
                } else {
                    const take = Math.min(512 - this.headerFill, available);
                    this.header.set(chunk.subarray(position, position + take), this.headerFill);
                    this.headerFill += take;
                    position += take;
                    if (this.headerFill === 512) {
                        this.headerFill = 0;
                        this.readHeader();
                    }
                }
            }
        }

        finish() {
            if (!this.ended && (this.headerFill || this.fileLeft || this.skip)) {
                throw new BuildError("The archive ends too early.");
            }
        }

        text(start, length) {
            let end = start;
            while (end < start + length && this.header[end] !== 0) end++;
            try {
                return this.decoder.decode(this.header.subarray(start, end));
            } catch (error) {
                throw new BuildError("The archive has a file name that is not valid UTF-8.");
            }
        }

        number(start, length) {
            if (this.header[start] & 0x80) throw new BuildError("The archive uses an unsupported tar number format.");
            const digits = this.text(start, length).trim();
            if (!/^[0-7]*$/.test(digits)) throw new BuildError("The archive is damaged (tar header).");
            return digits ? parseInt(digits, 8) : 0;
        }

        readHeader() {
            const header = this.header;
            if (header.every((byte) => byte === 0)) {
                this.ended = true;
                return;
            }
            let sum = 0;
            for (let i = 0; i < 512; i++) sum += i >= 148 && i < 156 ? 0x20 : header[i];
            if (sum !== this.number(148, 8)) throw new BuildError("The archive is damaged (tar header).");
            let name = this.text(0, 100);
            if (this.text(257, 5) === "ustar") {
                const prefix = this.text(345, 155);
                if (prefix) name = prefix + "/" + name;
            }
            const size = this.number(124, 12);
            const type = String.fromCharCode(header[156]);
            let kind = "other";
            if (type === "5" || (type === "\0" && name.endsWith("/"))) kind = "dir";
            else if (type === "0" || type === "\0" || type === "7") kind = "file";
            else if (!"12346".includes(type)) throw new BuildError("The archive uses an unsupported tar feature.");
            const stored = kind === "file" ? size : 0;
            const target = this.onMember(name, kind, stored);
            this.target = target || null;
            this.fileLeft = stored;
            this.skip = align512(stored) - stored;
        }
    }

    const align512 = (value) => Math.ceil(value / 512) * 512;

    // Applies the extraction rules and collects the game data as nested Maps:
    // name -> Uint8Array for a file, Map for a folder.
    class ReleaseTree {
        constructor() {
            this.tree = new Map();
        }

        add(name, kind, size) {
            const parts = name.split("/").filter((part) => part !== "" && part !== ".");
            if (name.startsWith("/") || parts.includes("..")) throw new BuildError("Unsafe archive member: " + name);
            if (!parts.length || !ROOTS.has(parts[0])) return null;
            if (kind === "other") throw new BuildError("Unexpected release member type: " + name);
            let folder = this.tree;
            for (const part of parts.slice(0, -1)) {
                if (!folder.has(part)) folder.set(part, new Map());
                folder = folder.get(part);
                if (!(folder instanceof Map)) throw new BuildError("Conflicting archive member: " + name);
            }
            const last = parts[parts.length - 1];
            if (kind === "dir") {
                if (!folder.has(last)) folder.set(last, new Map());
                if (!(folder.get(last) instanceof Map)) throw new BuildError("Conflicting archive member: " + name);
                return null;
            }
            if (folder.has(last)) throw new BuildError("Duplicate archive member: " + name);
            const data = new Uint8Array(size);
            folder.set(last, data);
            return data;
        }

        lookup(path) {
            let node = this.tree;
            for (const part of path.split("/")) {
                if (!(node instanceof Map) || !node.has(part)) return null;
                node = node.get(part);
            }
            return node;
        }
    }

    function tableLength(count) {
        if (count < 3) return 3;
        if (count < 19) return count | 1;
        while ([2, 3, 5, 7, 11, 13, 17].some((prime) => count % prime === 0)) count++;
        return count;
    }

    function nameHash(parent, name, total) {
        let value = (parent ^ 123456789) >>> 0;
        for (let i = 0; i < name.length; i++) {
            value = ((value >>> 5) | (value << 27)) >>> 0;
            value = (value ^ name.charCodeAt(i)) >>> 0;
        }
        return value % total;
    }

    // Lays the tree out as the RomFS image 3dsxtool writes and libctru mounts.
    // Entries are visited in sorted name order (UTF-16 code units), files and
    // folders together, and names starting with a dot are left out, as
    // 3dsxtool does. Returns the image size and a function that writes it.
    function layoutRomfs(tree) {
        const rootEntry = { offset: 0, parent: null, sibling: null, firstDir: null, firstFile: null, name: "" };
        rootEntry.parent = rootEntry;
        const dirs = [rootEntry];
        const files = [];
        let dirSize = 0x18;
        let fileSize = 0;
        let dataSize = 0;

        function scan(entry, node) {
            for (const name of [...node.keys()].sort()) {
                if (name.startsWith(".")) continue;
                const child = node.get(name);
                if (child instanceof Map) {
                    const item = { offset: dirSize, parent: entry, sibling: entry.firstDir, firstDir: null, firstFile: null, name };
                    dirSize = align4(dirSize + 0x18 + name.length * 2);
                    entry.firstDir = item;
                    dirs.push(item);
                    scan(item, child);
                } else {
                    const item = { offset: fileSize, parent: entry, sibling: entry.firstFile, name, dataOffset: dataSize, data: child };
                    fileSize = align4(fileSize + 0x20 + name.length * 2);
                    dataSize = align4(dataSize + child.length);
                    entry.firstFile = item;
                    files.push(item);
                }
            }
        }
        scan(rootEntry, tree);

        function chain(items) {
            const table = new Uint32Array(tableLength(items.length)).fill(ROMFS_NONE);
            for (const item of items) {
                const slot = nameHash(item.parent.offset, item.name, table.length);
                item.nextHash = table[slot];
                table[slot] = item.offset;
            }
            return table;
        }
        const dirTable = chain(dirs);
        const fileTable = chain(files);
        const offsetOf = (item) => (item ? item.offset : ROMFS_NONE);

        const header = [0x28];
        let total = 0x28;
        for (const size of [dirTable.length * 4, dirSize, fileTable.length * 4, fileSize]) {
            header.push(total, size);
            total += size;
        }
        header.push(total);
        const dataStart = total;
        total += dataSize;

        function write(out, start) {
            const view = new DataView(out.buffer, out.byteOffset, out.byteLength);
            let position = start;
            const word = (value) => {
                view.setUint32(position, value, true);
                position += 4;
            };
            const text = (value) => {
                for (let i = 0; i < value.length; i++, position += 2) view.setUint16(position, value.charCodeAt(i), true);
            };
            header.forEach(word);
            dirTable.forEach(word);
            const dirStart = position;
            for (const item of dirs) {
                position = dirStart + item.offset;
                [offsetOf(item.parent), offsetOf(item.sibling), offsetOf(item.firstDir), offsetOf(item.firstFile),
                    item.nextHash, item.name.length * 2].forEach(word);
                text(item.name);
            }
            position = dirStart + dirSize;
            fileTable.forEach(word);
            const fileStart = position;
            for (const item of files) {
                position = fileStart + item.offset;
                [offsetOf(item.parent), offsetOf(item.sibling), item.dataOffset, 0, item.data.length, 0,
                    item.nextHash, item.name.length * 2].forEach(word);
                text(item.name);
            }
            for (const item of files) out.set(item.data, start + dataStart + item.dataOffset);
        }
        return { size: total, write };
    }

    // Validates the engine 3DSX and returns the offset the RomFS image goes at.
    function checkEngine(engine) {
        const view = new DataView(engine.buffer, engine.byteOffset, engine.byteLength);
        if (engine.length < 32 || view.getUint32(0, true) !== 0x58534433) {
            throw new BuildError("The engine file is not a 3DSX program. Use assaultcube.3dsx from the AC-3DS release.");
        }
        const headerSize = view.getUint16(4, true);
        if (headerSize < EXT_HEADER_SIZE || engine.length < EXT_HEADER_SIZE) {
            throw new BuildError("The engine 3DSX has no extended header, so game data cannot be attached to it. Use assaultcube.3dsx from the AC-3DS release.");
        }
        const smdhOffset = view.getUint32(0x20, true);
        const smdhSize = view.getUint32(0x24, true);
        if (view.getUint32(FS_OFFSET_FIELD, true) !== 0) {
            throw new BuildError("This assaultcube.3dsx already has game data inside. " +
                "Use the small engine-only assaultcube.3dsx from the AC-3DS release.");
        }
        const end = smdhOffset + smdhSize;
        if (smdhOffset < headerSize || end > engine.length || engine.length > align4(end)) {
            throw new BuildError("The engine 3DSX is damaged or incomplete. Download the AC-3DS release again.");
        }
        if (!contains(engine, ENGINE_SIGNATURE)) {
            throw new BuildError("The engine file is a 3DSX program, but not AC-3DS. " +
                "Use assaultcube.3dsx from the AC-3DS release.");
        }
        return align4(end);
    }

    function contains(bytes, ascii) {
        const first = ascii.charCodeAt(0);
        for (let at = bytes.indexOf(first); at !== -1; at = bytes.indexOf(first, at + 1)) {
            let matched = 1;
            while (matched < ascii.length && bytes[at + matched] === ascii.charCodeAt(matched)) matched++;
            if (matched === ascii.length) return true;
        }
        return false;
    }

    // archive and engine are Uint8Arrays. options.onProgress(step, fraction)
    // reports progress; options.pause, when given, is awaited between chunks
    // so the page stays responsive if this runs outside a worker.
    async function build(archive, engine, options) {
        const settings = options || {};
        const progress = settings.onProgress || (() => {});
        const hash = (data) => lib.sha256Hex(data, { plain: settings.plainSha256 });

        const fsOffset = checkEngine(engine);
        if (archive.length !== RELEASE_SIZE) {
            throw new BuildError("The game archive is " + grouped(archive.length) + " bytes; " + RELEASE_NAME + " is " +
                grouped(RELEASE_SIZE) + " bytes. Download it again from " + DOWNLOAD_PAGE);
        }
        progress("verify", 0);
        if (await hash(archive) !== RELEASE_SHA256) {
            throw new BuildError("The game archive is not the official " + RELEASE_NAME + " (SHA-256 differs). " +
                "Download it again from " + DOWNLOAD_PAGE);
        }

        const release = new ReleaseTree();
        const tar = new TarReader((name, kind, size) => release.add(name, kind, size));
        const decoder = new lib.Bzip2Decoder(archive);
        try {
            for (let chunk = decoder.next(); chunk; chunk = decoder.next()) {
                tar.push(chunk);
                progress("unpack", decoder.consumed / archive.length);
                if (settings.pause) await settings.pause();
            }
        } catch (error) {
            if (error instanceof lib.Bzip2Error) throw new BuildError(error.message);
            throw error;
        }
        tar.finish();
        const missing = REQUIRED.filter((path) => !(release.lookup(path) instanceof Uint8Array)).sort();
        if (missing.length) throw new BuildError("Release is missing: " + missing.join(", "));

        progress("pack", 0);
        const romfs = layoutRomfs(release.tree);
        const out = new Uint8Array(fsOffset + romfs.size);
        out.set(engine);
        new DataView(out.buffer).setUint32(FS_OFFSET_FIELD, fsOffset, true);
        romfs.write(out, fsOffset);
        if (settings.pause) await settings.pause();
        progress("check", 0);
        if (romfs.size !== ROMFS_SIZE || await hash(out.subarray(fsOffset)) !== ROMFS_SHA256) {
            throw new BuildError("The game data image did not come out as expected.");
        }
        progress("done", 1);
        return out;
    }

    const api = {
        build, checkEngine, layoutRomfs, BuildError, TarReader, ReleaseTree,
        RELEASE_NAME, RELEASE_SIZE, RELEASE_SHA256, DOWNLOAD_PAGE, ROMFS_SIZE, ROMFS_SHA256,
    };
    root.AC3DS = Object.assign(root.AC3DS || {}, api);
    if (typeof module === "object" && module.exports) module.exports = api;
})(typeof self !== "undefined" ? self : globalThis);
