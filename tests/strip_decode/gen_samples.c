//
// Generates the three JPEG test samples used by the strip-decode test:
//   big_4k_420.jpg   - 3840x2160, 4:2:0, no restart markers ("large image")
//   small_444.jpg    - 163x121 (odd size), 4:4:4, no restart markers ("small")
//   restart_420.jpg  - 1920x1080, 4:2:0, restart interval 8 ("restart markers")
//
// Image content is synthetic and deterministic: gradients, a checkerboard
// band, solid circles and a pseudo-random noise patch (gives the entropy
// coder realistic work). No external assets or libraries needed.
//
#include "jpeg_enc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

static void make_image(uint8_t *rgb, int w, int h, uint32_t seed)
{
    int x, y;
    rng_state = seed;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t *p = &rgb[((size_t)y * w + x) * 3];
            int r = (x * 255) / (w > 1 ? w - 1 : 1);
            int g = (y * 255) / (h > 1 ? h - 1 : 1);
            int b = ((x + y) * 255) / ((w + h - 2) > 0 ? w + h - 2 : 1);
            // checkerboard band
            if (y >= h / 4 && y < h / 4 + h / 8 && (((x >> 3) + (y >> 3)) & 1)) {
                r = 255 - r; g = 255 - g; b = 255 - b;
            }
            // a few solid circles
            {
                static const int cx[3] = {0, 1, 2};
                int k;
                for (k = 0; k < 3; k++) {
                    int ccx = w * (2 + 3 * k) / 10, ccy = h * (3 + 2 * k) / 8;
                    int rad = (w < h ? w : h) / 10;
                    int dx = x - ccx, dy = y - ccy;
                    if (dx * dx + dy * dy < rad * rad) {
                        r = (k * 90 + 40) & 0xFF;
                        g = (k * 60 + 160) & 0xFF;
                        b = (k * 120 + 20) & 0xFF;
                    }
                }
                (void)cx;
            }
            // deterministic noise patch
            if (x >= w / 2 && x < w / 2 + w / 4 && y >= h / 2 && y < h / 2 + h / 4) {
                int n = (int)(rnd() & 0x7F) - 64;
                r += n; g += n; b += n;
            }
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b;
        }
    }
}

static int gen_one(const char *dir, const char *name, int w, int h,
                   int quality, int subsample, int restart)
{
    char path[512];
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    FILE *f;
    long size;
    int rc;
    if (!rgb) { fprintf(stderr, "oom\n"); return -1; }
    make_image(rgb, w, h, 12345u + (uint32_t)w);
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    rc = jpe_encode_file(path, rgb, w, h, quality, subsample, restart);
    free(rgb);
    if (rc) { fprintf(stderr, "encode failed for %s\n", path); return -1; }
    f = fopen(path, "rb");
    fseek(f, 0, SEEK_END); size = ftell(f); fclose(f);
    printf("  %-24s %4dx%-4d q%d %s rst=%d  %8ld bytes\n", name, w, h, quality,
           subsample == JPE_SUBSAMPLE_420 ? "4:2:0" : "4:4:4", restart, size);
    return 0;
}

int main(int argc, char *argv[])
{
    const char *dir = (argc > 1) ? argv[1] : "samples";
    int rc = 0;
    printf("generating samples into %s/\n", dir);
    rc |= gen_one(dir, "big_4k_420.jpg",  3840, 2160, 80, JPE_SUBSAMPLE_420, 0);
    rc |= gen_one(dir, "small_444.jpg",    163,  121, 80, JPE_SUBSAMPLE_444, 0);
    rc |= gen_one(dir, "restart_420.jpg", 1920, 1080, 80, JPE_SUBSAMPLE_420, 8);
    if (rc == 0) printf("all samples generated\n");
    return rc ? 1 : 0;
}
