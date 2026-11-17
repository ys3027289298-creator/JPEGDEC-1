//
// IDCT precision tier test for the JPEGDEC library
//
// Self-contained: generates 4 small test images (smooth / detail / high-freq
// noise / solid color) with the minimal baseline JPEG encoder below, decodes
// them with each IDCT tier and reports PSNR, decode time and max pixel
// difference of the two integer tiers versus the floating-point reference.
// Also verifies that invalid tiers are rejected and that the default
// (original) IDCT path is pixel-identical before/after tier switching.
//
// No third-party dependencies; build & run everything in one go:
//   g++ -O2 -Wall -D__LINUX__ -I../../src idct_tier_test.cpp ../../src/JPEGDEC.cpp -o idct_tier_test -lm
//   ./idct_tier_test
//
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "JPEGDEC.h"

// IDCT tier control API added in src/jpeg.inl
extern "C" int JPEGIDCTSetMode(int iMode);
extern "C" int JPEGIDCTGetMode(void);

// tier values must match the JPEG_IDCT_* defines in src/jpeg.inl
enum { IDCT_DEFAULT = 0, IDCT_FAST = 1, IDCT_ACCURATE = 2, IDCT_FLOAT = 3 };
static const char *sTierNames[] = {"default(orig)", "fast(int Q8)", "accurate(int Q13)", "float(ref)"};

#define IMG_W 128
#define IMG_H 128
#define TIME_ITERS 200

//===========================================================================
// Minimal baseline JPEG encoder (grayscale, standard Annex K tables)
//===========================================================================
static const uint8_t kZigZag[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63};
// standard luminance quantization table (quality 50), natural order
static const uint8_t kQuantLum[64] = {
    16,11,10,16, 24, 40, 51, 61,
    12,12,14,19, 26, 58, 60, 55,
    14,13,16,24, 40, 57, 69, 56,
    14,17,22,29, 51, 87, 80, 62,
    18,22,37,56, 68,109,103, 77,
    24,35,55,64, 81,104,113, 92,
    49,64,78,87,103,121,120,101,
    72,92,95,98,112,100,103, 99};
