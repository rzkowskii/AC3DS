#!/usr/bin/env python3
"""Scale the pinned release's 32×32 RGBA icon to the SMDH tool's 48×48 PNG."""

import argparse
import pathlib
import struct
import zlib


PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
SOURCE_SIDE = 32
TARGET_SIDE = 48
PIXEL_BYTES = 4


def paeth(left, above, upper_left):
    estimate = left + above - upper_left
    return min((left, above, upper_left), key=lambda sample: abs(estimate - sample))


def rgba_rows(data):
    if not data.startswith(PNG_MAGIC):
        raise ValueError("Release icon is not a PNG")
    offset = len(PNG_MAGIC)
    compressed = bytearray()
    header = None
    while offset + 12 <= len(data):
        length, kind = struct.unpack_from(">I4s", data, offset)
        end = offset + 8 + length
        if end + 4 > len(data):
            raise ValueError("Truncated PNG chunk")
        payload = data[offset + 8:end]
        offset = end + 4
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", payload)
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            break
    if header != (SOURCE_SIDE, SOURCE_SIDE, 8, 6, 0, 0, 0):
        raise ValueError("Release icon must be a noninterlaced 32×32 RGBA PNG")

    stride = SOURCE_SIDE * PIXEL_BYTES
    raw = zlib.decompress(compressed)
    if len(raw) != (stride + 1) * SOURCE_SIDE:
        raise ValueError("Unexpected PNG image data size")
    previous = bytearray(stride)
    for row_start in range(0, len(raw), stride + 1):
        mode = raw[row_start]
        if mode > 4:
            raise ValueError("Unsupported PNG filter")
        row = bytearray(raw[row_start + 1:row_start + 1 + stride])
        for x, value in enumerate(row):
            left = row[x - PIXEL_BYTES] if x >= PIXEL_BYTES else 0
            above = previous[x]
            upper_left = previous[x - PIXEL_BYTES] if x >= PIXEL_BYTES else 0
            if mode == 1:
                value += left
            elif mode == 2:
                value += above
            elif mode == 3:
                value += (left + above) // 2
            elif mode == 4:
                value += paeth(left, above, upper_left)
            row[x] = value & 255
        yield row
        previous = row


def chunk(kind, payload):
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))


def resize(source, destination):
    rows = list(rgba_rows(source.read_bytes()))
    # Nearest neighbor keeps the release's small pixel-art edges sharp.
    columns = [x * SOURCE_SIDE // TARGET_SIDE * PIXEL_BYTES for x in range(TARGET_SIDE)]
    pixels = bytearray()
    for y in range(TARGET_SIDE):
        row = rows[y * SOURCE_SIDE // TARGET_SIDE]
        pixels.append(0)  # No PNG filter.
        for start in columns:
            pixels.extend(row[start:start + PIXEL_BYTES])

    header = struct.pack(">IIBBBBB", TARGET_SIDE, TARGET_SIDE, 8, 6, 0, 0, 0)
    destination.write_bytes(PNG_MAGIC + chunk(b"IHDR", header) +
                            chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("destination", type=pathlib.Path)
    args = parser.parse_args()
    resize(args.source, args.destination)
