//
// Sample / test harness for the optional 3-stage post-processing pipeline
// (JPEG_PostProcess in src/JPEGDEC.cpp).
//
// Decodes the 640x480 tulips test image into an RGB8888 framebuffer using
// the unmodified decode path, then measures, for two configurations
// (all three stages on / only the middle chroma stage on):
//   1. block-boundary gradient difference (luma)
//   2. chroma edge error (block-boundary chroma discontinuity)
//   3. whole-image PSNR vs. the raw decode output
//   4. total time of the enabled stages at 640x480
// It also verifies that invalid parameters are rejected with error codes.
//
// Build (from the repo root):
//   g++ -D__LINUX__ -Wall -O2 -I src linux/examples/postprocess/main.cpp src/JPEGDEC.cpp -o /tmp/postprocess_demo -lm
//
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "../../../src/JPEGDEC.h"
#include "../../../test_images/tulips.h" // 640x480 4:2:0 JPEG

// --- Post-processing API (implemented in src/JPEGDEC.cpp) -----------------
// The prototypes/macros are repeated here because this task allows
// modifying only one library file (src/JPEGDEC.cpp), so the public header
// could not be extended. The values must match the ones in JPEGDEC.cpp.
#define JPEG_PP_DEBLOCK 0x1 // stage 1: block-boundary deblocking filter
#define JPEG_PP_CHROMA  0x2 // stage 2: chroma bilinear up-sampling
#define JPEG_PP_SHARPEN 0x4 // stage 3: mild luma sharpening
#define JPEG_PP_ALL     0x7
int JPEG_PostProcess(uint8_t *pFrame, int iWidth, int iHeight, int iPixelType, int iFlags);
// ----------------------------------------------------------------------------

static JPEGDEC jpeg;
static uint8_t fb_raw[640*480*4];  // untouched decode output (reference)
static uint8_t fb_work[640*480*4]; // post-processed copy
static int W, H;

static int JPEGDraw(JPEGDRAW *) { return 1; }

static long micros(void)
{
struct timespec res;
    clock_gettime(CLOCK_MONOTONIC, &res);
    return 1000000L*res.tv_sec + res.tv_nsec/1000;
}

static int luma(const uint8_t *p)
{
    return (77*p[0] + 150*p[1] + 29*p[2] + 128) >> 8;
}

// Metric 1: mean absolute luma gradient across 8x8 block boundaries vs.
// mean gradient of non-boundary neighboring pixels.
static void boundary_gradient(const uint8_t *p, double *pBoundary, double *pInterior)
{
double bs = 0, is = 0;
long bc = 0, ic = 0;
int x, y, d;
    for (y = 0; y < H; y++) {
        for (x = 1; x < W; x++) {
            d = abs(luma(&p[(y*W+x)*4]) - luma(&p[(y*W+x-1)*4]));
            if ((x & 7) == 0) { bs += d; bc++; } else { is += d; ic++; }
        }
    }
    for (y = 1; y < H; y++) {
        for (x = 0; x < W; x++) {
            d = abs(luma(&p[(y*W+x)*4]) - luma(&p[((y-1)*W+x)*4]));
            if ((y & 7) == 0) { bs += d; bc++; } else { is += d; ic++; }
        }
    }
    *pBoundary = bs / bc;
    *pInterior = is / ic;
}

// Metric 2: chroma edge error = mean |dCb|+|dCr| jump across 8x8 block
// boundaries (blocking shows up as chroma steps on the block grid).
static double chroma_edge_error(const uint8_t *p)
{
double err = 0;
long cnt = 0;
int x, y, R0, G0, B0, R1, G1, B1, cb0, cr0, cb1, cr1;
    for (y = 0; y < H; y++) {
        for (x = 1; x < W; x++) {
            if ((x & 7) != 0) continue;
            R0 = p[(y*W+x-1)*4]; G0 = p[(y*W+x-1)*4+1]; B0 = p[(y*W+x-1)*4+2];
            R1 = p[(y*W+x)*4];   G1 = p[(y*W+x)*4+1];   B1 = p[(y*W+x)*4+2];
            cb0 = (-87*R0 - 169*G0 + 256*B0) >> 9; cr0 = (256*R0 - 214*G0 - 42*B0) >> 9;
            cb1 = (-87*R1 - 169*G1 + 256*B1) >> 9; cr1 = (256*R1 - 214*G1 - 42*B1) >> 9;
            err += abs(cb1-cb0) + abs(cr1-cr0); cnt++;
        }
    }
    for (y = 1; y < H; y++) {
        for (x = 0; x < W; x++) {
            if ((y & 7) != 0) continue;
            R0 = p[((y-1)*W+x)*4]; G0 = p[((y-1)*W+x)*4+1]; B0 = p[((y-1)*W+x)*4+2];
            R1 = p[(y*W+x)*4];     G1 = p[(y*W+x)*4+1];     B1 = p[(y*W+x)*4+2];
            cb0 = (-87*R0 - 169*G0 + 256*B0) >> 9; cr0 = (256*R0 - 214*G0 - 42*B0) >> 9;
            cb1 = (-87*R1 - 169*G1 + 256*B1) >> 9; cr1 = (256*R1 - 214*G1 - 42*B1) >> 9;
            err += abs(cb1-cb0) + abs(cr1-cr0); cnt++;
        }
    }
    return err / cnt;
}

