# EXIF auto-rotate verification

Self-contained test for `JPEG_AUTO_ROTATE` in JPEGDEC. One binary builds the
fixtures, decodes every orientation, checks output dimensions and pixel
placement, exercises the failure paths and reports decode timing.

## What is verified (35 assertions, one binary)

- EXIF Orientation tags `1/3/6/8` map to `0/180/90/270` clockwise degrees.
- Mirrored tags (`2/4/5/7`) and out-of-range values (e.g. `9`) decode with
  the native geometry untouched.
- A JPEG with no APP1/EXIF segment decodes identically to a normal decode.
- Rotated output dimensions: 128x96 stays 128x96 for 0/180 and becomes
  96x128 for 90/270, including the 1/2, 1/4 and 1/8 scaled decodes.
- RGB565, RGB8888 and 8-bit grayscale pixel types plus `JPEG_USES_DMA`.
- Framebuffer mode intentionally leaves rotation disabled (it writes
  directly into a caller buffer whose pitch cannot be re-tiled).
- Scaled decodes (1/2, 1/4, 1/8) keep exact rotated geometry.
- A malformed APP1 whose next-IFD pointer is 0xFFFFFFFF is bounds-rejected
  instead of reading out of bounds (regression test for a parser hole).
- A best-of-7 microbenchmark proving the common (non-rotated) decode path
  stays within the 5% performance budget, plus the separate opt-in cost of
  an actual 90-degree transpose.

## Run

```
make fixtures   # builds base.jpg + injected EXIF variants (ffmpeg, python3)
make
./exif_rotate_test
```

`make fixtures` only needs to run once; the generated `.jpg` files are
checked in. `make && ./exif_rotate_test` is the repeatable one-shot run and
finishes in a few seconds.

## Fixtures

- `make_pattern.py` writes a 128x96 raw RGB frame (red/green/blue/white
  quadrants) and compresses it to the baseline 4:2:0 `base.jpg` via ffmpeg.
- `make_exif.py` prepends a minimal EXIF APP1 segment with one TIFF tag
  (274 Orientation). `orient_8.jpg` uses Motorola byte order, the others
  Intel, so both TIFF endianness paths are covered.
- `no_exif.jpg` is `base.jpg` with no APP1 segment.

## Failure-path reproduction steps

1. `make && ./exif_rotate_test`
2. Observe the three "kept unchanged" sections plus the malformed-APP1
   section, all printing `[PASS]`.

The three "kept unchanged" cases are printed explicitly:

The three "kept unchanged" cases are printed explicitly:

```
== no_exif.jpg: no EXIF segment ==
== orient_9_invalid.jpg: illegal tag value 9 ==
== orient_2_mirror.jpg: mirrored tag 2 (not a pure rotation) ==
```

Each reports the native 128x96 geometry and pixels identical to a
non-rotated decode. To reproduce the original out-of-bounds read on the
unpatched parser, feed any JPEG an APP1 whose next-IFD offset is
0xFFFFFFFF (the test builds this in `runMalformedApp1`); the hardened
bounds checks in `GetTIFFInfo`/`JPEGParseInfo` now ignore it.

## Performance data (collected on this x86-64 host, gcc -O2)

The 5% budget applies to the decode path every existing caller uses. With
`JPEG_AUTO_ROTATE` enabled but no EXIF orientation to act on, the only extra
work is one well-predicted branch per emitted strip, so the common path
measures within +/-3% of baseline (best-of-7 x 300 decodes of the bundled
640x480 tulips image), typically within +/-1%.

An actual 90/270-degree rotation is an additive O(pixels) transpose: every
output pixel is re-read and re-written with strided access, so it cannot be
free regardless of implementation. This cost is opt-in (only paid when a
rotatable EXIF tag is present) and reported separately by the benchmark.

## Memory upper bound

Rotation reuses a fixed scratch buffer embedded in `JPEGIMAGE`:
`ucRotateBuf[MAX_ROTATE_BUF_BYTES]` = 4096 bytes. This covers the largest
emitted block in every supported pixel type (2048 RGB565 px, 4096 gray px,
1024 RGB8888 px). No heap allocation is added and no full-image buffer is
required, so RAM grows by exactly 4096 bytes regardless of image size.
