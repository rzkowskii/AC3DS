#!/usr/bin/env python3
"""Resolve the addresses in a handheld crash.txt to function names.

Usage: scripts/symbolize-crash.py crash.txt [build-dir/assaultcube.elf]

Uses arm-none-eabi-nm from $DEVKITARM or .deps/devkitpro. The ELF must come
from the same build that produced the 3DSX that crashed.
"""
import bisect
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


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
    out = subprocess.run([find_nm(), '-C', '-n', '--defined-only', elf],
                         check=True, capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in 'tTwW':
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def resolve(addr, addrs, names):
    i = bisect.bisect_right(addrs, addr) - 1
    if i < 0:
        return '?'
    return f'{names[i]}+0x{addr - addrs[i]:x}'


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    report = Path(sys.argv[1]).read_text(errors='replace')
    elf = sys.argv[2] if len(sys.argv) > 2 else str(ROOT / 'build-3ds/assaultcube.elf')
    addrs, names = load_symbols(elf)
    lo = 0x00100000

    def annotate(match):
        value = int(match.group(2), 16) & ~1
        if value < lo or not addrs or value > addrs[-1] + 0x100000:
            return match.group(0)
        return f'{match.group(0)}  <{resolve(value, addrs, names)}>'

    for line in report.splitlines():
        if re.match(r'\s*(pc|sp\+)', line) or line.startswith('pc='):
            line = re.sub(r'(pc=|lr=|sp\+[0-9a-f]+ )([0-9a-f]{8})', annotate, line)
        print(line)
    return 0


if __name__ == '__main__':
    sys.exit(main())
