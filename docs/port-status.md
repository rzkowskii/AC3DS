# Development and verification status

The native ARM 3DSX builds with devkitARM and packages 1,979 assets from the supplied Lockdown Edition release. Azahar 2126.1.2 ran the application in New 3DS mode with its Vulkan backend at native resolution. The tested build, emulator configuration, guest log and observed files are retained per run in `build-azahar/`. See [emulator QA](emulator-qa.md) for reproduction and coverage.

| Surface | Observed in Azahar | Remaining |
| --- | --- | --- |
| Combat and bots | All 22 stock modes loaded with the expected mode and map. Six local bots joined Bot Deathmatch on ac_desert, navigated and fought. Original waypoints loaded. | Complete rounds, team and flag scoring, spectator behavior and every weapon/item interaction. |
| Stock maps | All 44 official maps attempted in one rotation, with 43 warning-free loads and no texture allocation errors. Kazemat was the exception and passed a focused retest after the audio fix. | Run all 44 maps on one build with packed ambience, then traverse and compare them with desktop. |
| Controls and menus | Circle Pad movement changed the live player's coordinates. L triggered shots, A reached the game, and X triggered reload. Both screens rendered. | Touch actions and aiming, C-Stick, menu navigation, remapping and persistence, and software keyboard behavior. |
| Rendering and output | Textured ac_desert gameplay and HUD, minimap, 400×240 PNG screenshot and mapshot. The loader reported the Citro3D/PICA200 renderer. | Compare matching scenes with desktop, including fog, lighting, shadows, reflection/refraction, transparency, model animation and editor pick precision. |
| Audio | DSP firmware initialized NDSP and the client loaded map sounds. On Kazemat the packed PCM decode counter increased from 490 to 491 and NDSP wave submissions from 2,322 to 2,324 during the local ambience check. | Capture sound output, music, spatial placement, latency and applet continuity. |
| Network | The port joined the original desktop server through host loopback using protocol 1300, entered ac_douze revision 40, exchanged chat and disconnected. The native dedicated server bound both UDP ports and answered a protocol 1300 browser query. | Desktop client joining a native host, LAN discovery, authentication, multiple peers, administration, coop editing, public services and media transfers. |
| Demos | The guest played, rewound and stopped the original desktop server's recording. It wrote a screenshot during replay. | In-game demo downloads and playback behavior across varied recordings, timestamps and controls. |
| Editor | Created a 128×128 map, changed floor height, verified undo and redo, stored an xmap, and saved and reopened the CGZ. A separate parse verified all 16 edited floor cells in the saved map. | Other edit tools, selection precision, clipboard, xmap restore, and synchronized editing with a second peer. |
| Storage and packaging | The RomFS package booted from 3DSX. The guest wrote screenshots and profile config files and quit. | SD-only assets, existing profiles, ZIP mods, backup and interrupted write recovery, and handheld lifecycle. |

## Frame timing

These samples use six bots in ac_desert at 400×240. Each records 600 guest frame submission intervals. The first two runs used the same build. The third used texture-combiner and shader-uniform deduplication, faster common vertex packing and the precomputed model index ranges. Bot names and spawn sequences varied across launches.

| Build and Azahar CPU setting | Mean FPS | 99th percentile | Maximum | Intervals over 20 ms |
| --- | ---: | ---: | ---: | ---: |
| Initial, 100% | 40.045 | 32.666 ms | 149.272 ms | 600 |
| Initial, 300% | 59.440 | 17.866 ms | 101.497 ms | 2 |
| Optimized, 100% | 59.839 | 19.962 ms | 25.337 ms | 5 |

A separate 100% run used the release's Good graphics preset. Empty ac_desert averaged 59.924 FPS with a 17.147 ms p99 and a 17.159 ms maximum. The six-bot team match averaged 51.195 FPS with a 25.477 ms p99, a 281.828 ms maximum and 214 of 600 intervals over 20 ms. A later run produced a lower team-match mean but Azahar opened its software keyboard during the run, so that sample is excluded from the comparison.

