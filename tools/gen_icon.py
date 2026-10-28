#!/usr/bin/env python3
"""Generate icon.bmp — a 32x32 16-colour paletted BMP for the DS ROM header.

ndstool requires an indexed bitmap; 24-bit BMPs are rejected.
"""

import struct
import sys

W = H = 32
BPP = 4
PALETTE = [
    (0x00, 0x00, 0x00),  # 0 transparent (index 0 is transparent on DS)
    (0x14, 0x16, 0x20),  # 1 background
    (0x1d, 0x21, 0x2e),  # 2 background highlight
    (0x2b, 0x31, 0x42),  # 3 border
    (0x3d, 0x74, 0xd8),  # 4 accent dark
    (0x50, 0x96, 0xff),  # 5 accent
    (0x7d, 0xb4, 0xff),  # 6 accent light
    (0xc7, 0xdf, 0xff),  # 7 accent pale
]
PALETTE += [(0, 0, 0)] * (16 - len(PALETTE))

# 32x32, top row first. Each char is a palette index in hex.
ART = """
00000000000000000000000000000000
00111111111111111111111111110000
01222222222222222222222222221000
01222222222222222266666666621000
01222222222222226677777777621000
01222222222222667722222266621000
01222222222266772222222266621000
01222222226677222222222266621000
01222222667722222222222266621000
01222266772222222222222266621000
01222667222222222222222266621000
01226672222222222222222266621000
01226662222222222222222266621000
01226662222222222222222266621000
01226662222222222222222266621000
01226662222222222222222266621000
01226662222222222222222266621000
01226662222222222222222266621000
01226662222222222222222266621000
01226662222222222222222266621000
01226662222222222222266666621000
01226662222222222266777766621000
01226662222222226677777766621000
01666662222222667777777766621000
16777766222226677777777766621000
16777776222266777777777766621000
16777776222667777777777766621000
16777766226677777777777766621000
16666622226677777777777766621000
01222222222667777777766666621000
01222222222222666666662222221000
00111111111111111111111111110000
"""


def build_rows():
    rows = [r for r in ART.strip().split("\n")]
    if len(rows) != H:
        sys.exit(f"art has {len(rows)} rows, expected {H}")
    for i, r in enumerate(rows):
        if len(r) != W:
            sys.exit(f"art row {i} has {len(r)} cols, expected {W}")
    return rows


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "icon.bmp"
    rows = build_rows()

    row_bytes = W * BPP // 8  # 16, already 4-byte aligned
    pixel_size = row_bytes * H
    palette_size = 16 * 4
    offset = 14 + 40 + palette_size

    pixels = bytearray()
    for row in reversed(rows):  # BMP stores bottom-up
        for x in range(0, W, 2):
            hi = int(row[x], 16)
            lo = int(row[x + 1], 16)
            pixels.append((hi << 4) | lo)

    header = struct.pack("<2sIHHI", b"BM", offset + pixel_size, 0, 0, offset)
    dib = struct.pack(
        "<IiiHHIIiiII", 40, W, H, 1, BPP, 0, pixel_size, 2835, 2835, 16, 0
    )
    palette = b"".join(struct.pack("BBBB", b, g, r, 0) for r, g, b in PALETTE)

    with open(out, "wb") as f:
        f.write(header + dib + palette + bytes(pixels))
    print(f"wrote {out} ({W}x{H}, {BPP}bpp, 16-colour palette)")


if __name__ == "__main__":
    main()
