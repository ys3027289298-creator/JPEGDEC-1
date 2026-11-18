//
// Minimal baseline JPEG encoder (test-sample generator for the strip-decode
// tests). Self-contained, no third-party dependencies.
//
// Supports: 8-bit baseline Huffman, YCbCr 4:4:4 and 4:2:0, quality scaling,
// optional restart markers (DRI/RSTn). Edge MCUs are handled by clamping
// samples to the image border, so any width/height is accepted.
//
#ifndef JPEG_ENC_H
#define JPEG_ENC_H

#include <stdint.h>

#define JPE_SUBSAMPLE_444 0
#define JPE_SUBSAMPLE_420 1

// Encode the w*h*3 RGB image to a baseline JPEG file at 'path'.
// quality: 1..100 (libjpeg-style scaling)
// subsample: JPE_SUBSAMPLE_444 or JPE_SUBSAMPLE_420
// restart_interval: MCUs between RSTn markers, 0 = no restart markers
// Returns 0 on success, -1 on error.
int jpe_encode_file(const char *path, const uint8_t *rgb, int w, int h,
                    int quality, int subsample, int restart_interval);

#endif // JPEG_ENC_H
