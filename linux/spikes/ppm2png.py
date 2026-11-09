#!/usr/bin/env python3
"""Convert a binary PPM (P6) written by kwin_cast --dump to PNG, optionally
shrinking it by an integer factor. Standard library only.

Usage: ppm2png.py in.ppm out.png [shrink]
"""
import struct
import sys
import zlib


def main():
    src, dst = sys.argv[1], sys.argv[2]
    shrink = int(sys.argv[3]) if len(sys.argv) > 3 else 1

    data = open(src, "rb").read()
    # Header: "P6\n<w> <h>\n255\n"
    parts = data.split(b"\n", 3)
    width, height = map(int, parts[1].split())
    pixels = parts[3]

    out_w, out_h = width // shrink, height // shrink
    rows = bytearray()
    for y in range(out_h):
        start = y * shrink * width * 3
        row = pixels[start:start + width * 3]
        rows.append(0)  # filter type: none
        if shrink == 1:
            rows += row
        else:
            for x in range(out_w):
                rows += row[x * shrink * 3:x * shrink * 3 + 3]

    def chunk(kind, payload):
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", out_w, out_h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6))
    png += chunk(b"IEND", b"")
    open(dst, "wb").write(png)
    print(f"{dst}: {out_w}x{out_h}")


if __name__ == "__main__":
    main()
