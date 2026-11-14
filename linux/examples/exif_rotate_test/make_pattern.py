#!/usr/bin/env python3
# Build the baseline 128x96 RGB test JPEG (4:2:0) with four solid-color
# quadrants: red (TL), green (TR), blue (BL), white (BR).
# Pure stdlib for the raw frame; ffmpeg only for JPEG compression.
import subprocess

W, H = 128, 96
COLORS = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]

with open("pattern.rgb", "wb") as f:
    for y in range(H):
        for x in range(W):
            qid = (0 if y < H // 2 else 2) + (0 if x < W // 2 else 1)
            f.write(bytes(COLORS[qid]))

subprocess.run([
    "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
    "-f", "rawvideo", "-pix_fmt", "rgb24",
    "-video_size", f"{W}x{H}", "-i", "pattern.rgb",
    "-pix_fmt", "yuvj420p", "-q:v", "4", "base.jpg",
], check=True)
print("wrote base.jpg (128x96, 4:2:0, RGB quadrants)")
