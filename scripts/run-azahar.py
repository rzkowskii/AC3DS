#!/usr/bin/env python3
"""Run an AssaultCube 3DSX in a fresh macOS Azahar profile and retain receipts."""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read_slice(path, limit, *, tail=False):
    try:
        with path.open('rb') as stream:
            if tail:
                stream.seek(max(0, os.fstat(stream.fileno()).st_size - limit))
            return stream.read(limit).decode(errors='replace')
    except FileNotFoundError:
        return ''


def observe_backend(receipt, log):
    text = read_slice(log, 128 * 1024)
    # Vulkan capability probing also runs before software rendering starts.
    # Only renderer-instance messages are evidence of an observed backend;
    # Renderer_GraphicsAPI is the requested configuration, not proof of use.
    for name, marker in (('vulkan', 'renderer_vulkan/renderer_vulkan.cpp:'),
                         ('opengl', 'renderer_opengl/renderer_opengl.cpp:'),
                         ('software', 'renderer_software/renderer_software.cpp:')):
        if marker in text:
            receipt['renderer_observed'] = name
    for line in text.splitlines():
        if 'Unknown or unsupported graphics API' in line:
            receipt['status'] = 'backend-unavailable'
            receipt['renderer_error'] = line
            return False
    return True


def processes(timeout=2):
    return subprocess.check_output(['ps', '-axo', 'pid=,args='], text=True, timeout=timeout)


def process_command(pid, deadline):
    # Observe only the owned PID. A whole-system process inventory timed out
    # during a successful menu run; it is only needed for the prelaunch check.
    for attempt in range(2):
        remaining = deadline-time.monotonic()
        if remaining <= 0:
            raise TimeoutError('Process observation deadline reached')
        try:
            result = subprocess.run(['ps', '-p', str(pid), '-o', 'args='],
                text=True, capture_output=True, check=False, timeout=min(remaining, 2))
        except subprocess.TimeoutExpired:
            if attempt == 0 and time.monotonic() < deadline:
                continue
            raise
        command = result.stdout.strip()
        if result.returncode == 1 and not command and not result.stderr.strip():
            return None  # ps reports no matching PID.
        if result.returncode != 0 or not command:
            raise RuntimeError(f'Cannot observe owned PID {pid}: ps returned {result.returncode}')
        return command


def owned(pid, emulator, binary, deadline):
    # Both paths are absolute and the binary is this run's private copy.
    # A command that merely mentions these paths is never our emulator.
    return process_command(pid, deadline) == f'{emulator} -w {binary}'


def launch(emulator, binary, env, log, state, deadline):
    # This Azahar macOS build warns for a terminal parent. Retain the PID of
    # our double-forked child so detaching never means unbounded ownership.
    # The grandchild cannot exec until the owner has stored its PID and sent
    # the acknowledgement. Closing that pipe aborts a failed launch.
    read_fd, write_fd = os.pipe()
    go_read, go_write = os.pipe()
    intermediate = os.fork()
    if intermediate == 0:
        os.close(read_fd)
        os.close(go_write)
        try:
            os.setsid()
            child = os.fork()
            if child:
                os.write(write_fd, f'PID {child}\n'.encode())
                os._exit(0)
            while os.getppid() != 1:
                if time.monotonic() >= deadline:
                    raise TimeoutError('Azahar launcher was not adopted before the deadline')
                time.sleep(.01)
            fd = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            os.dup2(fd, 1); os.dup2(fd, 2); os.close(fd)
            os.write(write_fd, b'READY\n')
            remaining = deadline-time.monotonic()
            if remaining <= 0 or not select.select([go_read], [], [], remaining)[0]:
                raise TimeoutError('Azahar launch acknowledgement timed out')
            if os.read(go_read, 1) != b'G':
                raise RuntimeError('Azahar launch owner withdrew before exec')
            os.close(go_read)
            # Python pipes are close-on-exec. EOF tells the owner that startup
            # has left this helper; an exec failure sends an explicit error.
            os.execve(str(emulator), [str(emulator), '-w', str(binary)], env)
        except BaseException as error:
            try:
                os.write(write_fd, ('ERROR '+str(error)+'\n').encode()[:2048])
            except OSError:
                pass
            os._exit(127)
    state['launcher_pid'] = intermediate
    os.close(write_fd)
    os.close(go_read)
    pending = b''
    ready = acknowledged = False
    try:
        while True:
            if state.get('signal') is not None:
                raise InterruptedError('Emulator launch interrupted')
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                raise TimeoutError('Azahar launch timed out')
            if not select.select([read_fd], [], [], min(remaining, .1))[0]:
                continue
            chunk = os.read(read_fd, 4096)
            if not chunk:
                if not acknowledged:
                    raise RuntimeError('Azahar launcher exited before acknowledgement')
                return state['pid']
            pending += chunk
            while b'\n' in pending:
                line, pending = pending.split(b'\n', 1)
                if line.startswith(b'PID '):
                    state['pid'] = int(line[4:])
                elif line == b'READY':
                    ready = True
                elif line.startswith(b'ERROR '):
                    raise RuntimeError(line[6:].decode(errors='replace'))
            if ready and state['pid'] is not None and not acknowledged:
                os.write(go_write, b'G')
                acknowledged = True
    finally:
        os.close(read_fd)
        os.close(go_write)


