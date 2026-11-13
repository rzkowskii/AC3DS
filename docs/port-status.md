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

The 39.749 FPS build passed the original Lockdown Edition desktop server retest at 100% CPU clock. It entered `ac_douze` revision 40 on protocol 1300, exchanged chat and disconnected. See `build-azahar/network-native-ui-100`.

The profiler reports submission intervals and VBlank callbacks observed between frame starts. Azahar uses separate CPU and display-event timing, so its callback count is not an exact count of displayed or repeated frames. The emulator's nominal refresh is about 59.83 Hz. Azahar logs that it does not implement the New 3DS CPU boost service. The 300% setting was an emulator comparison. Timing on the target handheld is unmeasured.

The initial map rotation ran out of GPU-accessible linear memory by the third map. The revised renderer retires world textures, skies and map model resources between maps and keeps cached model records for reloading. The native path uses a 48 MiB linear heap, caps most mipmapped textures at 256 pixels per axis, and packs opaque world, sky and map model textures as RGB565. This preserves logical dimensions and can reduce fine detail and color precision compared with desktop. The audio loader transfers short decoded clips directly to the mixer and stores long clips in lossless blocks with a bounded decoded cache.

Desktop arbitrary window sizes and multisample settings have no direct equivalents on the 400×240 target. The desktop server wizard and OS service tools stay external. Native hosting uses launch arguments. Water reflection and refraction use two projected passes when both effects are enabled. Mono spatialization uses the native mixer's attenuation and panning. The New 3DS uses the same monoscopic view as the New 2DS XL.
