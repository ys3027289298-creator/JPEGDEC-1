# Strip decoding with a row-reuse buffer (peak heap: 16.6 MB -> 141 KB @ 4K)

Decoding a 4K image the traditional way — one full-image framebuffer that
every `JPEGDraw` block is copied into — needs `W*H*bpp` bytes of heap
(3840x2160 RGB565 = 15.8 MiB; 8-bit grayscale = 7.9 MiB), which does not fit
on the target device. This directory implements **strip decoding**: the image
is assembled one MCU-row band at a time into a single reused strip buffer
(`W * stripRows * bpp`, stripRows = 16 for 4:2:0, 8 otherwise) and each
completed strip is handed to a consumer callback. Peak heap drops to
O(width) instead of O(width*height).

The JPEGDEC library itself already streams MCU rows through its draw
callback and never allocates, so **no library file is modified** — the fix
is the decode-side buffering strategy, implemented here as a reusable
header plus a measured test.

## Files

| file | what it is |
|---|---|
| `strip_decode.h`   | `JpegStripDecoder`: drop-in strip decoder on top of the public JPEGDEC API (row-reuse buffer, per-strip consumer callback, POSIX fd streaming so the compressed file is never fully resident) |
| `main.cpp`         | harness: baseline (full framebuffer) vs strip, per sample |
| `tracked_alloc.cpp`| allocator instrumentation (`--wrap=malloc/...` + operator new override), tracks live/peak heap bytes |
| `jpeg_enc.c/.h`    | tiny self-contained baseline JPEG encoder (4:4:4/4:2:0, restart markers) used to synthesize samples — no third-party code |
| `gen_samples.c`    | deterministic synthetic image content, writes the 3 samples |
| `dump_gray.c`      | luma dump tool used to cross-check the generated JPEGs against ffmpeg |
| `run.sh`           | one-shot: generate samples, build, run comparison, run repo tests |

## Samples (generated, deterministic)

| file | class | size |
|---|---|---|
| `big_4k_420.jpg`   | large image | 3840x2160, 4:2:0, q80 (~475 KB) |
| `small_444.jpg`    | small image | 163x121 (odd size exercises edge clipping), 4:4:4 |
| `restart_420.jpg`  | restart markers | 1920x1080, 4:2:0, RST every 8 MCUs |

## Measured results (x86-64, -O2, best of N reps; peak = wrapped-allocator high water)

| sample | peak baseline | peak strip | t baseline | t strip | slowdown | pixels |
|---|---|---|---|---|---|---|
| large 3840x2160 4:2:0  | 16,607,284 B | **141,412 B** | ~32.4 ms | ~31.7 ms | -1..-3% | identical |
| small 163x121 4:4:4    |     57,454 B |  **20,664 B** |  118 us  |  118 us  | 0..+1%   | identical |
| restart 1920x1080 rst8 |  4,165,416 B |  **79,704 B** |  ~9.2 ms |  ~9.0 ms | -1..-3%  | identical |

Targets: strip peak <= 512 KB (achieved: worst case 138 KiB, 3.6x headroom),
slowdown <= 5% (achieved: parity or slightly faster — the reused strip stays
cache-hot while the 16 MB framebuffer does not), pixel-identical output
(per-strip CRC32 compared between the two modes for every sample).

Correctness of the generated JPEGs themselves was cross-validated by
decoding them with ffmpeg and comparing against JPEGDEC's luma output:
mean abs diff 0.26-0.51 LSB (IDCT rounding only), same as for the repo's
own `demo.jpg`.

## Run

```sh
sh run.sh   # builds into ./build, samples into ./samples, runs everything
```

The script also builds and runs the repo's existing Linux examples
(`jpeg_perf_test`, `c_cmdline` converting `demo.jpg` to BMP) as a
regression check. No repo build files are touched.