def reap_launcher(state, deadline):
    pid = state.get('launcher_pid')
    if pid is None:
        return True
    # An unreaped direct child cannot have its PID reused. Never signal a
    # grandchild or another process through this launcher-only path.
    try:
        if os.waitpid(pid, os.WNOHANG)[0]:
            state['launcher_pid'] = None
            return True
        os.kill(pid, signal.SIGKILL)
        while time.monotonic() < deadline:
            if os.waitpid(pid, os.WNOHANG)[0]:
                state['launcher_pid'] = None
                return True
            time.sleep(.01)
    except ChildProcessError:
        state['launcher_pid'] = None
        return True
    except ProcessLookupError:
        pass
    return False


def stop_owned(state, emulator, binary, deadline):
    pid = state['pid']
    if pid is None:
        # Before PID acknowledgement, the unreaped direct child reserves the
        # session/group ID created by setsid. Only this launch can own that
        # group. Stop its pre-exec helpers before reaping releases the ID.
        launcher = state.get('launcher_pid')
        if launcher is not None:
            try:
                os.killpg(launcher, signal.SIGKILL)
            except ProcessLookupError:
                pass  # setsid may not have run; reap_launcher stops our child.
        return reap_launcher(state, deadline)
    launcher_reaped = reap_launcher(state, deadline)
    term_sent = kill_sent = False
    kill_after = min(time.monotonic()+2, deadline-.5)
    while time.monotonic() < deadline:
        command = process_command(pid, deadline)
        if command is None:
            return launcher_reaped
        if command == f'{emulator} -w {binary}':
            sig = signal.SIGKILL if time.monotonic() >= kill_after else signal.SIGTERM
            if (sig == signal.SIGTERM and not term_sent) or (sig == signal.SIGKILL and not kill_sent):
                try:
                    os.kill(pid, sig)
                except ProcessLookupError:
                    return launcher_reaped
                term_sent |= sig == signal.SIGTERM
                kill_sent |= sig == signal.SIGKILL
        # Before exec, a helper may still be observing its closed owner pipe.
        # A different command is never signaled and never assumed cleaned up.
        time.sleep(min(.05, max(0, deadline-time.monotonic())))
    return False


def paths_arg(value):
    path = Path(value)
    if path.is_absolute() or '..' in path.parts or not path.parts:
        raise argparse.ArgumentTypeError('use a relative path inside the guest profile')
    return path


def read_log(path):
    return read_slice(path, 1024 * 1024, tail=True)


def profile_config(args):
    sections = {
        'Core': {'use_cpu_jit': 'true', 'cpu_clock_percentage': str(args.cpu_clock)},
        'System': {'is_new_3ds': 'true'},
        'Debugging': {'enable_exception_handler': 'true'},
        'Renderer': {'graphics_api': {'software': '0', 'opengl': '1', 'vulkan': '2'}[args.renderer],
                     'resolution_factor': '1'},
        'Layout': {'render_3d': '0'},
        'UI': {'first_start': 'false', 'confirmClose': 'false'},
        # Azahar reads the update check from Miscellaneous; under UI it is ignored.
        'Miscellaneous': {'enable_telemetry': 'false', 'check_for_update_on_start': 'false'},
    }
    if args.touch_key:
        code, x, y = args.touch_key
        sections['Controls'] = {
            'profiles\\size': '1', 'profiles\\1\\use_touch_from_button': 'true',
            'touch_from_button_maps\\size': '1',
            'touch_from_button_maps\\1\\entries\\size': '1',
            'touch_from_button_maps\\1\\entries\\1\\bind': f'"code:{code},engine:keyboard,x:{x},y:{y}"',
        }
    return ''.join(f'[{section}]\n' + ''.join(f'{key}={value}\n{key}\\default=false\n'
                                             for key, value in entries.items())
                   for section, entries in sections.items())