The final build used the release's Good graphics preset for these samples, with both water effects explicitly enabled on ac_venison. Both used Azahar's 100% CPU setting and 600 submission intervals.

| Final build scene | Mean FPS | 99th percentile | Maximum | Intervals over 20 ms |
| --- | ---: | ---: | ---: | ---: |
| ac_desert, six team bots | 59.823 | 20.883 ms | 34.183 ms | 17 |
| ac_venison, eight bots and both water effects | 14.017 | 105.418 ms | 224.013 ms | 600 |

Individual ac_venison frames sampled before and after mesh batching recorded 7,399 and 358 draws respectively, at different points in combat. The later scene included player death and the scoreboard.

### Venison at stock emulator speed

The focused fixture runs Bot Deathmatch at 400×240 with eight Good bots, the Good graphics preset and 128×128 reflection and refraction targets. It starts at the same position and angle for each run. Bot seeds and combat vary between launches. Player death can switch to the death camera during sampling.

| Build | Mean FPS | 99th percentile | Maximum | Intervals over 20 ms |
| --- | ---: | ---: | ---: | ---: |
| Reproduced baseline | 14.175 | 98.369 ms | 291.399 ms | 600 |
| Direct PICA world batches and current-view water bounds | 19.155 | 66.611 ms | 269.216 ms | 600 |
| Height-aware projected culling and reusable ray occlusion | 23.456 | 51.194 ms | 54.841 ms | 600 |
| Reusable world batches and direct water strips | 37.003 | 45.684 ms | 53.582 ms | 600 |
| Additional texture and map-model state reuse | 33.071 | 49.052 ms | 165.466 ms | 579 |
| Persistent PICA world vertex and index buffers | 38.970 | 43.769 ms | 46.037 ms | 290 |
| Native HUD glyph batches | 39.749 | 42.957 ms | 44.851 ms | 270 |
| Tighter projected bounds and shared map-model occlusion | 43.541 | 39.940 ms | 68.914 ms | 540 |
| Shared model preparation and audio mixer worker | 46.990 | 38.541 ms | 170.213 ms | 192 |
| Shared main and refracted world geometry with late frame pacing | 53.766 | 39.563 ms | 44.115 ms | 84 |
| Shared geometry, compact HUD and world surface replay support | 59.114 | 26.734 ms | 48.714 ms | 15 |
| Additional reflection selection reuse and compact baked models | 59.373 | 21.779 ms | 40.865 ms | 13 |
| Shared reflection and refraction with native clip and link-time optimization | 59.898 | 16.758 ms | 16.758 ms | 0 |
| Repeat optimized sample | 59.843 | 16.758 ms | 19.851 ms | 0 |
| Default settings before the top-screen change | 57.267 | 28.766 ms | 33.517 ms | 31 |
| Default settings after the top-screen change | 59.431 | 25.762 ms | 37.613 ms | 11 |

In the 43.541 FPS run, 600 reflections averaged 4.234 ms each and 600 refractions averaged 1.411 ms. Projected static-world drawing averaged 1.828 ms across the two views, and projected models averaged 3.554 ms. The final water surface averaged 0.229 ms. The complete draw averaged 11.813 ms, game update 3.269 ms, and audio 6.680 ms. A 60 FPS frame has about 16.7 ms available. The 33.071 FPS run included a local death and a camera change. Receipts are under `build-azahar/venison-reproduce`, `venison-native-01` through `venison-native-03`, `venison-final-100`, `venison-gpu-100`, `venison-native-ui-100`, and `venison-water-bounds-100`.

Venison has 529 base cells eligible to seed water in five disconnected groups and 475 map-model entities. A rectangle around all eligible cells spans 30,408 cells. The engine derives its actual rectangle from the current visible world and the tightened projected scissor still covered 80.69% of the 128×128 reflection target in the focused view. Each effect has its own scene visibility. The reflection mirrors the camera across the water and the refraction clips geometry at the water plane.

