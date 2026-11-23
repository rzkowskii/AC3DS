# Network play and SD storage

The 3DS client uses the Lockdown Edition v1.3.0.2 gameplay and network code with a libctru socket adapter for the bundled ENet 1.3.6. The AssaultCube wire protocol remains 1300. The original server browser, favourites, LAN discovery, direct IPv4 or hostname connections, master list, authentication, votes, administration, coop editing and HTTP media downloads use the same engine entry points as the desktop game. Offline play can start if the 3DS socket service fails to initialize.

Base assets are read from the 3DSX RomFS when present. Otherwise they come from `sdmc:/3ds/assaultcube/assets.romfs`, one RomFS image file that the game mounts as `romfs:` in place of the built-in one, or, without that file, from loose `config/`, `packages/` and `bot/` directories of the release in `sdmc:/3ds/assaultcube/`. An engine-only build that finds none of the three offers to download the official package and write the image file itself ([first-run download](first-run-download.md)). The image is read-only and is only ever replaced whole; deleting it removes the game data and nothing else. In every layout, the default writable profile is `sdmc:/3ds/assaultcube/profile/`. The game looks there for overrides before falling back to base assets. The directory is created as needed.

| Relative to `sdmc:/3ds/assaultcube/profile/` | Contents |
| --- | --- |
| `config/` | Saved settings and bindings, initialization settings, console history, servers and favourites, package sources, server configuration and optional user scripts. |
| `private/` | Local entropy file and private authentication material. Keep this directory with the profile when migrating an installation. |
| `packages/` | Custom maps, uploaded maps and downloaded media. Put dedicated server maps in `packages/maps/servermaps/`. The server puts player uploads under `packages/maps/servermaps/incoming/`. |
| `mods/` | Optional ZIP mod packages. |
| `demos/`, `screenshots/` | Demo files, screenshots and map shots. |
| `logs/` | Dedicated server logs by default. The client uses `clientlog.txt` in the profile root. |

Native initialization and saved settings retain the original rotated backups. The port stages each replacement on SD and flushes its data before renaming it into place. If an interrupted replacement leaves the target missing, reads can fall back to its `.ac-prev` copy. Incoming map files use the same staged write. A downloaded media file that is not on the card yet is written beside its target and renamed when complete, without the flush: nothing is replaced, and each extra SD operation is felt on a map change. A multi-file upload or media archive is written one file at a time.

## Hosting a dedicated server

Create `sdmc:/3ds/assaultcube/server.args` and launch the regular 3DS application. Put one argument per line. Blank lines and lines that do not start with `-` are ignored. Keep spaces in values such as the server name and omit shell quotes.

```text
-d
-nAssaultCube on 3DS
-f28763
```

With this example, other clients can connect to the handheld’s IPv4 address on UDP port 28763 or use LAN discovery. The server status screen shows the exit controls. Remove the `-d` line or the file to launch the client again. Add `-x` followed by your chosen admin password on the same line to configure admin access. Normal loader arguments follow the file arguments, so they can override its settings. The embedded server used by bots and demos starts automatically in client mode.

The desktop server switches still apply, including `-C` for an additional server options file. Master registration needs an explicit `-m` master address, the server key via `-Y`, and a client limit of at most 16. Public hosting also needs the game and query UDP ports reachable from other clients. The default game port is 28763, the default server query port is 28764, and LAN discovery uses UDP 28762. The desktop interactive server wizard remains a desktop tool. Its shell or batch launcher is replaced by `server.args` on the handheld.

An Azahar guest joined the original Lockdown Edition dedicated server over loopback, loaded the host's map, exchanged chat and disconnected. The guest also played, rewound and stopped a demo produced by the original server. In a separate run, the native dedicated server opened its game and query ports and answered an original-format browser query over loopback. A desktop client joining the guest server, LAN discovery, public master and media services, authenticated accounts, and SD recovery still need coverage. The inherited media downloader uses HTTP. To move a desktop key encrypted with the executable-dependent protection option, unlock it in its original build and save it again with an even `authmemusage` value before copying it to the 3DS profile.