def take_capture(tool, pid, out, second, index, marker=None):
    lookup = subprocess.run([tool, str(pid)], text=True, capture_output=True, timeout=2)
    window = lookup.stdout.strip()
    if lookup.returncode or not window.isdecimal():
        return {'second': second, 'status': 'no-owned-window', 'error': lookup.stderr.strip()}
    image = out / f'frame-{index:02d}-{second:06.2f}.png'
    result = subprocess.run(['/usr/sbin/screencapture', '-x', '-o', '-l', window, str(image)],
                            text=True, capture_output=True, timeout=4)
    ok = result.returncode == 0 and image.is_file()
    return {'second': second, 'marker': marker, 'status': 'saved' if ok else 'failed',
            'path': str(image), 'error': result.stderr.strip(),
            **({'sha256': sha(image)} if ok else {})}


def emulator_active():
    active = subprocess.check_output(['ps', '-axo', 'comm='], text=True, timeout=3)
    return any(Path(line.strip()).name.lower() in {'azahar', 'citra', 'citra-qt'}
               for line in active.splitlines())


def lock_when_available(lock, seconds):
    deadline = time.monotonic() + seconds
    while True:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            pass
        else:
            if not emulator_active():
                return True
            fcntl.flock(lock, fcntl.LOCK_UN)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        time.sleep(min(.25, remaining))