The native path feeds PICA200 interleaved world and water vertices, reuses world batches for a stationary view with static lighting, and clips projected geometry conservatively. Moving views and dynamic lighting still rebuild batches when needed. An experimental VRAM cache copies static water-view color and depth, then renders models and particles against the restored depth. It skipped the static draw in 772 of 1,200 projected passes in `venison-vram-100`. That run averaged 37.065 FPS and included a camera change. The experiment defaults off pending controlled performance and image comparisons. The Good settings, original lighting and both water effects were used throughout. Pixel-level comparison with desktop is still pending.

Map models, pickups and players share a frame's scene preparation while each water view applies its own projection, clipping and depth. When the refracted view selects the same world surfaces as the main view, it reuses the native world batch and its GPU data. The reflection can replay the main view's ordered horizontal visibility and LOD selection and uses its own floor and ceiling rules. The 59.898 FPS sample retained 80.69% projected target coverage, ran both water effects on all 600 frames and missed zero VBlanks. It had no world-batch lighting misses or camera transition. A high-coverage combat run with 152 lighting misses per view averaged 46.963 FPS. Another run with shared reflection selection, a death-camera transition and a first-use HUD spike averaged 52.323 FPS. Receipts are under `build-azahar/shared-scene-*-100`.

The shipped macOS v1.3.0.2 client was captured at the same Venison camera pose, with matching map data, Good settings, eight bots and both water effects. The basin, grate, rail, walls and projection agree in the sampled views. Bot positions and water phase differed. The desktop captures and log are in `build-azahar/desktop-venison-reference/`.

The native refraction reuse comparison in `build-azahar/refraction-ab-100/comparison.json` found identical RGB at all 96,000 pixels with the option on, off and restored in a frozen frame. The fixture recorded its initial shared-frame count and five screenshots. Its runner was interrupted after capture because the final guest log markers were buffered.

A second controlled Azahar comparison held the camera and water phase fixed and toggled scene sharing and joined water commands together. The result differed at 7,392 of 96,000 pixels, with a mean absolute channel difference of 0.089 on an 8-bit scale. Differences were confined to the water region. See `build-azahar/water-parity-final-auto-100/comparison.json`.

One New 2DS XL launch reached renderer initialization and stopped before the menus initialized. The top-screen update uses the standard Citro3D target binding and restores the conservative GPU command and pacing defaults. Azahar verified the title, both-screen menus, map previews and gameplay in `build-azahar/hardware-display-menu-100`. The same build joined the original Lockdown Edition desktop server, exchanged chat and disconnected in `build-azahar/network-hardware-display-retry-100`. Its follow-up with startup logging ran both water passes for all 600 Venison intervals in `build-azahar/hardware-startup-log-venison-100`.

A handheld retest confirmed the initial clear frame completed and the splash texture loaded. The GPU queue timed out after the first textured draw, before sound and menu initialization. The updated vertex shader writes each configured output once, after any conditional calculations. Startup now checks an untextured draw before the splash so that another timeout can distinguish vertex processing from the textured path. This build passed the dual-screen menu and gameplay fixture in `build-azahar/hardware-shader-menu-100`. Physical verification is pending. If startup stops, the last checkpoint is in `/3ds/assaultcube/profile/clientlog.txt` on the SD card.

The next handheld log showed that the untextured draw also timed out. The renderer now uses the main-memory base for its vertex and index buffers. This keeps offsets within the PICA200's 28-bit range throughout the New model's main memory. The startup path uses indexed triangles and a straight-line shader, following the draw pattern used by Kingdom3DS, and explicitly flushes each command list before queuing it. The startup log includes the GPU status register if the queue times out again.

