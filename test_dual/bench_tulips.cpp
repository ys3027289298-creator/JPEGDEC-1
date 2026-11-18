// Timing-only benchmark on the repository's large 640x480 4:2:0 tulips image.
// Functional correctness is covered by dual_test.cpp on the small samples;
// this file exists because 300-iteration timing on a 500-byte JPEG is pure
// noise, whereas a 56 KB image produces stable, meaningful deltas.
#include <JPEGDEC.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <algorithm>

#ifndef PROGMEM
#define PROGMEM
#endif
#include "../test_images/tulips.h"

#define JPEG_DUAL_OUTPUT   0x00000100
#define JPEG_DUAL_MAGIC    0x44554144u
typedef struct jpeg_dual_plane_tag
{
    uint32_t u32Magic;
    uint8_t *pYPlane;
    int iYPitch;
    int iYWidth;
    int iYHeight;
    int bOutputY;
} JPEG_DUAL_PLANE;

static long micros_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static int sink_cb(JPEGDRAW *d) { (void)d; return 1; }

static long median_of(std::vector<long> &v)
{
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

static long minimum_of(std::vector<long> &v)
{
    // Minimum is the least interfered-with sample and the most stable
    // figure for a fixed-workload decode benchmark on a shared host.
    return *std::min_element(v.begin(), v.end());
}

// One open()+decode() cycle timed on its own; returns microseconds.
static long time_one(int pixel_type, int options, JPEG_DUAL_PLANE *dual)
{
    JPEGDEC j;
    if (!j.openRAM((uint8_t *)tulips, (int)sizeof(tulips), sink_cb))
        return -1;
    j.setPixelType(pixel_type);
    if (dual) j.setUserPointer(dual);
    long t0 = micros_now();
    int rc = j.decode(0, 0, options);
    long us = micros_now() - t0;
    j.close();
    return rc ? us : -1;
}

int main(void)
{
    const int N = 101; // odd so the median is a real sample
    JPEGDEC probe;
    probe.openRAM((uint8_t *)tulips, (int)sizeof(tulips), sink_cb);
    int W = probe.getWidth(), H = probe.getHeight();
    probe.close();
    std::vector<uint8_t> ybuf((size_t)W * H);
    printf("tulips %dx%d 4:2:0, %zu bytes JPEG; Y plane = %d bytes (%.1f KB)\n",
           W, H, sizeof(tulips), W * H, W * H / 1024.0);
    printf("statistics over %d open+decode cycles, microseconds\n", N);

    struct Mode { int pt; int opt; int yout; const char *tag; };
    Mode modes[] = {
        { RGB565_LITTLE_ENDIAN, 0,                              0, "single RGB565" },
        { RGB8888,              0,                              0, "single RGB8888" },
        { EIGHT_BIT_GRAYSCALE,  JPEG_LUMA_ONLY,                 0, "Y-only (LUMA_ONLY)" },
        { RGB565_LITTLE_ENDIAN, JPEG_DUAL_OUTPUT,               1, "dual RGB565 + Y" },
        { RGB8888,              JPEG_DUAL_OUTPUT,               1, "dual RGB8888 + Y" },
        { RGB565_LITTLE_ENDIAN, JPEG_DUAL_OUTPUT,               0, "dual RGB only (bOutputY=0)" },
        { EIGHT_BIT_GRAYSCALE,  JPEG_LUMA_ONLY | JPEG_DUAL_OUTPUT, 1, "Y-only + dual Y plane" },
    };
    double min565 = 0;
    printf("%-28s %9s %9s %10s\n", "mode", "min_us", "median", "min_delta%");
    for (auto &m : modes) {
        std::vector<long> samples;
        JPEG_DUAL_PLANE *dp = NULL;
        JPEG_DUAL_PLANE dual;
        if (m.opt & JPEG_DUAL_OUTPUT) {
            dual = { JPEG_DUAL_MAGIC, m.yout ? ybuf.data() : NULL, W, 0, 0, m.yout };
            dp = &dual;
        }
        for (int i = 0; i < N; i++) {
            long us = time_one(m.pt, m.opt, dp);
            if (us < 0) { printf("%s: decode failed\n", m.tag); return 1; }
            samples.push_back(us);
        }
        long mn = minimum_of(samples);
        long md = median_of(samples);
        if (strcmp(m.tag, "single RGB565") == 0) min565 = (double)mn;
        printf("%-28s %9ld %9ld %+9.1f%%\n", m.tag, mn, md,
               (mn - min565) * 100.0 / min565);
    }
    return 0;
}
