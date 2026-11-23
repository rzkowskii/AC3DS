# AssaultCube release evidence

The original Linux tarball and macOS disk image were inspected to identify the source baseline and shipped behavior. The stock macOS client was later run in an isolated profile for a Venison rendering comparison.

| Artifact | SHA-256 |
| --- | --- |
| Linux Lockdown Edition RC1 tar.bz2 | `9b3118e7929253f0114fa06cad8310faab646d177b4660406011152f34086b55` |
| macOS Lockdown Edition DMG | `3aba97191904d9167e9f6c8caf8c472453fb4f987acb127ee2c7d2c2f8128eba` |
| Archived Linux x86-64 client | `f8209caa52644069f056421fd2488be5ab018293773f138651c7602ba68cb228` |
| Embedded macOS game Mach-O | `93e16fc7560a7110e752bbfb27444abdd69bd6e197ce2d95c486a6cc49725950` |

The Linux archive is 52,071,427 bytes. Everything in this project that reads the archive checks its SHA-256 first, and the download and the builder also check its size:

| Where the pin is | What uses it |
| --- | --- |
| `source/3ds/firstrun_core.h` and `firstrun_core.cpp` | The [first-run download](first-run-download.md) on the console |
| `tools/builder/package.js`, `scripts/make-3dsx.py` | The [one-file builder](builder.md), in the browser and on the command line |
| `scripts/stage-assets.py` | Staging loose folders, and a source build with embedded assets |

The builder also pins the RomFS image it makes from the archive's data folders: 59,588,032 bytes, SHA-256 `91fc1f0f890ad4f3f5abe0971e07ef148845d0961e3234cfb6c78dd8d07e916d`. The `assets.romfs` the console writes holds the same 1,979 files in the order the archive delivers them, followed by an end mark, and is 59,588,112 bytes.

The Linux archive contains the original C/C++ source. The source files common to the archive and [official v1.3.0.2](https://github.com/assaultcube/AC/tree/1ece5af7533983bb8f827616381d282470793d90) match byte for byte. The archive also carries generated config templates, map-model attributes, checksums, and model shadow caches. The embedded macOS game shares all 1,761 of its package files byte for byte with the archive, including its 44 CGZ maps. The archive has 22 additional package files, including model shadow caches and the checksum list. All 47 bot files and both mod files match; the supplied config-template ZIPs differ. The Linux archive is the source and data baseline for this port.

The macOS bundle contains a thin x86-64 client at `AssaultCube.app/Contents/gamedata/assaultcube.app/Contents/MacOS/assaultcube`. It links SDL2, SDL2_image, OpenGL, OpenAL, Vorbis/Ogg and zlib. Static inspection found its configuration loader, local connection, SDL frame loop, protocol 1300 and product 1302. The archived Linux client carries the same product and protocol markers, along with the `ms.cubers.net` master address.

The archived source defaults to a negative development product version unless `PRODUCTION` is defined. The native build selects `PRODUCTION` by default to match the shipped product version. Wire protocol 1300 is independent of that switch. See [verification status](port-status.md) for runtime interoperability and rendering results.