The indexed build also stopped at the untextured draw. Its log read `busy=00000000`, so the PICA200 was idle and the P3D completion event never reached the queue. The cause is still unconfirmed on hardware. Startup now runs a staged check: screen clear, an empty command list, a plain triangle through a position-and-color probe program with Citro3D's default buffer base, the game vertex program, the projection-texture program, then the splash. Each stage waits for the GPU queue. A stage that never completes is named on the touch screen and in `clientlog.txt` with the GX submission count, the last GSP result and the GPU busy bits. The first command lists of each launch are saved as `profile/gpu-list-N.bin`.

The renderer changes that go with it: command lists are submitted the stock Citro3D way with no extra flush wrapper, every vertex program writes each output register in full exactly once, the homogeneous texture coordinate moved to a separate projection program that is bound only for water, and texture unit one is never enabled without unit zero. If a launch stops inside the projection check, the next launch disables that program and records it in `profile/gpu-projective.txt`. Delete that file to retry. Azahar passed the smoke and Venison water fixtures on this build in `build-azahar/probe-01` and `build-azahar/probe-venison-01`. Venison averaged 53.875 FPS in that run, below the earlier 57 to 59. Physical verification is pending.

The next handheld log passed every startup check: screen clear, empty list, minimal draw, game shader draw, projection draw and the splash. It went on through sound, configuration and models to `init: mainloop`, then stopped with no further log lines. The crash is now after startup, in the first frames or the first map load. Model cache entries are now kept 8-byte aligned. Odd-sized shadow data could leave the next entry misaligned, and the ARM11 faults on multi-word and VFP stores to it while Azahar does not. Whether this was the handheld fault is unconfirmed.

The handheld's Luma3DS dumps then identified the fault for both the probe build and the alignment build. Each was a data abort with DFSR `0x801`, an alignment fault on a write. It came from a `stm` in `vertmodel::mesh::render` while `rendermapmodels` drew stencil shadows, writing the shadow-direction vector into a cache entry at an address 2 bytes off a word boundary. Rounding the allocation did not help, because the shadow-volume builder then trims each entry to its exact index data. That is 6 bytes per silhouette edge, so an odd edge count misaligns the next entry. The builder now adds one degenerate triangle when the edge count is odd, and the allocator rounds each next entry up to 8 bytes. Azahar passed the smoke fixture on this build in `build-azahar/shadowfix-02`. Handheld verification is pending.

The next handheld launch passed the stencil-shadow point and stopped in `load map` for `ac_keller`, where the watchdog reported a 20-second stall. With the handheld's own profile, Azahar spent about 22 to 26 seconds in the first map load, including 6 to 7 seconds for textures and 13 to 17 seconds for map models, before running normally. The 20-second limit was too short and its touch-screen message looked like a hang. Load stages now get 180 seconds and other phases 30. Each load stage is named in the client log and flushed to the SD card. Azahar ran the handheld profile through the load and 1,800 frames on this build in `build-azahar/keller-02`. Load time on the handheld is unmeasured.

A later handheld run reached gameplay at a steady 16 ms per frame in New 3DS memory mode (APPMEMTYPE 6, 124 MB). Its first map load took 68 seconds: 9.8 s for textures, 45 s for map models and 8.7 s for sounds. Later loads spent up to 30 s on sounds alone. Profiling `ac_keller` in Azahar with the console's DSP firmware found three causes, all now addressed without changing output:

| Stage, `ac_keller` in Azahar | Before | After |
| --- | ---: | ---: |
| `findfile` probing of the SD profile folder | 6,886 ms | 7 ms |
| Texture tiling into PICA format | 1,958 ms | 428 ms |
| Ambient sound decode (blocking) | 12,725 ms | 0 ms (background on core 2) |
| Load frame total, sound enabled | 19.0 s | 5.6 s |

- **SD probing:** every asset lookup checked the SD profile folder first, costing an FS IPC `stat` each time. The folder is now listed once into an index, and paths missing from it skip the probe.
- **Tiling:** the texture upload writes whole Morton tiles through lookup tables. A host test found byte-identical output across 576 format and size cases.
- **Sound and texture decoding:** map ambience now decodes on core 2 after the map is playable. Each loop starts once it is ready, and the PC map-sound refresh retries stale locations. World textures decode ahead on core 2 while the main thread helps. Azahar runs all cores on one host thread, so the parallel gain is unmeasured there.

