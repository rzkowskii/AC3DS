// Page behaviour: collects the two input files, runs the build in a worker
// (or on the page itself where workers cannot load, as when the page is
// opened straight from disk in some browsers), and offers the result to save.
(function () {
    "use strict";

    const core = window.AC3DS;
    const ENGINE_URL = "assaultcube.3dsx";
    const OUTPUT_NAME = "assaultcube.3dsx";
    const ENGINE_LIMIT = 32 * 1024 * 1024;   // an engine-only build is a few MB
    const STEPS = {
        verify: ["Checking the game archive…", 0, 4],
        unpack: ["Unpacking the game files…", 4, 90],
        pack: ["Packing the game data…", 90, 94],
        check: ["Checking the result…", 94, 99],
        done: ["Done.", 100, 100],
    };

    const $ = (id) => document.getElementById(id);
    const ui = {
        picker: $("picker"), archiveStatus: $("archive-status"), engineStatus: $("engine-status"),
        engineHelp: $("engine-help"), inputMessage: $("input-message"), build: $("build"),
        progress: $("progress"), buildStatus: $("build-status"), buildMessage: $("build-message"),
        saveWaiting: $("save-waiting"), saveReady: $("save-ready"), save: $("save"), saveSize: $("save-size"),
    };
    const state = { archive: null, engine: null, busy: false, resultUrl: null };

    const megabytes = (bytes) => (bytes / 1e6).toFixed(1) + " MB";
    const grouped = (value) => String(value).replace(/\B(?=(\d{3})+$)/g, ",");

    function setStatus(element, text, kind) {
        element.textContent = text;
        element.className = kind;
    }

    function showMessage(element, text) {
        element.textContent = text || "";
        element.hidden = !text;
    }

    function refresh() {
        ui.build.disabled = state.busy || !state.archive || !state.engine;
        ui.picker.disabled = state.busy;
    }

    async function head(file, length) {
        return new Uint8Array(await file.slice(0, length).arrayBuffer());
    }

    const startsWith = (bytes, text) => text.split("").every((char, i) => bytes[i] === char.charCodeAt(0));

    // English short title from the SMDH inside a 3DSX, for display only.
    function engineTitle(bytes) {
        const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        const start = view.getUint32(0x20, true) + 8 + 0x200;
        let title = "";
        for (let i = 0; i < 0x40 && start + i * 2 + 2 <= bytes.length; i++) {
            const unit = view.getUint16(start + i * 2, true);
            if (!unit) break;
            title += String.fromCharCode(unit);
        }
        return title || "AC-3DS";
    }

    // Returns null when the engine is usable, else the reason it is not.
    async function useEngine(blob, origin) {
        const start = await head(blob, 44);
        if (!startsWith(start, "3DSX")) {
            return "That is not a 3DSX program. Use assaultcube.3dsx from the AC-3DS release.";
        }
        let bytes = start;
        if (blob.size <= ENGINE_LIMIT) bytes = new Uint8Array(await blob.arrayBuffer());
        try {
            core.checkEngine(bytes);
        } catch (error) {
            if (!(error instanceof core.BuildError)) throw error;
            return error.message;
        }
        if (blob.size > ENGINE_LIMIT) return "That 3DSX is too large to be the engine-only assaultcube.3dsx from the AC-3DS release.";
        state.engine = blob;
        setStatus(ui.engineStatus, engineTitle(bytes) + " (" + megabytes(blob.size) + "), " + origin + ".", "ready");
        ui.engineHelp.hidden = true;
        return null;
    }

    async function useArchive(file) {
        const name = file.name || "That file";
        const lower = name.toLowerCase();
        const start = await head(file, 4);
        const wanted = "This page needs the Linux package, " + core.RELEASE_NAME +
            ", which works on every system. Get it from " + core.DOWNLOAD_PAGE;
        if (lower.endsWith(".exe") || startsWith(start, "MZ")) {
            return name + " is the Windows installer. " + wanted;
        }
        if (lower.endsWith(".dmg")) return name + " is the Mac disk image. " + wanted;
        if (lower.endsWith(".zip") || startsWith(start, "PK")) {
            if (lower.startsWith("ac-3ds") || lower.startsWith("ac3ds")) {
                return name + " is the AC-3DS release zip. Unzip it and drop the assaultcube.3dsx inside it.";
            }
            return name + " is a zip. " + wanted;
        }
        if (file.size !== core.RELEASE_SIZE) {
            if (startsWith(start, "BZh") && lower.includes("assaultcube") && !lower.includes("1.3.0.2")) {
                return name + " is a different version of AssaultCube. Only 1.3.0.2 works here: " + core.RELEASE_NAME +
                    ". Get it from " + core.DOWNLOAD_PAGE;
            }
            if (startsWith(start, "BZh") && file.size < core.RELEASE_SIZE && lower.includes("assaultcube")) {
                return name + " is incomplete: " + grouped(file.size) + " of " + grouped(core.RELEASE_SIZE) +
                    " bytes. Download it again from " + core.DOWNLOAD_PAGE;
            }
            if (lower.endsWith(".tar")) {
                return name + " has been unpacked from its .bz2 wrapper (some browsers do this). " +
                    "Download " + core.RELEASE_NAME + " again and keep it as it is.";
            }
            return name + " is not the file this page needs. The right one is " + core.RELEASE_NAME + ", " +
                grouped(core.RELEASE_SIZE) + " bytes. Get it from " + core.DOWNLOAD_PAGE;
        }
        state.archive = file;
        setStatus(ui.archiveStatus, name + " (" + megabytes(file.size) + "). It is checked fully when you build.", "ready");
        return null;
    }

    async function addFiles(files) {
        if (state.busy || !files.length) return;
        const problems = [];
        for (const file of files) {
            try {
                const start = await head(file, 4);
                const isEngine = startsWith(start, "3DSX") || (file.name || "").toLowerCase().endsWith(".3dsx");
                const problem = isEngine ? await useEngine(file, "from the file you chose") : await useArchive(file);
                if (problem) problems.push(problem);
            } catch (error) {
                problems.push("Could not read " + (file.name || "that item") + ". Drop the file itself, not a folder.");
            }
        }
        // A result built from the previous inputs is no longer what Build would make.
        if (state.resultUrl) {
            URL.revokeObjectURL(state.resultUrl);
            state.resultUrl = null;
            ui.save.removeAttribute("href");
        }
        ui.saveReady.hidden = true;
        ui.saveWaiting.hidden = false;
        showMessage(ui.inputMessage, problems.join(" "));
        showMessage(ui.buildMessage, "");
        refresh();
        if (!problems.length && state.archive && state.engine) ui.build.focus();
    }

    async function findEngineBesidePage() {
        try {
            if (location.protocol === "file:") throw new Error("pages opened from disk cannot fetch");
            const response = await fetch(ENGINE_URL, { cache: "no-cache" });
            if (!response.ok) throw new Error("not found");
            if (await useEngine(await response.blob(), "found beside this page") === null) {
                refresh();
                return;
            }
        } catch (error) {
            // Not hosted with the page, or the page was opened from disk.
        }
        if (!state.engine) {
            setStatus(ui.engineStatus, "Not chosen yet.", "pending");
            ui.engineHelp.hidden = false;
        }
    }

    function showProgress(step, fraction) {
        const [label, from, to] = STEPS[step];
        const percent = Math.round(from + (to - from) * fraction);
        ui.progress.value = percent;
        ui.buildStatus.textContent = step === "unpack" ? label + " " + percent + "%" : label;
    }

    function finish(buffer) {
        if (state.resultUrl) URL.revokeObjectURL(state.resultUrl);
        state.resultUrl = URL.createObjectURL(new Blob([buffer], { type: "application/octet-stream" }));
        ui.save.href = state.resultUrl;
        ui.save.download = OUTPUT_NAME;
        ui.saveSize.textContent = "(" + megabytes(buffer.byteLength) + ")";
        ui.saveWaiting.hidden = true;
        ui.saveReady.hidden = false;
        showProgress("done", 1);
        ui.buildStatus.textContent = "Done. Save the file in step 4, then copy it to the SD card.";
        state.busy = false;
        refresh();
        ui.save.focus();
    }

    function fail(message) {
        ui.progress.hidden = true;
        ui.buildStatus.textContent = "";
        if (/allocation failed|out of memory|invalid array length/i.test(message)) {
            message = "This device ran out of memory. Close other tabs, or use a computer.";
        }
        showMessage(ui.buildMessage, "Nothing was saved. " + message);
        state.busy = false;
        refresh();
    }

    // Fallback for browsers that will not start a worker for this page.
    async function buildOnPage() {
        try {
            const archive = new Uint8Array(await state.archive.arrayBuffer());
            const engine = new Uint8Array(await state.engine.arrayBuffer());
            const out = await core.build(archive, engine, {
                onProgress: showProgress,
                pause: () => new Promise((resolve) => setTimeout(resolve, 0)),
            });
            finish(out.buffer);
        } catch (error) {
            fail(error instanceof core.BuildError ? error.message : "Unexpected error: " + error);
        }
    }

    function startBuild() {
        if (state.busy || !state.archive || !state.engine) return;
        state.busy = true;
        refresh();
        showMessage(ui.buildMessage, "");
        ui.saveReady.hidden = true;
        ui.saveWaiting.hidden = false;
        ui.progress.hidden = false;
        ui.progress.value = 0;
        ui.buildStatus.textContent = "Reading the files…";

        let worker = null;
        let started = false;
        try {
            worker = new Worker("worker.js");
        } catch (error) {
            buildOnPage();
            return;
        }
        worker.onmessage = (event) => {
            started = true;
            const message = event.data;
            if (message.type === "progress") {
                showProgress(message.step, message.fraction);
                return;
            }
            worker.terminate();
            if (message.type === "done") finish(message.buffer);
            else fail(message.message);
        };
        worker.onerror = (event) => {
            event.preventDefault();
            worker.terminate();
            if (started) fail("The build stopped unexpectedly. This device may be out of memory.");
            else buildOnPage();
        };
        worker.postMessage({ archive: state.archive, engine: state.engine });
    }

    ui.picker.addEventListener("change", () => {
        const files = Array.from(ui.picker.files);
        ui.picker.value = "";
        addFiles(files);
    });
    ui.build.addEventListener("click", startBuild);

    let dragDepth = 0;
    const hasFiles = (event) => event.dataTransfer && Array.from(event.dataTransfer.types || []).includes("Files");
    window.addEventListener("dragenter", (event) => {
        if (!hasFiles(event)) return;
        event.preventDefault();
        dragDepth++;
        document.body.classList.add("dragging");
    });
    window.addEventListener("dragover", (event) => {
        if (hasFiles(event)) event.preventDefault();
    });
    window.addEventListener("dragleave", (event) => {
        if (!hasFiles(event)) return;
        dragDepth = Math.max(0, dragDepth - 1);
        if (!dragDepth) document.body.classList.remove("dragging");
    });
    window.addEventListener("drop", (event) => {
        if (!hasFiles(event)) return;
        event.preventDefault();
        dragDepth = 0;
        document.body.classList.remove("dragging");
        addFiles(Array.from(event.dataTransfer.files));
    });

    refresh();
    findEngineBesidePage();
})();
