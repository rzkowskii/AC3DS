# Known issues

Build 26 (`AC-3DS0v1.14`). This page lists what I know is wrong or not yet checked, and says for each one whether it was seen on a real console or only in the Azahar emulator. The console is a New 2DS XL. The history behind each item is in [Development status](port-status.md).

## The weapon in hand ghosts or flickers while moving (console only)

On the real console the weapon you are holding can ghost, flicker or look see-through for a moment while you move and turn. It is most noticeable with the sniper rifle. It does not show in the emulator, which is why it is taking a while.

What has been done so far:

- **Build 14** took the weapon out of the per-pixel fog stage, gave it its own slice of the depth range, and made a finished frame wait until the previous one has reached the screen.
- **Build 22** changed how the weapon is positioned after a shot. The kick used to follow a direction that was one frame old, so the weapon jumped between frames while the view was turning; with the sniper that lasted 1.5 seconds per shot. It now follows the current view direction. In the emulator the largest jump between two frames fell from 0.128 to 0.017 game units.
- Build 22 also added a **Diagnostics** page to the pause menu (START in a match): a frame recorder and switches for each of the changes above, so the fault can be narrowed down on the console itself. None of the switches is saved; every launch starts from the defaults.

It is not fixed yet. Whether the build 22 change addresses what I saw on the console is not confirmed. I'm working on it.

If you see it and want to help: open **Diagnostics**, choose **Record weapon frames**, and keep moving for the three-second countdown and the short recording that follows. The game writes `gunburst_N.bin` to `/3ds/assaultcube/profile/`. `python3 scripts/decode-gunburst.py gunburst_0.bin` turns it into a picture of the recorded frames. That recording, the `clientlog.txt` beside it, and the weapon you were holding are what I need in an issue.

## Rare freeze: the GPU queue stops advancing (console only)

Twice the game has frozen on the console with the GPU idle: once during online play on build 14 and once on the menu on build 22. In both, the wait for the previous frame's GPU commands never returned. The emulator has never shown it.

Build 23 puts a limit on that wait. After two seconds with the GPU idle the game clears the queue, drops that one frame and carries on, instead of hanging. Each recovery writes an `AC3DS_QUEUE` line to `/3ds/assaultcube/profile/clientlog.txt`.

The cause is not understood yet. The recovery cannot be triggered in the emulator, so it has not been exercised there. If the game stalls for about two seconds and then continues, that is this, and the log line is worth sending.

## First-run download: build 26 has not been timed on a console

The download itself is proven on the console: build 24 fetched the official package on a New 2DS XL, verified it, unpacked it and ran the game from it. That took about 31 minutes (1,881 seconds), nearly all of it spent creating 1,979 small files on the SD card.

Build 26 writes one file instead, and decodes on a second core. In the emulator the whole installation takes 74 seconds, but the emulator's SD card is nearly free where the console's is the main cost, so that figure says nothing about a console. I expect it to be much quicker than build 24 and have not measured it. Mounting the one file and loading maps from it are likewise checked in the emulator only.

Also not tried during the offer or the transfer: HOME, the power button, a full SD card, a console whose clock is far off. Closing the lid does not stop the transfer by design, but that has not been run on a console either. Details are in [First-run download](first-run-download.md#test-status).

## One-file builder

- The built file has booted and loaded maps in the emulator. No built file has run on a console yet. It attaches the same data in the same format as a source build with embedded assets, which does run on the console.
- The web page has not been tested in Safari.
- Textures are not converted to baseline JPEG by the builder or by the first-run download, so maps load somewhat slower than from a source build or from folders staged with `jpegtran` installed. The pixels are the same.
- Copying a release's small `assaultcube.3dsx` over a built one removes the game data from it. Build again with the new release.

## No `.cia`

There is no installable `.cia` and no `.3ds` image, and the builder cannot make one. A package of that kind would contain AssaultCube's media in a modified form, which its licence does not let me distribute, and its container has to be built with separate tools. The game starts from the Homebrew Launcher as a `.3dsx`.

## Other things to know

- **Original 3DS, 3DS XL and 2DS are not supported.** The port needs the New models' memory and CPU.
- **No sound without `/3ds/dspfirm.cdc`.** The game still runs, silently. See [Audio](audio-3ds.md).
- **See-through walls.** Build 11 sometimes showed players and sky through walls on the console. Build 12 addressed three possible causes without isolating one, so it is not confirmed gone.
- **Very long sessions.** Free memory falls by about 0.3 MB per map change on average in the emulator. A 30-map bot rotation completes; a much longer one is expected to cost frame rate on the largest maps, not crash.
- **Custom maps online.** Content a server's custom map needs is fetched for at most 30 seconds during the map change, so the server does not drop the console. Whatever is still missing is fetched the next time that map loads.
- **Hosting a server on the console** has been checked in the emulator only.
- **Emulator frame rates are not console frame rates.** Azahar does not model GPU time. Console figures in the documentation are marked as such.

Found something else? Open an issue at <https://github.com/rzkowskii/AC3DS/issues> with `/3ds/assaultcube/profile/clientlog.txt` and, if there is one, `crash.txt` or `stall.txt` from the same folder.
