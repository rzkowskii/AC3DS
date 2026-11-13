#!/usr/bin/env python3
"""Stage one AssaultCube QA scenario on an isolated Azahar virtual SD."""
import argparse
import json
import pathlib
import re
import tarfile

QA = pathlib.Path(__file__).resolve().parent / "qa"
CASES = ("smoke", "performance", "venison", "maps", "modes", "edit", "modes-edit", "network", "dedicated", "demo", "hid", "audio")
MODE_IDS = list(range(22))
BOT_MODE_IDS = {7, 8, 12, 18, 19, 20, 21}
BOT_TEAM_MODE_IDS = {7, 20, 21}
FLAG_MODE_IDS = {5, 13, 14, 15}
EDIT_MARKERS = ["new_map", "edit_height", "edit_undo", "edit_redo", "edit_xmap", "reload_map"]


def quote_path(path):
    if any(c in path for c in '\r\n"[]'):
        raise ValueError("Unsupported characters in CubeScript path")
    return f'"{path}"'


def map_names(archive):
    with tarfile.open(archive, "r:bz2") as release:
        return sorted({
            pathlib.PurePosixPath(entry.name).stem
            for entry in release
            if entry.isfile() and
            re.fullmatch(r"(?:\./)?packages/maps/official/[\w-]+\.cgz", entry.name)
        })


def map_case(names):
    commands = ['echo AC3DS_QA BEGIN maps', 'sleep 1000 [closecurmenu; qa_map0] 1']
    for i, name in enumerate(names):
        commands.append(f'''tempalias qa_map{i} [
    tempalias qa_result (loadmap {name})
    echo AC3DS_QA MAP {name} (curmap) $qa_result
    if (&& (strcmp (curmap) {name}) (= $qa_result 0)) [
        echo AC3DS_QA PASS map_{name} OK
    ] [ echo AC3DS_QA FAIL map_{name} ]
    sleep 200 qa_map{i + 1} 1
]''')
    commands.append(f'tempalias qa_map{len(names)} [echo AC3DS_QA END maps; quit]')
    return '\n'.join(commands)


def mode_case(modes, edit=False):
    commands = ['echo AC3DS_QA BEGIN modes', 'tempalias qa_pending -1',
                'sleep 1500 [closecurmenu; qa_mode0] 1']
    for i, mode in enumerate(modes):
        # Flag modes use a stock map with flag bases and team spawns.
        name = "ac_shine" if mode in FLAG_MODE_IDS or mode == 0 else "ac_desert"
        bots = ""
        if mode in BOT_TEAM_MODE_IDS:
            bots = "addnbot 1 CLA good; addnbot 1 RVSF good"
        elif mode in BOT_MODE_IDS:
            bots = "addnbot 2 random good"
        commands.append(f'''tempalias qa_mode{i} [
    qa_pending = {i}
    mapstartonce = [
        sleep 1000 [
            if (= $qa_pending {i}) [
                if (&& (= $gamemode {mode}) (strcmp (curmap) {name})) [
                    {bots}
                    echo AC3DS_QA PASS mode_{mode} OK (curmap) $gamemode (player1 health) (curweapon)
                ] [ echo AC3DS_QA FAIL mode_{mode} (curmap) $gamemode ]
                qa_pending = -1
                sleep 200 qa_mode{i + 1} 1
            ]
        ] 1
    ]
    mode {mode}
    map {name}
    sleep 25000 [
        if (= $qa_pending {i}) [
            echo AC3DS_QA FAIL mode_{mode} timeout (curmap) $gamemode
            mapstartonce = []
            qa_pending = -1
            sleep 200 qa_mode{i + 1} 1
        ]
    ] 1
]''')
    final = 'echo AC3DS_QA END modes; quit'
    if edit:
        # The final stock mode can be a bot mode. Enter coop edit before newmap.
        final = '''echo AC3DS_QA END modes
    mapstartonce = [
        if (&& (= $gamemode 1) (strcmp (curmap) ac_desert)) [
            exec config/qa-edit.cfg
        ] [ echo AC3DS_QA FAIL editor_mode; quit ]
    ]
    mode 1
    map ac_desert
    sleep 25000 [ echo AC3DS_QA TIMEOUT modes-edit; quit ] 1'''
    commands.append(f'tempalias qa_mode{len(modes)} [{final}]')
    return '\n'.join(commands)