def run(args):
    emulator, source, out = (path.resolve() for path in (args.emulator, args.binary, args.output))
    out.mkdir(parents=True)
    binary = out / 'tested-AssaultCube.3dsx'
    shutil.copy2(source, binary)
    elf = None
    if args.elf:
        elf = out / 'tested-AssaultCube.elf'
        shutil.copy2(args.elf, elf)
    env = os.environ.copy()
    env.pop('TERM', None)
    for category in ('data', 'config', 'cache'):
        base = out / category
        (base / 'azahar-emu').mkdir(parents=True)
        env[f'XDG_{category.upper()}_HOME'] = str(base)
    if args.assets:
        # External assets are input only. The emulator can change its private copy.
        game = out / 'data/azahar-emu/sdmc/3ds/assaultcube'
        shutil.copytree(args.assets, game)
    else:
        game = out / 'data/azahar-emu/sdmc/3ds/assaultcube'
        game.mkdir(parents=True)
    profile = game / 'profile'
    if args.profile:
        shutil.copytree(args.profile, profile, dirs_exist_ok=True)
    (profile / 'config').mkdir(parents=True, exist_ok=True)
    if args.dsp:
        shutil.copy2(args.dsp, game.parent / 'dspfirm.cdc')
    if args.guest_args:
        shutil.copy2(args.guest_args, game / 'server.args')
    if args.autoexec:
        shutil.copy2(args.autoexec, profile / 'config/autoexec.cfg')
    for target, staged_input in args.sd_file:
        destination = game.parent.parent / target
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(staged_input, destination)
    config = out / 'config/azahar-emu/qt-config.ini'
    config.write_text(profile_config(args))
    azahar_log = out / 'data/azahar-emu/log/azahar_log.txt'
    client_log = profile / 'clientlog.txt'
    # A seed may include logs from a prior run. Only match this guest's log.
    client_log.unlink(missing_ok=True)
    process_log = out / 'process.log'
    receipt_path = out / 'result.json'
    receipt = {'scope': 'Azahar New 3DS guest behavior and timings',
               'binary': str(binary), 'binary_source': str(source), 'binary_sha256': sha(binary),
               'elf': str(elf) if elf else None, 'elf_sha256': sha(elf) if elf else None,
               'emulator': str(emulator), 'emulator_sha256': sha(emulator),
               'renderer_requested': args.renderer, 'renderer_observed': None,
               'cpu_clock_percentage': args.cpu_clock, 'config_sha256': sha(config),
               'dsp_sha256': sha(args.dsp) if args.dsp else None,
               'required_log_text': args.require, 'rejected_log_text': args.reject,
               'captures': [], 'cleanup_complete': False,
               'guest_arguments_sha256': sha(args.guest_args) if args.guest_args else None,
               'autoexec_sha256': sha(args.autoexec) if args.autoexec else None,
               'sd_inputs': [{'path': str(target), 'sha256': sha(source)}
                             for target, source in args.sd_file],
               'status': 'launching'}
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    started = time.monotonic()
    deadline = started + args.timeout
    observe_until = deadline - 5
    state = {'pid': None, 'launcher_pid': None, 'signal': None}

    def request_stop(signum, _frame):
        state['signal'] = signum

    handlers = {sig: signal.signal(sig, request_stop) for sig in (signal.SIGINT, signal.SIGTERM)}
    planned = iter(sorted(set(args.capture)))
    capture_second = next(planned, None)
    capture_markers = list(dict.fromkeys(args.capture_when))
    try:
        pid = launch(emulator, binary, env, str(process_log), state, min(started+8, observe_until))
        receipt['pid'] = pid
        receipt['status'] = 'running'
        receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
        print(f'Azahar PID {pid}. Receipts in {out}', flush=True)
        while time.monotonic() < observe_until:
            if state['signal'] is not None:
                raise InterruptedError
            if not observe_backend(receipt, azahar_log):
                break
            crash = read_slice(azahar_log, 32 * 1024, tail=True)
            if 'Exception Type:' in crash and 'Core <Critical>' in crash:
                receipt['status'] = 'guest-exception'
                receipt['exception_tail'] = crash[-4096:]
                break
            elapsed = time.monotonic()-started
            if capture_second is not None and elapsed >= capture_second:
                if not owned(pid, emulator, binary, observe_until):
                    raise RuntimeError('Owned Azahar exited before capture')
                receipt['captures'].append(take_capture(str(args.window_tool), pid, out,
                                                        capture_second, len(receipt['captures'])))
                capture_second = next(planned, None)
            log = read_log(client_log) + '\n' + read_log(process_log)
            if any(marker in log for marker in args.reject):
                receipt['status'] = 'rejected-log-text'
                break
            for marker in capture_markers[:]:
                if marker in log:
                    if not owned(pid, emulator, binary, observe_until):
                        raise RuntimeError('Owned Azahar exited before capture')
                    receipt['captures'].append(take_capture(str(args.window_tool), pid, out, elapsed,
                                                            len(receipt['captures']), marker))
                    capture_markers.remove(marker)
            missing = [text for text in args.require if text not in log]
            absent = [str(path) for path in args.expect if not (profile / path).is_file()]
            have_condition = args.require or args.expect or args.capture_when
            if have_condition and not missing and not absent and capture_second is None and not capture_markers:
                receipt['status'] = 'observed'
                break
            # Azahar returns to its library when a homebrew process exits.
            # Do not wait out the guest observation budget for missing markers.
            if '[mvk-info] Destroyed VkDevice on GPU' in read_log(process_log):
                receipt['status'] = 'guest-exited-before-observation'
                break
            if elapsed > 2 and not owned(pid, emulator, binary, observe_until):
                receipt['status'] = 'emulator-exited'
                break
            time.sleep(.15)
        else:
            receipt['status'] = 'timed-out' if args.require or args.expect or args.capture_when else 'observation-complete'
    except (InterruptedError, KeyboardInterrupt):
        receipt['status'] = 'interrupted'
    except Exception as error:
        receipt['status'] = 'runner-error'
        receipt['error'] = str(error)
    finally:
        for sig in handlers:
            signal.signal(sig, signal.SIG_IGN)
        try:
            receipt['cleanup_complete'] = stop_owned(state, emulator, binary, deadline)
            observe_backend(receipt, azahar_log)
        except Exception as error:
            receipt['cleanup_error'] = str(error)
        for sig, handler in handlers.items():
            signal.signal(sig, handler)
        log = read_log(client_log) + '\n' + read_log(process_log)
        receipt['missing_log_text'] = [text for text in args.require if text not in log]
        receipt['observed_rejected_log_text'] = [text for text in args.reject if text in log]
        receipt['missing_files'] = [str(path) for path in args.expect if not (profile / path).is_file()]
        receipt['performance'] = list(dict.fromkeys(line.strip() for line in log.splitlines()
                                                     if line.lstrip().startswith('AC3DS_PERF ')))
        receipt['elapsed_seconds'] = round(time.monotonic()-started, 3)
        receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
        print(json.dumps(receipt, indent=2))
    return 0 if (receipt['cleanup_complete'] and not receipt['observed_rejected_log_text'] and
                 receipt['renderer_observed'] == args.renderer and
                 receipt['status'] in {'observed', 'observation-complete'} and
                 all(capture['status'] == 'saved' for capture in receipt['captures'])) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emulator', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--elf', type=Path, help='Copy the corresponding ELF for crash symbolication')
    parser.add_argument('--output', type=Path, required=True, help='Fresh output directory')
    parser.add_argument('--dsp', type=Path)
    parser.add_argument('--assets', type=Path, help='Staged external assets, if not in RomFS')
    parser.add_argument('--profile', type=Path, help='Seed with a copy of this prior guest profile')
    parser.add_argument('--guest-args', type=Path, help='Source for the guest server.args file')
    parser.add_argument('--autoexec', type=Path, help='Source for the guest config/autoexec.cfg')
    parser.add_argument('--sd-file', action='append', nargs=2, metavar=('DESTINATION','SOURCE'),
                        default=[], help='Copy a file to a relative path in the private guest SD card')
    parser.add_argument('--require', action='append', default=[], help='Literal guest log marker')
    parser.add_argument('--reject', action='append', default=[], help='Literal guest log text that fails the run')
    parser.add_argument('--expect', type=paths_arg, action='append', default=[], help='Guest profile file to observe')
    parser.add_argument('--timeout', type=int, default=90)
    parser.add_argument('--lock-wait', type=int, default=0,
                        help='Wait this many seconds for the shared emulator before the run timeout starts')
    parser.add_argument('--renderer', choices=('software', 'opengl', 'vulkan'), default='vulkan')
    parser.add_argument('--cpu-clock', type=int, default=100)
    parser.add_argument('--touch-key', nargs=3, type=int, metavar=('QT_KEY', 'X', 'Y'),
                        help='Map one host Qt key code to a guest touchscreen point for HID QA')
    parser.add_argument('--window-tool', type=Path, help='Existing utility that prints the owned Azahar window ID for a PID')
    parser.add_argument('--capture', type=float, action='append', default=[], help='Wall seconds to screenshot; repeat as needed')
    parser.add_argument('--capture-when', action='append', default=[],
                        help='Capture the owned window after a literal guest log marker appears')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('This runner supports macOS')
    if not 12 <= args.timeout <= 600 or not 5 <= args.cpu_clock <= 400:
        parser.error('Use a timeout of 12..600 seconds and an emulated CPU clock of 5..400 percent')
    if not 0 <= args.lock_wait <= 600:
        parser.error('Use a lock wait of 0..600 seconds')
    if args.touch_key:
        code, x, y = args.touch_key
        if not (0 < code <= 0x01ffffff and 0 <= x < 320 and 0 <= y < 240):
            parser.error('Specify a Qt key code and touchscreen coordinates inside 320 by 240')
    if (args.capture or args.capture_when) and (not args.window_tool or
                                               any(not 2 <= at < args.timeout-8 for at in args.capture)):
        parser.error('Captures require --window-tool and must fit the observation window')
    try:
        args.sd_file = [(paths_arg(target), Path(source)) for target, source in args.sd_file]
    except argparse.ArgumentTypeError as error:
        parser.error(str(error))
    for path in (args.emulator, args.binary, args.elf, args.dsp, args.guest_args, args.autoexec,
                 args.window_tool, *(source for _, source in args.sd_file)):
        if path is not None and not path.is_file():
            parser.error(f'Missing input file: {path}')
    for path in (args.assets, args.profile):
        if path is not None and not path.is_dir():
            parser.error(f'Missing input directory: {path}')
    lock_path = Path(tempfile.gettempdir()) / f'halo3ds-azahar-{os.getuid()}.lock'
    with os.fdopen(os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600), 'r+') as lock:
        if not lock_when_available(lock, args.lock_wait):
            parser.error('Another 3DS project or an active process owns the emulator')
        return run(args)


if __name__ == '__main__':
    raise SystemExit(main())
