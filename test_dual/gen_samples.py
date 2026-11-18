#!/usr/bin/env python3
"""Generate two tiny self-made JPEG samples for the dual-output tests.

Image type 1: color YCbCr 4:2:0, deliberately odd-sized (31x17) so the
              right/bottom MCU edge clipping is exercised.
Image type 2: grayscale, 24x16 (clean MCU grid, single component).

No third-party library is added to the repository; Pillow is a host-side
test tool only and never linked into the decoder.
"""
import os
import sys
import site

# Keep interpreter isolation (-I: no cwd/script-dir imports, no env vars) while
# still allowing the host's user-installed Pillow; never rely on sys.path[0].
_sys_site = site.getusersitepackages()
if _sys_site not in sys.path:
    sys.path.insert(0, _sys_site)

from PIL import Image

OUT = os.path.join(os.path.dirname(__file__), "samples")


def make_color(path):
    w, h = 31, 17
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            # Smooth gradients + a few hard edges: enough AC energy to make
            # IDCT output non-trivial in every block.
            r = (x * 9 + y * 5) & 0xff
            g = (x * 3 + 128 - y * 7) & 0xff
            b = (255 - x * 6 + y * 11) & 0xff
            if x < 8 and y < 8:
                r, g, b = 200, 30, 30
            if x >= 23:
                g = 240
            px[x, y] = (r, g, b)
    img.save(path, "JPEG", quality=88, subsampling="4:2:0")


def make_gray(path):
    w, h = 24, 16
    img = Image.new("L", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (x * 11 + y * 13 + ((x // 8) * 40)) & 0xff
    img.save(path, "JPEG", quality=90)


def make_444(path):
    # 4:4:4 (subsample 0x11): one 8x8 luma block per MCU.
    w, h = 17, 11
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = ((x * 17) & 0xff, (200 - y * 9) & 0xff, (x + y * 13) & 0xff)
    img.save(path, "JPEG", quality=92, subsampling="4:4:4")


def make_422(path):
    # 4:2:2 horizontal (subsample 0x21): two side-by-side luma blocks per MCU.
    w, h = 19, 9
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = ((255 - x * 8) & 0xff, (x * 5 + y * 6) & 0xff, (y * 21) & 0xff)
    img.save(path, "JPEG", quality=92, subsampling="4:2:2")


def main():
    os.makedirs(OUT, exist_ok=True)
    color = os.path.join(OUT, "color_420_31x17.jpg")
    gray = os.path.join(OUT, "gray_24x16.jpg")
    c444 = os.path.join(OUT, "color_444_17x11.jpg")
    c422 = os.path.join(OUT, "color_422_19x9.jpg")
    make_color(color)
    make_gray(gray)
    make_444(c444)
    make_422(c422)
    for p in (color, gray, c444, c422):
        print(p, os.path.getsize(p), "bytes")


if __name__ == "__main__":
    sys.exit(main())