// standard luminance Huffman tables (JPEG Annex K)
static const uint8_t kDCLumBits[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t kDCLumVals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t kACLumBits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t kACLumVals[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,
    0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,
    0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,
    0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,
    0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
    0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,
    0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,
    0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
    0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,
    0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,
    0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa};

typedef struct { uint8_t *buf; size_t len; uint32_t acc; int nbits; } BitWriter;

static void bw_put(BitWriter *w, uint32_t code, int n)
{
    if (n == 0) return;
    w->acc = (w->acc << n) | (code & ((1u << n) - 1));
    w->nbits += n;
    while (w->nbits >= 8)
    {
        uint8_t b = (uint8_t)(w->acc >> (w->nbits - 8));
        w->buf[w->len++] = b;
        if (b == 0xFF) w->buf[w->len++] = 0x00; // byte stuffing
        w->nbits -= 8;
    }
}
static void bw_flush(BitWriter *w)
{
    if (w->nbits) bw_put(w, (1u << (8 - w->nbits)) - 1, 8 - w->nbits); // pad with 1s
}

static void huff_build(const uint8_t *bits, const uint8_t *vals, int nvals,
                       uint16_t *codeOf, uint8_t *sizeOf)
{
    uint16_t code = 0;
    int k = 0, l, i;
    for (l = 0; l < 16; l++)
    {
        for (i = 0; i < bits[l]; i++)
        {
            codeOf[vals[k]] = code;
            sizeOf[vals[k]] = (uint8_t)(l + 1);
            code++; k++;
        }
        code <<= 1;
    }
    (void)nvals;
}

static int coeff_size(int v) // magnitude category
{
    int a = (v < 0) ? -v : v, n = 0;
    while (a) { n++; a >>= 1; }
    return n;
}

// forward DCT + quantization of one 8x8 block, output in zigzag order
static void fdct_quant_block(const uint8_t *px, int stride, int16_t *zz)
{
    static float cosT[8][8];
    static int bInit = 0;
    float tmp[64];
    int u, v, x, y;
    if (!bInit)
    {
        for (u = 0; u < 8; u++)
            for (x = 0; x < 8; x++)
                cosT[u][x] = cosf((float)((2*x + 1) * u) * 0.19634954084936207f); // *PI/16
        bInit = 1;
    }
    for (u = 0; u < 8; u++)
        for (y = 0; y < 8; y++)
        {
            float s = 0.0f;
            for (x = 0; x < 8; x++)
                s += ((float)px[y*stride + x] - 128.0f) * cosT[u][x];
            tmp[u*8 + y] = s;
        }
    {
        int16_t nat[64];
        for (u = 0; u < 8; u++)
            for (v = 0; v < 8; v++)
            {
                float s = 0.0f, cu = (u == 0) ? 0.70710678f : 1.0f, cv = (v == 0) ? 0.70710678f : 1.0f;
                float f;
                for (y = 0; y < 8; y++)
                    s += tmp[u*8 + y] * cosT[v][y];
                f = 0.25f * cu * cv * s / (float)kQuantLum[u*8 + v];
                nat[u*8 + v] = (int16_t)(f >= 0.0f ? (int)(f + 0.5f) : (int)(f - 0.5f));
            }
        for (u = 0; u < 64; u++)
            zz[u] = nat[kZigZag[u]];
    }
}

static void emit16(uint8_t *out, size_t *pos, uint16_t v)
{
    out[(*pos)++] = (uint8_t)(v >> 8);
    out[(*pos)++] = (uint8_t)(v & 0xff);
}

// encode a WxH (multiple of 8) 8-bit grayscale image as a baseline JFIF stream
static size_t encode_jpeg(const uint8_t *img, int w, int h, uint8_t *out)
{
    size_t pos = 0;
    uint16_t dcCode[256], acCode[256];
    uint8_t dcSize[256], acSize[256];
    BitWriter bw;
    int bx, by, i, prevDC = 0;

    huff_build(kDCLumBits, kDCLumVals, 12, dcCode, dcSize);
    huff_build(kACLumBits, kACLumVals, 162, acCode, acSize);

    emit16(out, &pos, 0xFFD8); // SOI
    emit16(out, &pos, 0xFFE0); // APP0 JFIF
    emit16(out, &pos, 16);
    memcpy(&out[pos], "JFIF\0", 5); pos += 5;
    out[pos++] = 1; out[pos++] = 1; out[pos++] = 0;
    emit16(out, &pos, 1); emit16(out, &pos, 1);
    out[pos++] = 0; out[pos++] = 0;
    emit16(out, &pos, 0xFFDB); // DQT
    emit16(out, &pos, 2 + 1 + 64);
    out[pos++] = 0; // 8-bit precision, table 0
    for (i = 0; i < 64; i++) out[pos++] = kQuantLum[kZigZag[i]];
    emit16(out, &pos, 0xFFC0); // SOF0
    emit16(out, &pos, 8 + 3);
    out[pos++] = 8;
    emit16(out, &pos, (uint16_t)h);
    emit16(out, &pos, (uint16_t)w);
    out[pos++] = 1;    // 1 component (grayscale)
    out[pos++] = 1;    // component id
    out[pos++] = 0x11; // no subsampling
    out[pos++] = 0;    // quant table 0
    emit16(out, &pos, 0xFFC4); // DHT (DC)
    emit16(out, &pos, 2 + 1 + 16 + 12);
    out[pos++] = 0x00;
    memcpy(&out[pos], kDCLumBits, 16); pos += 16;
    memcpy(&out[pos], kDCLumVals, 12); pos += 12;
    emit16(out, &pos, 0xFFC4); // DHT (AC)
    emit16(out, &pos, 2 + 1 + 16 + 162);
    out[pos++] = 0x10;
    memcpy(&out[pos], kACLumBits, 16); pos += 16;
    memcpy(&out[pos], kACLumVals, 162); pos += 162;
    emit16(out, &pos, 0xFFDA); // SOS
    emit16(out, &pos, 6 + 2);
    out[pos++] = 1;
    out[pos++] = 1;    // component id
    out[pos++] = 0x00; // DC/AC table 0
    out[pos++] = 0; out[pos++] = 63; out[pos++] = 0;

    bw.buf = out; bw.len = pos; bw.acc = 0; bw.nbits = 0;
    for (by = 0; by < h; by += 8)
        for (bx = 0; bx < w; bx += 8)
        {
            int16_t zz[64];
            int diff, s, run;
            fdct_quant_block(&img[by*w + bx], w, zz);
            diff = zz[0] - prevDC; prevDC = zz[0];
            s = coeff_size(diff);
            bw_put(&bw, dcCode[s], dcSize[s]);
            if (s) bw_put(&bw, (uint32_t)(diff < 0 ? diff + (1 << s) - 1 : diff), s);
            run = 0;
            for (i = 1; i < 64; i++)
            {
                if (zz[i] == 0) { run++; continue; }
                while (run > 15) { bw_put(&bw, acCode[0xF0], acSize[0xF0]); run -= 16; }
                s = coeff_size(zz[i]);
                bw_put(&bw, acCode[(run << 4) | s], acSize[(run << 4) | s]);
                bw_put(&bw, (uint32_t)(zz[i] < 0 ? zz[i] + (1 << s) - 1 : zz[i]), s);
                run = 0;
            }
            if (run) bw_put(&bw, acCode[0x00], acSize[0x00]); // EOB
        }
    bw_flush(&bw);
    pos = bw.len;
    emit16(out, &pos, 0xFFD9); // EOI
    return pos;
}

//===========================================================================
// Test image generators (deterministic, no external data)
//===========================================================================
static void gen_smooth(uint8_t *img)
{
    int x, y;
    for (y = 0; y < IMG_H; y++)
        for (x = 0; x < IMG_W; x++)
        {
            double v = 128.0 + 90.0*sin(x*0.06)*cos(y*0.05) + 20.0*sin((x+y)*0.02);
            img[y*IMG_W + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
}
static void gen_detail(uint8_t *img)
{
    int x, y;
    for (y = 0; y < IMG_H; y++)
        for (x = 0; x < IMG_W; x++)
        {
            int v = 128 + (int)(40.0*sin(x*0.11) + 30.0*cos(y*0.13));
            if ((x/8 + y/8) & 1) v += 35; else v -= 35; // 8x8 checker
            if (x >= 32 && x < 96 && y >= 32 && y < 96) // textured center box
                v = ((x*x + y*y) % 17 < 8) ? 220 : 30;
            if (x == 64 || y == 64) v = 255; // sharp edges
            img[y*IMG_W + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
}
static void gen_noise(uint8_t *img)
{
    uint32_t s = 12345;
    int i;
    for (i = 0; i < IMG_W*IMG_H; i++)
    {
        s = s*1664525u + 1013904223u; // LCG, deterministic
        img[i] = (uint8_t)(s >> 24);
    }
}
static void gen_solid(uint8_t *img)
{
    memset(img, 180, IMG_W*IMG_H);
}

//===========================================================================
// Decode helpers
//===========================================================================
static JPEGDEC g_jpeg;
static uint8_t g_cap[IMG_W*IMG_H];

static int DrawCapture(JPEGDRAW *p)
{
    int y;
    uint8_t *s = (uint8_t *)p->pPixels; // EIGHT_BIT_GRAYSCALE: 1 byte/pixel
    for (y = 0; y < p->iHeight; y++)
        memcpy(&g_cap[(p->y + y) * IMG_W + p->x], &s[y * p->iWidth], p->iWidthUsed);
    return 1;
}
static int DrawNull(JPEGDRAW *p) { (void)p; return 1; }

static int decode_image(const uint8_t *jpg, size_t len, int capture)
{
    int ok;
    if (!g_jpeg.openRAM((uint8_t *)jpg, (int)len, capture ? DrawCapture : DrawNull))
        return 0;
    g_jpeg.setPixelType(EIGHT_BIT_GRAYSCALE);
    ok = g_jpeg.decode(0, 0, 0);
    g_jpeg.close();
    return ok;
}

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec*1.0e6 + (double)ts.tv_nsec/1.0e3;
}

static double time_decode(const uint8_t *jpg, size_t len)
{
    double t0 = now_us();
    int i;
    for (i = 0; i < TIME_ITERS; i++)
        decode_image(jpg, len, 0);
    return (now_us() - t0) / TIME_ITERS;
}

//===========================================================================
int main(void)
{
    static uint8_t img[IMG_W*IMG_H], jpg[65536];
    static uint8_t cap[4][IMG_W*IMG_H]; // per-tier decoded output
    static const struct { const char *name; void (*gen)(uint8_t *); } kImages[4] = {
        {"smooth", gen_smooth}, {"detail", gen_detail},
        {"noise(hi-freq)", gen_noise}, {"solid", gen_solid}};
    int failures = 0, imgIdx, tier, i;

    printf("=== JPEGDEC IDCT precision tier test ===\n\n");

    // --- invalid tier values must be rejected ---
    printf("[check] invalid precision tiers are rejected:\n");
    {
        int bad[] = {-1, 4, 99, -1000};
        int ok = 1;
        JPEGIDCTSetMode(IDCT_DEFAULT);
        for (i = 0; i < 4; i++)
        {
            int rc = JPEGIDCTSetMode(bad[i]);
            if (rc != -1 || JPEGIDCTGetMode() != IDCT_DEFAULT)
            {
                printf("  FAIL: JPEGIDCTSetMode(%d) returned %d, mode now %d\n",
                       bad[i], rc, JPEGIDCTGetMode());
                ok = 0;
            }
        }
        for (tier = 0; tier <= 3; tier++)
            if (JPEGIDCTSetMode(tier) != 0) { printf("  FAIL: valid tier %d rejected\n", tier); ok = 0; }
        JPEGIDCTSetMode(IDCT_DEFAULT);
        printf("  %s\n\n", ok ? "OK (-1 returned, tier unchanged)" : "FAILED");
        if (!ok) failures++;
    }

    // --- default path must be pixel-identical before/after tier switching ---
    printf("[check] default (original) IDCT path is unchanged:\n");
    {
        static uint8_t capBefore[IMG_W*IMG_H];
        size_t len;
        gen_detail(img);
        len = encode_jpeg(img, IMG_W, IMG_H, jpg);
        JPEGIDCTSetMode(IDCT_DEFAULT);
        decode_image(jpg, len, 1);
        memcpy(capBefore, g_cap, sizeof(g_cap));
        // cycle through all tiers, then back to default
        for (tier = 1; tier <= 3; tier++) { JPEGIDCTSetMode(tier); decode_image(jpg, len, 1); }
        JPEGIDCTSetMode(IDCT_DEFAULT);
        decode_image(jpg, len, 1);
        if (memcmp(capBefore, g_cap, sizeof(g_cap)) == 0)
            printf("  OK (identical output before/after tier switching)\n\n");
        else { printf("  FAIL (default path output changed)\n\n"); failures++; }
    }

    // --- per-image comparison of the integer tiers vs the float reference ---
    for (imgIdx = 0; imgIdx < 4; imgIdx++)
    {
        size_t len;
        double us[4];
        kImages[imgIdx].gen(img);
        len = encode_jpeg(img, IMG_W, IMG_H, jpg);
        printf("=== image: %s (%dx%d grayscale, %u bytes JPEG) ===\n",
               kImages[imgIdx].name, IMG_W, IMG_H, (unsigned)len);
        for (tier = 0; tier < 4; tier++)
        {
            if (JPEGIDCTSetMode(tier) != 0) { printf("  FAIL: cannot set tier %d\n", tier); failures++; }
            if (!decode_image(jpg, len, 1)) { printf("  FAIL: decode error at tier %d\n", tier); failures++; }
            memcpy(cap[tier], g_cap, sizeof(g_cap));
            us[tier] = time_decode(jpg, len);
        }
        printf("  %-16s %10s %12s %14s\n", "tier", "time(us)", "PSNR vs float", "maxdiff vs float");
        for (tier = 0; tier < 4; tier++)
        {
            double mse = 0.0, psnr;
            int maxdiff = 0;
            for (i = 0; i < IMG_W*IMG_H; i++)
            {
                int d = (int)cap[tier][i] - (int)cap[IDCT_FLOAT][i];
                mse += (double)d * d;
                if (d < 0) d = -d;
                if (d > maxdiff) maxdiff = d;
            }
            mse /= (IMG_W*IMG_H);
            if (mse == 0.0)
            {
                printf("  %-16s %10.1f %12s %14d\n", sTierNames[tier], us[tier],
                       (tier == IDCT_FLOAT) ? "(reference)" : "inf (identical)", maxdiff);
            }
            else
            {
                psnr = 10.0 * log10(255.0*255.0 / mse);
                printf("  %-16s %10.1f %11.2f dB %14d\n", sTierNames[tier], us[tier], psnr, maxdiff);
                // sanity gates for the two integer tiers
                if ((tier == IDCT_FAST || tier == IDCT_ACCURATE) && (psnr < 30.0 || maxdiff > 24))
                {
                    printf("  FAIL: tier %s outside sanity bounds (PSNR<30dB or maxdiff>24)\n", sTierNames[tier]);
                    failures++;
                }
            }
        }
        // the accurate tier should track the float reference at least as well as the fast tier
        {
            double mseF = 0.0, mseA = 0.0;
            for (i = 0; i < IMG_W*IMG_H; i++)
            {
                int dF = (int)cap[IDCT_FAST][i] - (int)cap[IDCT_FLOAT][i];
                int dA = (int)cap[IDCT_ACCURATE][i] - (int)cap[IDCT_FLOAT][i];
                mseF += (double)dF*dF; mseA += (double)dA*dA;
            }
            if (mseA > mseF * 1.05 + 1e-9)
            {
                printf("  FAIL: accurate tier MSE (%.2f) worse than fast tier (%.2f)\n", mseA, mseF);
                failures++;
            }
        }
        printf("\n");
    }
    JPEGIDCTSetMode(IDCT_DEFAULT); // leave the library in its default state
    printf("=== %s ===\n", failures ? "FAILED" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}
