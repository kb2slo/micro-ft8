#!/usr/bin/env python3
"""Draw the Home Screen icon, webapp/app/icon-180.png (docs/architecture/webapp.md, Home Screen).

Eight bars rising left to right, like FT8's eight tones, white on the app's accent blue. iOS rounds the
corners and writes the name under it (apple-mobile-web-app-title), so the square carries no text. Standard
library only, so it runs anywhere: python3 webapp/tools/make_icon.py
"""

import struct
import zlib
from pathlib import Path

SIZE = 180
BG = (0x1D, 0x4E, 0xD8)  # --accent in app.html
FG = (0xFF, 0xFF, 0xFF)
BAR_W, GAP, BASE, LOW, STEP = 14, 5, 140, 30, 10  # bar width, gap, baseline y, first bar's height, rise per bar


def bars():
    left = (SIZE - (8 * BAR_W + 7 * GAP)) // 2
    for k in range(8):
        x = left + k * (BAR_W + GAP)
        h = LOW + k * STEP
        yield x, BASE - h, x + BAR_W, BASE


def png(pixels):
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in pixels)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    pixels = [[BG] * SIZE for _ in range(SIZE)]
    for x0, y0, x1, y1 in bars():
        for y in range(y0, y1):
            for x in range(x0, x1):
                pixels[y][x] = FG
    out = Path(__file__).resolve().parents[1] / "app" / "icon-180.png"
    out.write_bytes(png(pixels))
    print(f"wrote {out.relative_to(Path.cwd()) if out.is_relative_to(Path.cwd()) else out}")


if __name__ == "__main__":
    main()
