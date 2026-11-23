#!/usr/bin/env python3
"""Resolve the addresses in a handheld crash.txt to function names.

Usage: scripts/symbolize-crash.py crash.txt [assaultcube.elf] [--force]

The ELF must be the one the crashed .3dsx was packaged from. The script checks
that: the instruction words the report prints around pc must match the ELF,
and (build 12 and later) the report's "image:" addresses must match the ELF's
marker symbols. Without an ELF argument it searches build*/ for one that
matches. --force symbolizes against a mismatched ELF anyway.

Uses arm-none-eabi-nm from $DEVKITARM, $DEVKITPRO or .deps/devkitpro.
"""
import bisect
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MARKERS = {'text': 'ac3ds_image_text_marker', 'data': 'ac3ds_image_data_marker', 'bss': 'ac3ds_image_bss_marker'}


def find_nm():
    candidates = []
    if os.environ.get('DEVKITARM'):
        candidates.append(Path(os.environ['DEVKITARM']) / 'bin/arm-none-eabi-nm')
    if os.environ.get('DEVKITPRO'):
        candidates.append(Path(os.environ['DEVKITPRO']) / 'devkitARM/bin/arm-none-eabi-nm')
    candidates.append(ROOT / '.deps/devkitpro/devkitARM/bin/arm-none-eabi-nm')
    for c in candidates:
        if c.exists():
            return str(c)
    return 'arm-none-eabi-nm'


def load_symbols(elf):
    """Returns (sorted code addresses, their names, {plain name: address})."""
    out = subprocess.run([find_nm(), '-n', '--defined-only', str(elf)],
                         check=True, capture_output=True, text=True).stdout
    plain = {}
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3:
            plain.setdefault(parts[2], int(parts[0], 16))
    out = subprocess.run([find_nm(), '-C', '-n', '--defined-only', str(elf)],
                         check=True, capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in 'tTwW':
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names, plain


def read_words(elf, address, count):
    """Reads little-endian words at a virtual address from the ELF's PT_LOAD segments."""
    data = Path(elf).read_bytes()
    if data[:4] != b'\x7fELF' or data[4] != 1:
        return None
    phoff, = struct.unpack_from('<I', data, 0x1c)
    phentsize, phnum = struct.unpack_from('<HH', data, 0x2a)
    for i in range(phnum):
        kind, offset, vaddr, _, filesz, _, flags = struct.unpack_from('<IIIIIII', data, phoff + i * phentsize)
        # A writable segment changes at run time, so its file contents prove nothing.
        if kind == 1 and not flags & 2 and vaddr <= address and address + 4 * count <= vaddr + filesz:
            return list(struct.unpack_from(f'<{count}I', data, offset + address - vaddr))
    return None


def check(report, elf, plain):
    """Returns (verdict, notes): True = matches, False = does not, None = nothing to check."""
    notes, verdict = [], None
    pc = re.search(r'\bpc=([0-9a-f]{8})', report)
    words = re.search(r'code@pc-8:((?:\s+\[?[0-9a-f]{8}\]?){5})', report)
    if pc and words:
        expect = [int(w, 16) for w in re.findall(r'[0-9a-f]{8}', words.group(1))]
        found = read_words(elf, (int(pc.group(1), 16) & ~3) - 8, 5)
        if found is None:
            # A wild jump, or pc in writable data: the words prove nothing.
            notes.append("pc is not in this ELF's code or read-only data; instruction words not checked")
        else:
            verdict = found == expect
            if not verdict:
                notes.append('instruction words around pc differ from this ELF')
    image = re.search(r'image: text=([0-9a-f]{8}) data=([0-9a-f]{8}) bss=([0-9a-f]{8})', report)
    if image:
        run = dict(zip(('text', 'data', 'bss'), (int(g, 16) for g in image.groups())))
        if all(MARKERS[k] in plain for k in run):
            linked = {k: plain[MARKERS[k]] for k in run}
            shift = {k: run[k] - linked[k] for k in run}
            if len(set(shift.values())) == 1 and shift['text'] == 0:
                verdict = True if verdict is None else verdict
            elif shift['text'] == 0 and shift['data'] == shift['bss'] and shift['data'] % 0x1000 == 0:
                # Code is where it was linked, so the addresses below resolve.
                notes.append(f'the loader placed data and bss {shift["data"]:+#x} from their link addresses: '
                             'this launch path does not keep the linked layout')
                verdict = True if verdict is None else verdict
            elif len(set(shift.values())) != 1:
                notes.append('segments are not where this ELF links them: ' +
                             ', '.join(f'{k} {shift[k]:+#x}' for k in shift) +
                             ' (a different ELF, or a loader that moved a segment)')
                verdict = False
            else:
                notes.append(f'image loaded {shift["text"]:+#x} from its link address')
        else:
            notes.append('ELF has no image marker symbols (built before build 12)')
    return verdict, notes


def candidates():
    seen = set()
    for pattern in ('build*/assaultcube.elf', 'build*/logs/**/*.elf', 'build-azahar/*/tested-AssaultCube.elf'):
        for path in sorted(ROOT.glob(pattern)):
            key = path.resolve()
            if key not in seen:
                seen.add(key)
                yield path


def resolve(addr, addrs, names):
    i = bisect.bisect_right(addrs, addr) - 1
    if i < 0:
        return '?'
    return f'{names[i]}+0x{addr - addrs[i]:x}'


def main():
    args = [a for a in sys.argv[1:] if a != '--force']
    force = '--force' in sys.argv[1:]
    if not args or args[0] in ('-h', '--help'):
        print(__doc__)
        return 2
    report = Path(args[0]).read_text(errors='replace')
    if len(args) > 1:
        elf = Path(args[1])
        addrs, names, plain = load_symbols(elf)
        verdict, notes = check(report, elf, plain)
        if verdict is False and not force:
            print(f'{elf} does not match this report: ' + '; '.join(notes), file=sys.stderr)
            print('Pass the ELF the crashed .3dsx was built from, or --force.', file=sys.stderr)
            return 1
    else:
        elf = None
        for path in candidates():
            try:
                addrs, names, plain = load_symbols(path)
            except (subprocess.CalledProcessError, OSError):
                continue
            verdict, notes = check(report, path, plain)
            if verdict:
                elf = path
                break
        if elf is None:
            print('No ELF under build*/ matches this report. Pass the right one explicitly.', file=sys.stderr)
            return 1
    state = {True: 'verified', False: 'MISMATCH (forced)', None: 'NOT VERIFIED (report has nothing to check)'}[verdict]
    print(f'# ELF: {elf} [{state}]')
    for note in notes:
        print(f'# note: {note}')

    def annotate(match):
        value = int(match.group(2), 16) & ~1
        if value < 0x00100000 or not addrs or value > addrs[-1] + 0x100000:
            return match.group(0)
        return f'{match.group(0)}  <{resolve(value, addrs, names)}>'

    for line in report.splitlines():
        if re.match(r'\s*(pc=|sp\+|caller=)', line):
            line = re.sub(r'(pc=|lr=|caller=|sp\+[0-9a-f]+ )([0-9a-f]{8})', annotate, line)
        print(line)
    return 0


if __name__ == '__main__':
    sys.exit(main())
