# JPEGDEC dual-output (packed RGB + independent Y plane, one decode)

## What was added

A new decode option, `JPEG_DUAL_OUTPUT` (bit `0x00000100`), implemented entirely
inside `src/jpeg.inl`. No build script, public header or existing function
signature was changed.

When the option is set, one decode produces:

1. the normal packed pixel output delivered through the existing draw callback
   (RGB565 LE/BE or RGB8888), **byte-for-byte identical** to a plain decode, and
2. an optional independent, contiguous Y (luma) plane written into an
   application buffer.

Both outputs come from the **same IDCT result** held in the decoder's `sMCUs`
working area. The Huffman stream is parsed once and every block is IDCT'd once;
the Y plane is a straight copy of the luma bytes the color path already
produced. There is no second decode.

## Configuration

The option is passed in the existing `iOptions` argument of
`JPEGDEC::decode(x, y, iOptions)` / `JPEG_decode()`. The plane layout is
described by a descriptor handed over through the existing
`setUserPointer()` / `pUser` slot (the draw callback's `pUser` still points at
the same address):

```cpp
// Mirrors the definition in src/jpeg.inl; copy it into the application.
#define JPEG_DUAL_OUTPUT 0x00000100
#define JPEG_DUAL_MAGIC  0x44554144u  // 'DUAL'
struct JPEG_DUAL_PLANE {
    uint32_t u32Magic;   // always JPEG_DUAL_MAGIC
    uint8_t *pYPlane;    // destination buffer (may be NULL when bOutputY == 0)
    int iYPitch;         // bytes per row; 0 selects tightly packed (width)
    int iYWidth;         // filled by the library
    int iYHeight;        // filled by the library
    int bOutputY;        // 1 = write Y plane, 0 = packed output only
};
```

Typical use:

```cpp
JPEG_DUAL_PLANE dual = { JPEG_DUAL_MAGIC, y_buffer, /*pitch=*/width, 0, 0, 1 };
jpeg.setUserPointer(&dual);
jpeg.decode(0, 0, JPEG_DUAL_OUTPUT);          // RGB565 callback + Y plane
```

Modes:

| Goal                        | pixel type / options                                  |
|-----------------------------|-------------------------------------------------------|
| packed RGB **and** Y        | RGB565/RGB8888 + `JPEG_DUAL_OUTPUT`, `bOutputY = 1`   |
| packed RGB only             | RGB565/RGB8888 + `JPEG_DUAL_OUTPUT`, `bOutputY = 0`   |
| Y only (no color work)      | `EIGHT_BIT_GRAYSCALE` + `JPEG_LUMA_ONLY \| JPEG_DUAL_OUTPUT` |
| plain Y only (old behavior) | `EIGHT_BIT_GRAYSCALE` + `JPEG_LUMA_ONLY`              |

## Validation / error reporting

All layout problems are rejected up front with `decode()` returning 0 and
`getLastError() == JPEG_INVALID_PARAMETER`:

- `pUser` NULL or descriptor magic mismatch;
- `bOutputY == 1` with a NULL `pYPlane`;
- `bOutputY` not 0/1;
- negative pitch, or pitch smaller than the image width (pitch 0 = packed);
- dual output combined with any `JPEG_SCALE_*` downscale (the independent plane
  is defined for full-resolution luma only);
- dual output combined with a dithered pixel type.

Right/bottom MCU padding of odd-sized images is clipped to the real dimensions,
and stride padding bytes are never touched.

## Tests in this directory (no third-party library added to the repo)

- `gen_samples.py` - builds two tiny self-made JPEGs: a 31x17 color 4:2:0
  image (odd size, edge clipping) and a 24x16 grayscale image. Pillow is used
  only as a host-side generator; nothing is linked into the decoder.
- `dual_test.cpp` - per-pixel consistency (dual RGB vs single-output RGB;
  dual Y vs `JPEG_LUMA_ONLY` grayscale; Y-only mode), stride/padding checks,
  all invalid-parameter cases, and small-image timing.
- `bench_tulips.cpp` - stable median timing on the repo's own 640x480 4:2:0
  `test_images/tulips.h`.
- `run.sh` - one command: generates samples, builds and runs the suite against
  the modified library, builds the **pristine HEAD library** in a scratch dir
  and byte-compares every single-output reference dump (proving the original
  single-output path is unchanged), then builds/runs the repo's existing
  `jpeg_perf_test` example.

```sh
cd test_dual && ./run.sh
```

## Memory footprint

The feature adds no fields to `JPEGIMAGE` (still 17944 bytes on this 64-bit
build). The only extra memory is the application-provided Y plane:
`width * height` bytes packed, or `pitch * height` with a custom stride.
The descriptor itself is 32 bytes (64-bit) / 20 bytes (32-bit), application
allocated.
