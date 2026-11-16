#!/usr/bin/env python3
# Generates the two test images for the strip-decode test.
# Deterministic synthetic patterns; no network access.
#   small.jpg  -  96x64, 4:4:4 subsampling (8x8 MCU rows)
#   large.jpg  - 800x600, 4:2:0 subsampling (16-pixel MCU rows, clipped last band)
from PIL import Image

def pattern(w, h):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            # smooth gradients + a hard-edged diagonal band + fine checker detail
            r = (x * 255) // (w - 1)
            g = (y * 255) // (h - 1)
            b = ((x + y) * 255) // (w + h - 2)
            if (x - y) % 37 in (0, 1):
                r, g, b = 255 - r, 255 - g, 255 - b
            if (x // 4 + y // 4) % 2:
                b ^= 0x40
            px[x, y] = (r, g, b)
    return img

small = pattern(96, 64)
small.save("small.jpg", quality=90, subsampling=0)   # 4:4:4

large = pattern(800, 600)
large.save("large.jpg", quality=90)                  # 4:2:0 (libjpeg default)

print("wrote small.jpg (96x64, 4:4:4) and large.jpg (800x600, 4:2:0)")
