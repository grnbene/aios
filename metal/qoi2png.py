#!/usr/bin/env python3
"""Decode a QOI image (the Pi's full-resolution screenshot, tcp/42427) to PNG.

    nc -w 20 <pi-ip> 42427 > shot.qoi && metal/qoi2png.py shot.qoi shot.png
"""
import struct
import sys

from PIL import Image


def decode(data: bytes):
    if data[:4] != b"qoif":
        raise ValueError("not a QOI file")
    w, h = struct.unpack(">II", data[4:12])
    px = bytearray(w * h * 3)
    index = [(0, 0, 0)] * 64
    r = g = b = 0
    p = 14
    run = 0
    for i in range(w * h):
        if run:
            run -= 1
        elif p < len(data) - 8:
            op = data[p]
            p += 1
            if op == 0xFE:
                r, g, b = data[p], data[p + 1], data[p + 2]
                p += 3
            elif op == 0xFF:
                r, g, b = data[p], data[p + 1], data[p + 2]
                p += 4
            elif op >> 6 == 0:
                r, g, b = index[op]
            elif op >> 6 == 1:
                r = (r + ((op >> 4) & 3) - 2) & 0xFF
                g = (g + ((op >> 2) & 3) - 2) & 0xFF
                b = (b + (op & 3) - 2) & 0xFF
            elif op >> 6 == 2:
                dg = (op & 0x3F) - 32
                nb = data[p]
                p += 1
                r = (r + dg + (nb >> 4) - 8) & 0xFF
                g = (g + dg) & 0xFF
                b = (b + dg + (nb & 0x0F) - 8) & 0xFF
            else:
                run = op & 0x3F
            index[(r * 3 + g * 5 + b * 7 + 255 * 11) % 64] = (r, g, b)
        px[i * 3:i * 3 + 3] = bytes((r, g, b))
    return Image.frombytes("RGB", (w, h), bytes(px))


if __name__ == "__main__":
    decode(open(sys.argv[1], "rb").read()).save(sys.argv[2])
