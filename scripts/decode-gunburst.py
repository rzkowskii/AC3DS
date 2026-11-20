#!/usr/bin/env python3
"""Turn a gunburst_N.bin frame recording into pictures.

The game writes these with the console command "ac3dsgunburst [frames]": the
right half of the top screen for a run of consecutive frames, copied from the
GPU's render target before it is sent to the display. Each frame is stored in
the PICA's own layout (8x8 tiles in Morton order, the screen turned on its
side), which this script undoes.

    scripts/decode-gunburst.py gunburst_0.bin            # writes gunburst_0.png (contact sheet)
    scripts/decode-gunburst.py gunburst_0.bin --frames   # also one PNG per frame

The sheet's caption line per frame is printed to the terminal: milliseconds
since the recording started, queue barriers in that frame, the running count
of frames held back for the previous one to be shown, and the VBlank counter.
"""
import struct
import sys
import zlib

HEADER_BYTES = 4096
MORTON = [0] * 64
for _i in range(64):
    _x, _y = _i & 7, _i >> 3
    MORTON[_i] = (_x & 1) | ((_y & 1) << 1) | ((_x & 2) << 1) | ((_y & 2) << 2) | ((_x & 4) << 2) | ((_y & 4) << 3)


def write_png(path, width, height, rows):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    raw = b''.join(b'\x00' + bytes(row) for row in rows)
    with open(path, 'wb') as out:
        out.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                  chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def decode_frame(data, tile_rows, tiles_per_row):
    """Return rows of RGB bytes for the screen region, upright."""
    buf_w, buf_h = tiles_per_row * 8, tile_rows * 8  # buffer x = screen bottom-to-top, buffer y = screen x
    rows = [bytearray(buf_h * 3) for _ in range(buf_w)]
    for ty in range(tile_rows):
        for tx in range(tiles_per_row):
            base = (ty * tiles_per_row + tx) * 256
            for i in range(64):
                p = base + MORTON[i] * 4
                bx, by = tx * 8 + (i & 7), ty * 8 + (i >> 3)
                row = rows[buf_w - 1 - bx]
                row[by * 3] = data[p + 3]
                row[by * 3 + 1] = data[p + 2]
                row[by * 3 + 2] = data[p + 1]
    return buf_h, buf_w, rows


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        sys.exit(__doc__)
    path = args[0]
    blob = open(path, 'rb').read()
    magic, version, frames, tile_rows, tiles_per_row, first_row, buf_w, buf_h = struct.unpack_from('<8I', blob, 0)
    if magic != 0x42473341 or version != 1:
        sys.exit('%s: not a frame recording' % path)
    frame_bytes = tile_rows * tiles_per_row * 256
    if len(blob) < HEADER_BYTES + frames * frame_bytes:
        sys.exit('%s: truncated (%d frames expected)' % (path, frames))
    stem = path[:-4] if path.endswith('.bin') else path
    decoded = []
    for f in range(frames):
        ms, barriers, waits, vblank = struct.unpack_from('<4I', blob, 64 + f * 16)
        start = HEADER_BYTES + f * frame_bytes
        width, height, rows = decode_frame(blob[start:start + frame_bytes], tile_rows, tiles_per_row)
        decoded.append((width, height, rows))
        print('frame %2d: %5d ms, %u barriers, %u held, vblank %u' % (f, ms, barriers, waits, vblank))
        if '--frames' in sys.argv:
            write_png('%s_%02d.png' % (stem, f), width, height, rows)
    if not decoded:
        sys.exit('%s: no frames' % path)
    width, height = decoded[0][0], decoded[0][1]
    cols = 6
    gap = 2
    sheet_rows = (len(decoded) + cols - 1) // cols
    sheet_w, sheet_h = cols * (width + gap) - gap, sheet_rows * (height + gap) - gap
    sheet = [bytearray(b'\x30' * (sheet_w * 3)) for _ in range(sheet_h)]
    for n, (_, _, rows) in enumerate(decoded):
        ox, oy = (n % cols) * (width + gap), (n // cols) * (height + gap)
        for y in range(height):
            sheet[oy + y][ox * 3:(ox + width) * 3] = rows[y]
    write_png(stem + '.png', sheet_w, sheet_h, sheet)
    print('%s.png: %d frames, screen x %d-%d' % (stem, frames, first_row * 8, first_row * 8 + tile_rows * 8 - 1))


if __name__ == '__main__':
    main()
