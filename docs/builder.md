# One-file builder

The release is the engine only. The builder joins it with the official AssaultCube download on the player's own computer and produces one file, `assaultcube.3dsx`, with the game data inside. That file goes to `/3ds/assaultcube/assaultcube.3dsx` on the SD card. It contains AssaultCube's media, so it is for the player's own console and must not be shared.

There are two front ends with the same result, byte for byte:

- `tools/builder/`: a web page that does the work in the browser tab. Nothing is installed and nothing is uploaded.
- `scripts/make-3dsx.py`: a command-line script that needs only Python 3.

Neither needs devkitPro.

## Inputs

| Input | What it must be |
| --- | --- |
| Game archive | `AssaultCube_v1.3.0.2_LockdownEdition_RC1.tar.bz2`, 52,071,427 bytes, SHA-256 `9b3118e7929253f0114fa06cad8310faab646d177b4660406011152f34086b55`. The Windows installer and the Mac disk image are refused with a message naming this file. |
| Engine | The engine-only `assaultcube.3dsx` from an AC-3DS release (`-DAC3DS_EMBED_ASSETS=OFF`). A file that already has game data, or another program's 3DSX, is refused. |

## Command line

```sh
python3 scripts/make-3dsx.py AssaultCube_v1.3.0.2_LockdownEdition_RC1.tar.bz2 assaultcube.3dsx out/assaultcube.3dsx
```

`--stage-dir DIR` also writes the game files as loose folders, the same tree `stage-assets.py` writes when it does not convert JPEGs.

## Web page

Try it locally:

```sh
cp build-release/assaultcube.3dsx tools/builder/    # optional: the engine, so users need only one file
python3 -m http.server 8000 --bind 127.0.0.1 --directory tools/builder
```

Then open <http://localhost:8000/>. `*.3dsx` is ignored by Git, so the copied engine is not committed.

The page is plain files: `index.html`, `style.css`, `app.js` (the page), `worker.js` (runs the build off the main thread), `package.js` (archive rules, RomFS writer, 3DSX patch), `bzip2.js` and `sha256.js`. There is no build step, framework or outside dependency. `build-node.js` runs the same code under Node for the tests.

- If `assaultcube.3dsx` sits beside `index.html`, the page uses it. Otherwise it asks the user to drop the engine as well and links to the releases page.
- The page's Content-Security-Policy allows only its own files, so it cannot contact another site.
- Opened straight from disk (`file://`), the page still works where the browser allows it. Chrome will not start a worker for a local file, so the build then runs on the page itself in small steps; the engine must be dropped by hand because a local page cannot fetch the file beside it.
- SHA-256 uses WebCrypto when the browser offers it (https, localhost, local files) and a plain implementation otherwise.

### Hosting on GitHub Pages

The page can be served from any static host. For GitHub Pages, publish a folder that contains the files of `tools/builder/` (not `build-node.js`) plus the engine-only `assaultcube.3dsx` of the current release. The engine has to be on the same site: browsers will not let the page fetch it from the releases download URL. Do not publish a `.3dsx` that has game data in it. Update the engine on the site with each release.

## What the builder does

1. Checks the engine: 3DSX magic, extended header (`headerSize` 44), `fsOffset` 0, SMDH inside the file, and the AC-3DS data path string.
2. Checks the archive's size and SHA-256.
3. Unpacks it in memory with the rules of `stage-assets.py`: only `config`, `packages`, `bot`, `mods`, `demos`, `docs` and `screenshots`; absolute paths and `..` are refused; only regular files and folders are accepted; the six required files must be present.
4. Writes a RomFS image of those files and appends it to the engine at the next 4-byte boundary after the SMDH.
5. Stores that offset in the header word at 0x28, where libctru's `romfsMountSelf` reads it.
6. Checks the image against a pinned size and SHA-256 (59,588,032 bytes, `91fc1f0f890ad4f3f5abe0971e07ef148845d0961e3234cfb6c78dd8d07e916d`). The archive is pinned, so the image is the same for every engine build. If the layout code is ever changed on purpose, update `ROMFS_SIZE` and `ROMFS_SHA256` in both `make-3dsx.py` and `package.js`.

Differences from a source build with embedded assets:

- Progressive JPEGs are not converted to baseline, so textures load as they do with loose files staged without `jpegtran`. The pixels are the same.
- The launcher icon and title are the engine release's own.
- Directory entries are laid out in sorted name order. `3dsxtool` uses whatever order the host file system lists them in. libctru finds entries through the hash tables and sibling links, so the order does not matter to it.
- Names starting with a dot are left out of the image, as `3dsxtool` does. That includes the `.assaultcube-assets` marker, which only the staging script reads.

A `.cia` or `.3ds` cannot be made this way. Their data sits in a hashed container that has to be built with `makerom` from the engine ELF.

## Tests

```sh
python3 scripts/qa/check-make-3dsx.py ARCHIVE ENGINE.3dsx \
  --3dsxtool "$DEVKITPRO/tools/bin/3dsxtool" --elf build-release/assaultcube.elf --smdh build-release/assaultcube.smdh \
  --node node
```

- Reads every file and folder back out of the image the way libctru does and compares it with the archive.
- With `--3dsxtool`: packs the same files with devkitPro's tool and compares its RomFS region byte for byte with ours laid out in the same directory order.
- With `--node`: compares the JavaScript builder's output with the Python one, with both SHA-256 paths, and tests the bzip2 decoder against Python's on awkward inputs.
- Feeds both builders a wrong file, a cut-off archive, an archive with one changed bit, an engine that already has RomFS, a non-3DSX file and another program's 3DSX, and checks that nothing is written.

These checks run on the host. Booting the built file is a separate test, and an Azahar boot is not a handheld result.