def stage(sd, scenario, archive=None, demo=None, maps=(), modes=()):
    root = sd / "3ds/assaultcube"
    launch = root / "server.args"
    profile = root / "profile"
    if launch.exists() or profile.exists():
        raise ValueError(f"Expected an isolated SD without {launch} or {profile}")
    common = (QA / "common.cfg").read_text()
    args = ["--home", "--init", "--loadmap=ac_desert", "-m127.0.0.1"]
    expected = []
    if scenario == "maps":
        if not archive:
            raise ValueError("The maps scenario needs --archive")
        names = map_names(archive)
        if not names:
            raise ValueError("No official maps found in the archive")
        if maps:
            missing = set(maps) - set(names)
            if missing:
                raise ValueError(f"Unknown official maps: {', '.join(sorted(missing))}")
            names = list(dict.fromkeys(maps))
        fixture = map_case(names)
        expected = [f"map_{name}" for name in names]
    elif scenario in ("modes", "modes-edit"):
        modes = list(dict.fromkeys(modes)) if modes else MODE_IDS
        fixture = mode_case(modes, edit=scenario == "modes-edit")
        expected = [f"mode_{mode}" for mode in modes]
        if scenario == "modes-edit":
            expected.extend(EDIT_MARKERS)
    elif scenario == "dedicated":
        args = ["--home", "-d", "-i127.0.0.1", "-f28785", "-c4", "-nAssaultCube 3DS QA dedicated",
                "-Aconfig/qa-serverparameters.cfg"]
        fixture = ""
    else:
        fixture = (QA / f"{scenario}.cfg").read_text()
        expected = {"smoke": ["startup", "bot_map"], "edit": EDIT_MARKERS,
                    "network": ["desktop_join", "desktop_disconnect"],
                    "demo": ["demo_start", "demo_stop"],
                    "audio": ["audio_kazemat_load", "audio_kazemat_playback"],
                    "hid": ["hid_movement", "hid_fire", "hid_reload"]}.get(scenario, [])
    if scenario == "demo":
        if not demo or not demo.is_file():
            raise ValueError("The demo scenario needs --demo pointing to a recorded .dmo")
        args.append("--loaddemo=qa_demo")
    if scenario == "venison":
        args[2] = "--loadmap=ac_venison"
    if scenario not in ("dedicated", "hid"):
        args.append("-eexec " + quote_path("config/qa.cfg"))
    (profile / "config").mkdir(parents=True)
    (profile / "config/init.cfg").write_text("autodownload 0\n")
    (profile / "config/pcksources.cfg").write_text("// QA profile has no package servers.\n")
    (profile / "config/servers.cfg").write_text("// QA profile has no saved servers.\n")
    (profile / "config/autostart").mkdir()
    (profile / "config/autostart/_aftersaved_qa.cfg").write_text("showmenuonstartup = 0\n")
    # This runs before initial localconnect and prevents the first-run map menu.
    (profile / "config/autoexec.cfg").write_text(common + "\n" + fixture if scenario == "hid" else common)
    if scenario not in ("dedicated", "hid"):
        (profile / "config/qa.cfg").write_text(common + "\n" + fixture + "\n")
    if scenario == "dedicated":
        (profile / "config/qa-serverparameters.cfg").write_text("mandatory_auth:0\n")
    if scenario == "modes-edit":
        edit_fixture = (QA / "edit.cfg").read_text()
        edit_fixture = edit_fixture.replace("echo AC3DS_QA END edit; quit",
                                            "echo AC3DS_QA END edit; echo AC3DS_QA END modes-edit; quit")
        (profile / "config/qa-edit.cfg").write_text(edit_fixture)
    if scenario == "demo":
        (profile / "demos").mkdir()
        (profile / "demos/qa_demo.dmo").write_bytes(demo.read_bytes())
    with launch.open("x") as launch_file:
        launch_file.write("\n".join(args) + "\n")
    required_files = {
        "smoke": ["screenshots/qa_smoke.png"],
        "demo": ["screenshots/qa_demo.png"],
        "performance": ["screenshots/qa_water.png"],
        "venison": ["screenshots/qa_venison_water.png"],
    }.get(scenario, [])
    if scenario in ("edit", "modes-edit"):
        required_files = ["screenshots/qa_editor.png", "packages/maps/qa_editor.cgz"]
    required_log_text = [f"AC3DS_QA PASS {name}" + (" OK" if name.startswith(("map_", "mode_")) else "")
                         for name in expected]
    if scenario == "performance":
        required_log_text.extend(f"AC3DS_PERF map={map_name} mode={mode} intervals=600"
                                 for map_name, mode in (("ac_desert", 0), ("ac_desert", 7), ("ac_venison", 8)))
    if scenario == "venison":
        required_log_text.extend([
            "AC3DS_QA SCENE water_bots ac_venison 8 1 1",
            "AC3DS_PHASE reflection calls=600",
            "AC3DS_PHASE refraction calls=600",
            "AC3DS_PERF map=ac_venison mode=8 intervals=600",
        ])
    if scenario == "network":
        required_log_text.append("AC3DS_QA_LOOPBACK")
    if scenario == "hid":
        required_files.append("screenshots/qa_hid_ready.png")
        required_log_text.extend(["AC3DS_QA HID_READY ac_desert",
                                  "AC3DS_QA HID_DOWN 3DS_TOUCH_SCREENSHOT",
                                  "AC3DS_QA HID_UP 3DS_TOUCH_SCREENSHOT"])
    if scenario == "modes-edit":
        required_log_text.extend(["AC3DS_QA END modes", "AC3DS_QA END edit"])
    if scenario == "dedicated":
        required_files.append("logs/readmaps_log.txt")
    elif scenario != "venison":
        required_log_text.append(f"AC3DS_QA END {scenario}")
    return {
        "scenario": scenario,
        "launch": str(launch),
        "profile": str(profile),
        "client_log": str(profile / "clientlog.txt"),
        "expected_pass_markers": expected,
        "required_log_text": required_log_text,
        "required_guest_files": required_files,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", choices=CASES)
    parser.add_argument("--sd", type=pathlib.Path, required=True, help="Isolated emulator virtual SD")
    parser.add_argument("--archive", type=pathlib.Path, help="Release tar.bz2 for all-stock-map sweep")
    parser.add_argument("--map", action="append", default=[], help="Select an official map for a smaller maps run")
    parser.add_argument("--mode", action="append", type=int, choices=MODE_IDS, default=[],
                        help="Select a game mode for a smaller modes run")
    parser.add_argument("--demo", type=pathlib.Path, help="Genuine v1.3.0.2 recording for demo playback")
    args = parser.parse_args()
    if args.map and args.scenario != "maps":
        parser.error("--map requires the maps scenario")
    if args.mode and args.scenario not in ("modes", "modes-edit"):
        parser.error("--mode requires modes or modes-edit")
    try:
        output = stage(args.sd.absolute(), args.scenario, args.archive, args.demo, args.map, args.mode)
    except (ValueError, OSError, tarfile.TarError) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(output, indent=2))


if __name__ == "__main__":
    main()