// Metric 3: whole-image PSNR of a processed frame vs. the raw decode output
static double psnr_vs_raw(const uint8_t *p)
{
double mse = 0;
long i, n = (long)W*H*3;
    for (i = 0; i < (long)W*H; i++) {
        for (int c = 0; c < 3; c++) {
            double d = (double)p[i*4+c] - (double)fb_raw[i*4+c];
            mse += d*d;
        }
    }
    mse /= n;
    if (mse == 0.0) return 99.0; // identical
    return 10.0 * log10(255.0*255.0 / mse);
}

// Run one configuration: time the enabled stages at 640x480 (average of
// 20 runs on fresh copies) and print the four metrics.
static void run_config(const char *szName, int iFlags)
{
long lTime, lBest = 0x7fffffff, lTotal = 0;
const int iRuns = 20;
double bnd, interior, cerr, psnr;
int rc;

    for (int i = 0; i < iRuns; i++) {
        memcpy(fb_work, fb_raw, sizeof(fb_raw));
        lTime = micros();
        rc = JPEG_PostProcess(fb_work, W, H, RGB8888, iFlags);
        lTime = micros() - lTime;
        if (rc != JPEG_SUCCESS) { printf("  ERROR: JPEG_PostProcess returned %d\n", rc); return; }
        lTotal += lTime;
        if (lTime < lBest) lBest = lTime;
    }
    boundary_gradient(fb_work, &bnd, &interior);
    cerr = chroma_edge_error(fb_work);
    psnr = psnr_vs_raw(fb_work);
    printf("  [%s] flags=0x%x\n", szName, iFlags);
    printf("    block-boundary gradient: boundary=%.3f interior=%.3f difference=%.3f\n",
           bnd, interior, bnd - interior);
    printf("    chroma edge error (boundary |dCb|+|dCr|): %.4f\n", cerr);
    printf("    whole-image PSNR vs raw decode: %.2f dB\n", psnr);
    printf("    post-process time @640x480: avg=%ld us best=%ld us (%d runs)\n\n",
           lTotal/iRuns, lBest, iRuns);
}

int main(void)
{
double bnd, interior, cerr;
int i, rc, iFails = 0;

    // Decode once through the untouched decode path into an RGB8888 framebuffer
    if (!jpeg.openFLASH((const uint8_t *)tulips, sizeof(tulips), JPEGDraw)) {
        printf("openFLASH failed\n"); return 1;
    }
    jpeg.setPixelType(RGB8888);
    jpeg.setFramebuffer(fb_raw);
    if (!jpeg.decode(0, 0, 0)) {
        printf("decode failed, error=%d\n", jpeg.getLastError()); return 1;
    }
    W = jpeg.getWidth(); H = jpeg.getHeight();
    printf("Decoded %dx%d (subsample=0x%02x) into RGB8888 framebuffer\n\n",
           W, H, jpeg.getSubSample());
    jpeg.close();
    if (W != 640 || H != 480) { printf("unexpected image size\n"); return 1; }

    // ---- invalid-parameter checks (buffer must stay untouched) ----
    printf("Parameter validation:\n");
    struct { const char *name; uint8_t *p; int w, h, type, flags, expect; } tests[] = {
        {"NULL frame buffer",  NULL,   640, 480, RGB8888, JPEG_PP_ALL, JPEG_INVALID_PARAMETER},
        {"zero width",         fb_work,  0, 480, RGB8888, JPEG_PP_ALL, JPEG_INVALID_PARAMETER},
        {"negative height",    fb_work,640,  -1, RGB8888, JPEG_PP_ALL, JPEG_INVALID_PARAMETER},
        {"unsupported type",   fb_work,640, 480, RGB565_LITTLE_ENDIAN, JPEG_PP_ALL, JPEG_UNSUPPORTED_FEATURE},
        {"unknown flag bits",  fb_work,640, 480, RGB8888, 0x80,        JPEG_INVALID_PARAMETER},
        {"odd width + chroma", fb_work,641, 480, RGB8888, JPEG_PP_CHROMA, JPEG_INVALID_PARAMETER},
        {"flags=0 (no-op)",    fb_work,640, 480, RGB8888, 0,           JPEG_SUCCESS},
    };
    memset(fb_work, 0xA5, 1024);
    for (i = 0; i < (int)(sizeof(tests)/sizeof(tests[0])); i++) {
        rc = JPEG_PostProcess(tests[i].p, tests[i].w, tests[i].h, tests[i].type, tests[i].flags);
        int ok = (rc == tests[i].expect);
        if (!ok) iFails++;
        printf("  %-20s -> rc=%d (expect %d) %s\n", tests[i].name, rc, tests[i].expect,
               ok ? "PASS" : "FAIL");
    }
    // no-op call must not have modified the buffer
    for (i = 0; i < 1024 && fb_work[i] == 0xA5; i++);
    if (i != 1024) { printf("  no-op modified the buffer! FAIL\n"); iFails++; }
    else printf("  no-op left buffer untouched PASS\n");
    if (iFails) { printf("%d validation test(s) FAILED\n", iFails); return 1; }
    printf("  all validation tests passed\n\n");

    // ---- metrics on the raw decode output (baseline) ----
    boundary_gradient(fb_raw, &bnd, &interior);
    cerr = chroma_edge_error(fb_raw);
    printf("Baseline (raw decode output, no post-processing):\n");
    printf("  block-boundary gradient: boundary=%.3f interior=%.3f difference=%.3f\n",
           bnd, interior, bnd - interior);
    printf("  chroma edge error (boundary |dCb|+|dCr|): %.4f\n\n", cerr);

    // ---- config A: all three stages on ----
    run_config("all 3 stages on", JPEG_PP_ALL);

    // ---- config B: only the middle stage (chroma bilinear up-sampling) ----
    run_config("middle stage only", JPEG_PP_CHROMA);

    return 0;
}