The lower screen shows a loading bar for startup and map loads.

On the handheld, every TCP connect failed immediately, which blocked both the master server list and the package server, while UDP and DNS worked. AC3DS now runs a blocking `connect` on a helper thread with a timeout, and logs the SOC errno on failure. Azahar fetched the master list (44 servers) through this path. Handheld verification is pending.

The match screen on the lower display now shows a rotating radar. It is made from the map's own GPU-rendered minimap, copied to the CPU once per map by the transfer engine, and follows the PC radar's visibility rules: teammates in team modes and flags, never opponents. The radar is the stylus look area. RELOAD sits top centre between MENU and SCORES, with GRENADE, WEAPON and SCOPE along the bottom. Buttons dim when unavailable. The top screen's console and scoreboard text use `hudtextscale` (default 180%). With the radar running, the six-bot `ac_desert` sample averaged 59.835 FPS with a 19.2 ms p99 and no missed VBlanks (`build-azahar/perf-01`).

A review of that work (85 findings, 55 confirmed on an independent second pass) led to a second round of fixes. All figures are from Azahar.

**Load times**
- Progressive stock JPEGs are transcoded losslessly to baseline at staging, with pixel-identical output checked by the game's own decoder (485 files).
- The core-2 decode worker starts before the map config and also covers sky faces and cached model skins.
- Images are read with a single `fread`, and other reads use 32 KB stdio buffers.
- Startup game sounds decode in the background. A sound needed early is claimed on first use, so none is dropped.
- Results: `ac_keller` with sound loads in 3.1 s, against 19.0 s before the first round. `initsound` to first frame fell from 17.8 s to 8.2 s.
- Each load stage logs its duration. `ac_keller`'s embedded map config takes about 0.9 s, mostly the sky decode.

**Correctness**
- The lower radar uses the PC radar's visibility functions, including spectator modes and the flag-carrier marker.
- The stall watchdog pauses during HOME, sleep and the keyboard, and writes `stall.txt` instead of `crash.txt`.
- ENet is serviced during long load loops, and from a helper thread while the keyboard or HOME menu blocks the main loop, so a PC server does not time the 3DS out.
- Offline matches pause on return from HOME or sleep. Settings are saved on suspend.
- A dedicated host keeps the console awake.
- Online players get a "Match menu online" with chat, voting, team and admin menus and disconnect, and with no pause.
- During a vote, the lower screen shows VOTE YES / VOTE NO.

**Diagnostics**
- Startup logs `cpu: boost clock ratio` (about 3.0 means 804 MHz is in effect).
- The 600-frame profile adds `AC3DS_GPU` drawing and processing time from Citro3D. Both are meaningful only on hardware.

**Frame times** on this build: six-bot `ac_desert` 59.84 FPS with a 17.4 ms p99. Venison with eight bots and both water passes is unchanged at about 52.7 FPS, with draw at 14.3 ms including a 5.3 ms reflection pass. The hardware GPU and clock figures will show whether Venison is limited by the CPU or the GPU there. None of this has been verified on the handheld.

**Handheld verification, 2026-11-14 (previous checkpoint build on the New 2DS XL)**
- The master list loaded and the package server ping succeeded, confirming the blocking-connect fix on real SOC.
- The 2DS connected to a public PC 1.3.0.2 server ("Vortex | 1v1 Arena | USA [West Coast]"), authenticated, loaded `ac_urban` and played deathmatch at 16-19 ms per logged frame. Cross-play with PC works on hardware.
- First load of `ac_arctic` offline took 9.2 s, against 68 s for `ac_wasteland` before the SD-probing fix.
- The `ac_urban` load spent 11.7 s of CPU in image decode across 90 calls (about 130 ms each). That build read images through small stdio refills. `find file` took 6 ms.
- The texture prefetch worker kept ahead of the main thread (53 of 53 ready, none waited), so core 2 runs in parallel on hardware.
- The current build adds whole-file image reads, baseline JPEGs and the CPU clock ratio log, so the next handheld log shows how much of the decode time was I/O and whether the 804 MHz boost is active.

