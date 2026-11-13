# AssaultCube emulator QA

Use Azahar in New 3DS mode and keep its emulated CPU clock at 100% for baseline measurements. Record the emulator build, graphics backend, CPU clock, 3DSX and ELF digests, and DSP firmware digest with each result. Measurements at another CPU clock can help diagnose a bottleneck. Frame submission timing and missed VBlanks in Azahar measure the emulator guest. Measure sustained delivery on the target handheld separately.

## Run a fixture

Provide the original Lockdown Edition v1.3.0.2 release archive and the packaged 3DSX. Set `azahar` to the emulator executable, `binary` to the 3DSX, `elf` to its corresponding ELF and `dsp` to the DSP firmware for audio checks. This example runs three maps from a fresh profile. Omit `--map` to sweep all 44 official maps or select individual modes with `--mode`.

```sh
qa_stage=$(mktemp -d /tmp/ac3ds-qa.XXXXXX)
python3 scripts/stage-emulator-qa.py maps --sd "$qa_stage" \
  --archive "$archive" --map ac_desert --map ac_shine --map ac_venison \
  > "$qa_stage/fixture.json"

qa_game="$qa_stage/3ds/assaultcube"
python3 scripts/run-azahar.py --emulator "$azahar" --binary "$binary" \
  --elf "$elf" --dsp "$dsp" --output build-azahar/maps-01 \
  --profile "$qa_game/profile" --guest-args "$qa_game/server.args" \
  --timeout 180 --cpu-clock 100 --renderer vulkan \
  --require 'AC3DS_QA PASS map_ac_desert OK' \
  --require 'AC3DS_QA PASS map_ac_shine OK' \
  --require 'AC3DS_QA PASS map_ac_venison OK' \
  --require 'AC3DS_QA END maps' --reject 'AC3DS_QA FAIL' \
  --reject 'AC3DS_QA TIMEOUT'
```

Use the `required_log_text` and `required_guest_files` in `fixture.json` as the runner’s `--require` and `--expect` values. For the combined mode and editor check, stage `modes-edit`. Its fixture requires independent mode and editor completion markers and both saved files. Use `--reject 'AC3DS_QA FAIL' --reject 'AC3DS_QA TIMEOUT'` for these fixtures. Keep the `result.json` receipt, `process.log`, guest `clientlog.txt`, and any captures or output files. The `END` marker is written before the guest quits and flushes its log. The profile turns off public package discovery before startup, clears the saved server list, disables LAN browsing and directs master lookups to loopback. Each runner invocation uses its own profile and owns its emulator cleanup.

To collect every failure during a long map sweep, require only `AC3DS_QA END maps` and omit the runner’s `--reject` options. Read each `AC3DS_QA MAP` line in the final guest log and compare the set of passed maps to the manifest. The runner’s completion status records that the sweep finished. Each map result records the observed name and engine warning flags. Use smaller `--map` batches if the full sweep exceeds the runner’s maximum timeout.

For a desktop interoperability check, build the dedicated server from the original v1.3.0.2 source. Start it in a scratch copy of the release's `config` and `packages/maps` directories. Set `mandatory_auth:0` in the private server parameter file. Set `demo_save:1` and create `demos/` if you plan to use its recording. Run the following in a separate terminal and stop it with Ctrl-C when the emulator finishes. The command limits the server to 600 seconds.

```sh
python3 -c 'import subprocess, sys; subprocess.run(sys.argv[1:], timeout=600)' \
  "$desktop_server" -i127.0.0.1 -f28783 '-nAssaultCube QA loopback' \
  -Aconfig/qa-serverparameters.cfg -Wdemos/
```

Stage `network` for this endpoint. Check that the desktop server received `AC3DS_QA_LOOPBACK` and logged the guest leaving. A recorded `.dmo` from the original server can seed a separate `demo` run.

```sh
qa_demo=$(mktemp -d /tmp/ac3ds-qa-demo.XXXXXX)
python3 scripts/stage-emulator-qa.py demo --sd "$qa_demo" --demo "$recording"
```

## Checks and coverage

| Fixture | Evidence | Additional coverage needed |
| --- | --- | --- |
| `smoke` | Startup on `ac_desert`, switch to bot deathmatch, six bot connection lines, native rendering counters, PNG screenshot and minimap shot | Compare screenshot and minimap against the desktop scene. Exercise the in-game menus and handheld controls. |
| `maps` | One explicit result per stock map. Pass requires an exact map name match and no engine load warning. | Traverse each map and compare textures, skins, collision, lighting, sky, fog, visibility, water and item placement with desktop. |
| `modes` | Mode and map change for each of 22 modes. Bot modes request two local bots. Flag modes use `ac_shine`. | Complete rounds and verify respawn, scoring, team changes, pickups, weapon restrictions, flag interactions, spectator modes and bot combat. |
| `edit` | Create a 128×128 map, change floor height, verify undo and redo depth, store an xmap, write and reopen a map, capture the editor | Compare map content after reload, exercise other editor tools, clipboard and preferences, and test synchronized editing with a second peer. |
| `modes-edit` | Run the mode and editor checks in the same process | Same coverage as `modes` and `edit`. |
| `network` | Join the original desktop dedicated server at `127.0.0.1:28783`, exchange a chat message, then disconnect. Confirm the matching log lines on the host. | Test multiple peers, LAN browsing, authentication, admin and votes, server discovery, custom media and map transfers. |
| `dedicated` | Start the native guest dedicated server, inspect its stock map scan log and query its loopback server browser port for a protocol-1300 response | Connect an original desktop game client to the guest to verify reverse ENet session, gameplay and disconnect. |
| `demo` | Play, seek and stop an original recorded `.dmo`, capture gameplay during playback. Stage with `--demo` and keep the original recording with the receipt. | Verify deterministic playback, downloading a demo over the game protocol, controls and timestamps against the desktop. |
| `hid` | Circle Pad movement, L fire, X reload and a bottom-screen screenshot through Azahar’s emulated input | Check C-Stick and touch aiming, menus, remapping and software keyboard on the same build. |
| `audio` | Load `ac_kazemat`, position the listener below the stock helicopter sound and check compressed PCM decode and NDSP wave submission during a five-second dwell | Capture and listen to the guest output. Compare ambience, effects, speech, music and spatial placement. |
| `performance` | Three 600-interval samples at Good settings, covering `ac_desert`, `ac_desert` with six bots and `ac_venison` with eight bots and both water effects | Check gameplay during movement and combat, varied views and player counts, editor work and audio plus network load. Compare p99 and maximum frame intervals, intervals over 20 ms and missed VBlanks as well as mean FPS. |
| `venison` | Run the eight-bot water scene at CPU 100%, capture `qa_venison_water.png` before timing, and require 600 submissions with both projected passes running | Repeat from varied camera positions and compare reflection and refraction against desktop. Death and bot spawn order can affect frame timings between runs. |

Audio needs output evidence for effects, speech, music and spatial placement. The NDSP initialization line records service startup. Input needs control, touch, remapping and software keyboard checks. Applet, sleep and resume behavior and sustained frame delivery need measurements on the physical New 2DS/3DS.

## Query the native dedicated server

Stage `dedicated` and use the runner’s `--profile` and `--guest-args` options as above. Set `--timeout 100` and leave out `--require` and `--expect` to keep the server available during the query. When the runner prints its PID, probe from another terminal and keep the result with the runner receipts.

```sh
python3 scripts/qa/query-dedicated.py --timeout 70 \
  > build-azahar/dedicated-01/dedicated-query.json
```

The probe targets the same loopback address as the guest's explicit bind and queries UDP port 28786, one above its game port 28785. It checks the server description, client limit, protocol version and echoed request ID. Check `profile/logs/readmaps_log.txt` for the guest’s map scan. Press B or Start in the owned dedicated guest to exit, or let the runner’s timeout clean it up. If Azahar fails to expose the guest loopback socket, the bounded probe reports that it received no response.

## Drive the HID fixture on macOS

Stage `hid` and pass its profile, arguments, required markers and expected file to the runner. Add `--touch-key 69 280 178` to map the host E key to the center of the guest touchscreen’s Capture button. The fixture writes `screenshots/qa_hid_ready.png` once the local player is alive in bot deathmatch. Wait for that file inside the runner’s virtual SD before posting keys to the PID from `result.json`.

```sh
mkdir -p build-azahar
swiftc scripts/qa/azahar-keys.swift -o build-azahar/azahar-keys
build-azahar/azahar-keys "$azahar_pid" UP .7 WAIT .2 Q .6 WAIT .3 Z .2 WAIT .6 E .2
```

The helper requires macOS Accessibility access. Focus Azahar’s game view before driving it. The helper requests app activation, and Qt delivers gameplay input through the focused render widget. Verify the guest’s movement, fire, reload and touchscreen log markers and inspect the second PNG. If the key sequence fails to reach the guest, focus the game view, repeat the sequence and click the bottom-screen Capture button directly.