**Lower screen and server browser rework, and the hardware smear report.**

*Match screen.* The map now fills the whole lower screen and remains the stylus look area. It carries only translucent RELOAD (top centre) and GRENADE (bottom centre) buttons, plus a MAP/RADAR corner toggle that switches between the close rotating radar and a north-up view of the whole map. A corner tab slides out a drawer with MENU, SCORES, WEAPON and CHAT.

*Server browser.* The top screen shows a handheld list at the readable text size: ping, players, name and map, with a footer for the selected server. The lower screen shows a card for the selected server, a large JOIN button, repeating scroll arrows, and REFRESH, FILTER, SORT and BACK. Six-bot `ac_desert` stays at 59.8-59.9 FPS in Azahar.

*Smear report.* On the handheld, ammo crates appeared to "stretch out and repeat", with general blurring; Azahar shows neither. Checking 27 candidate causes, each twice, refuted the coherency and stencil theories. Citro3D queues mid-frame splits and starts the GPU only at `C3D_FrameEnd`, after the arena flush.

The best-supported mechanism is that AssaultCube clears the top colour buffer only outside the map. Any pixel the world leaves uncovered on PICA keeps earlier frames, and the rotating, bobbing pickups smear into stacked copies. The 3DS build now clears colour every frame (`ac3dsclearcolor`, default 1). That is one GPU fill and gives identical output where the world draws. It also flushes the arena before every mid-frame split, as hardening.

`ac3dsnear` (hundredths, default 15) is a diagnostic. If holes remain visible as fog-coloured flicker with clearing on, raising it tests whether near-plane clipping of large world polygons causes them. Handheld confirmation is pending.

To capture the next failure, the main and audio mixer threads now install a CPU exception handler. A fault writes `profile/crash.txt` with the exception type, PC, LR, fault address, registers, nearby code and stack return candidates, and shows the summary on the touch screen. A watchdog on the extra core reports a main loop that makes no progress for 20 seconds. Both reports name the last main loop phase. The client log records the first six frames, every 1,800th frame and each map load. Resolve the addresses with `scripts/symbolize-crash.py crash.txt <build>/assaultcube.elf` using the ELF from the same build. Azahar passed the smoke fixture on this build in `build-azahar/crash-01` without a stall report. Handheld verification is pending.

The 39.749 FPS build passed the original Lockdown Edition desktop server retest at 100% CPU clock. It entered `ac_douze` revision 40 on protocol 1300, exchanged chat and disconnected. See `build-azahar/network-native-ui-100`.

The profiler reports submission intervals and VBlank callbacks observed between frame starts. Azahar uses separate CPU and display-event timing, so its callback count is not an exact count of displayed or repeated frames. The emulator's nominal refresh is about 59.83 Hz. Azahar logs that it does not implement the New 3DS CPU boost service. The 300% setting was an emulator comparison. Timing on the target handheld is unmeasured.

The initial map rotation ran out of GPU-accessible linear memory by the third map. The revised renderer retires world textures, skies and map model resources between maps and keeps cached model records for reloading. The native path uses a 48 MiB linear heap, caps most mipmapped textures at 256 pixels per axis, and packs opaque world, sky and map model textures as RGB565. This preserves logical dimensions and can reduce fine detail and color precision compared with desktop. The audio loader transfers short decoded clips directly to the mixer and stores long clips in lossless blocks with a bounded decoded cache.

Desktop arbitrary window sizes and multisample settings have no direct equivalents on the 400×240 target. The desktop server wizard and OS service tools stay external. Native hosting uses launch arguments. Water reflection and refraction use two projected passes when both effects are enabled. Mono spatialization uses the native mixer's attenuation and panning. The New 3DS uses the same monoscopic view as the New 2DS XL.
