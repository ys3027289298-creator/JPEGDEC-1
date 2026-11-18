//
// Strip-decode vs full-framebuffer decode comparison harness.
//
// For each sample JPEG it runs:
//   BASELINE - the traditional pattern: one full-image framebuffer in heap,
//              every JPEGDraw block memcpy'ed into it.
//   STRIP    - JpegStripDecoder: one reused strip buffer (one MCU row tall),
//              strips handed to a consumer as they complete.
//
// Both paths stream the file through identical POSIX fd callbacks and use
// the same unmodified JPEGDEC library. Heap is instrumented with a wrapped
// allocator (tracked_alloc.c); peak live bytes are recorded per decode.
// Correctness: per-strip CRC32 of the decoded pixels must be identical
// between the two modes. Speed: decode() time, best of N reps per mode.
//
#include <JPEGDEC.h>
#include "strip_decode.h"
#include "tracked_alloc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

// ---------------------------------------------------------------- CRC32
static uint32_t crc_table[256];
static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
}
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ---------------------------------------------------------------- timing
static long micros(void)
{
    struct timespec res;
    clock_gettime(CLOCK_MONOTONIC, &res);
    return 1000000L * res.tv_sec + res.tv_nsec / 1000;
}

// ------------------------------------------------------- file callbacks
static void *fOpen(const char *name, int32_t *size)
{
    int fd = open(name, O_RDONLY);
    if (fd < 0) return NULL;
    *size = (int32_t)lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    return (void *)(intptr_t)fd;
}
static void fClose(void *h) { close((int)(intptr_t)h); }
static int32_t fRead(JPEGFILE *f, uint8_t *buf, int32_t len)
{
    return (int32_t)read((int)(intptr_t)f->fHandle, buf, len);
}
static int32_t fSeek(JPEGFILE *f, int32_t pos)
{
    return (int32_t)lseek((int)(intptr_t)f->fHandle, pos, SEEK_SET);
}

// ------------------------------------------------------------- baseline
typedef struct {
    uint16_t *fb;      // full framebuffer
    int w, h, stripRows;
    int curStripY;     // strip currently being filled (-1 = none)
    uint32_t *crcs;    // one CRC per strip
} BaseCtx;

// CRC one assembled strip straight from the framebuffer (row-major,
// contiguous because the framebuffer pitch equals the image width).
static void baseCrcStrip(BaseCtx *c, int stripY)
{
    int h = c->h - stripY;
    if (h > c->stripRows) h = c->stripRows;
    c->crcs[stripY / c->stripRows] =
        crc32_update(c->crcs[stripY / c->stripRows],
                     (const uint8_t *)&c->fb[(size_t)stripY * c->w],
                     (size_t)c->w * h * 2);
}

static int baseDraw(JPEGDRAW *p)
{
    BaseCtx *c = (BaseCtx *)p->pUser;
    if (p->y != c->curStripY) { // strip complete -> checksum it
        if (c->curStripY >= 0) baseCrcStrip(c, c->curStripY);
        c->curStripY = p->y;
    }
    for (int r = 0; r < p->iHeight; r++) {
        uint16_t *dst = &c->fb[(size_t)(p->y + r) * c->w + p->x];
        const uint16_t *src = &p->pPixels[(size_t)r * p->iWidth];
        memcpy(dst, src, (size_t)p->iWidthUsed * 2);
    }
    return 1;
}

static void baseFlush(BaseCtx *c) // checksum the final strip
{
    if (c->curStripY >= 0) { baseCrcStrip(c, c->curStripY); c->curStripY = -1; }
}

// ---------------------------------------------------------------- strip
typedef struct {
    int stripRows;
    uint32_t *crcs;
} StripCtx;

static void stripConsume(int y, int h, const uint16_t *px, int w, void *user)
{
    StripCtx *c = (StripCtx *)user;
    int si = y / c->stripRows;
    c->crcs[si] = crc32_update(c->crcs[si], (const uint8_t *)px,
                               (size_t)w * h * 2);
}

// ---------------------------------------------------------------- driver
typedef struct {
    const char *name;
    const char *file;
    int reps;
} Sample;

static int strip_rows_of(JPEGDEC *dec)
{
    int ss = dec->getSubSample();
    return (ss == 0x22 || ss == 0x12) ? 16 : 8;
}

#define PEAK_TARGET (512 * 1024)
#define MAX_SLOWDOWN_PCT 5.0

int main(int argc, char *argv[])
{
    const char *dir = (argc > 1) ? argv[1] : "samples";
    char path[512];
    const Sample samples[] = {
        {"large 3840x2160 4:2:0",        "big_4k_420.jpg",  5},
        {"small 163x121 4:4:4",          "small_444.jpg",   50},
        {"restart 1920x1080 4:2:0 rst8", "restart_420.jpg", 5},
    };
    const int nSamples = sizeof(samples) / sizeof(samples[0]);
    int failures = 0;

    crc_init();
    printf("%-30s | %-14s %-14s | %-9s %-9s %-7s | %s\n",
           "sample", "peak base", "peak strip", "t base", "t strip", "slow", "pixels");
    printf("-------------------------------+-----------------"
           "-----------------+--------------------------+--------\n");

    for (int s = 0; s < nSamples; s++) {
        snprintf(path, sizeof(path), "%s/%s", dir, samples[s].file);
        int W = 0, H = 0, stripRows = 0, nStrips = 0;
        long bestBase = 1L << 60, bestStrip = 1L << 60;
        size_t peakBase = 0, peakStrip = 0;
        uint32_t *crcBase = NULL, *crcStrip = NULL;
        int decodeOK = 1;

        // ---- baseline: full framebuffer
        for (int rep = 0; rep < samples[s].reps; rep++) {
            ta_reset();
            JPEGDEC *dec = new JPEGDEC;
            if (!dec->open(path, fOpen, fClose, fRead, fSeek, baseDraw)) {
                printf("ERROR: cannot open %s\n", path); decodeOK = 0; delete dec; break;
            }
            W = dec->getWidth(); H = dec->getHeight();
            stripRows = strip_rows_of(dec);
            nStrips = (H + stripRows - 1) / stripRows;
            BaseCtx ctx;
            ctx.w = W; ctx.h = H; ctx.stripRows = stripRows;
            ctx.curStripY = -1;
            ctx.fb = (uint16_t *)malloc((size_t)W * H * 2);
            ctx.crcs = (uint32_t *)calloc(nStrips, sizeof(uint32_t));
            dec->setUserPointer(&ctx);
            long t0 = micros();
            int rc = dec->decode(0, 0, 0);
            baseFlush(&ctx); // inside the timed region, like the strip path
            long dt = micros() - t0;
            if (!rc) { printf("ERROR: baseline decode failed (%d)\n", dec->getLastError()); decodeOK = 0; }
            if (dt < bestBase) bestBase = dt;
            if (ta_peak() > peakBase) peakBase = ta_peak();
            if (rep == 0) { // keep the reference CRCs
                crcBase = (uint32_t *)malloc((size_t)nStrips * 4);
                memcpy(crcBase, ctx.crcs, (size_t)nStrips * 4);
            }
            free(ctx.crcs); free(ctx.fb);
            dec->close(); delete dec;
        }
        // ---- strip: reused row buffer
        for (int rep = 0; rep < samples[s].reps && decodeOK; rep++) {
            ta_reset();
            JpegStripDecoder *sd = new JpegStripDecoder;
            StripCtx sctx;
            if (!sd->open(path, stripConsume, &sctx)) {
                printf("ERROR: cannot open %s (strip)\n", path); decodeOK = 0; delete sd; break;
            }
            sctx.stripRows = sd->stripRows();
            sctx.crcs = (uint32_t *)calloc((size_t)nStrips, sizeof(uint32_t));
            long t0 = micros();
            int rc = sd->decode();
            long dt = micros() - t0;
            if (!rc) { printf("ERROR: strip decode failed (%d)\n", sd->decoder()->getLastError()); decodeOK = 0; }
            if (dt < bestStrip) bestStrip = dt;
            if (ta_peak() > peakStrip) peakStrip = ta_peak();
            if (rep == 0) {
                crcStrip = (uint32_t *)malloc((size_t)nStrips * 4);
                memcpy(crcStrip, sctx.crcs, (size_t)nStrips * 4);
            }
            free(sctx.crcs);
            sd->close(); delete sd;
        }

        // ---- verdict
        int pixelsOK = decodeOK && crcBase && crcStrip &&
                       memcmp(crcBase, crcStrip, (size_t)nStrips * 4) == 0;
        double slow = (bestBase > 0) ? 100.0 * (double)(bestStrip - bestBase) / (double)bestBase : 0;
        int memOK = peakStrip <= PEAK_TARGET;
        int speedOK = slow <= MAX_SLOWDOWN_PCT;
        if (!pixelsOK || !memOK || !speedOK) failures++;

        printf("%-30s | %10zu B  %10zu B  | %7ld us  %7ld us  %+5.1f%% | %s%s%s\n",
               samples[s].name, peakBase, peakStrip, bestBase, bestStrip, slow,
               pixelsOK ? "identical" : "MISMATCH",
               memOK ? "" : " [peak>512KB]",
               speedOK ? "" : " [too slow]");
        if (crcBase && crcStrip && !pixelsOK) {
            for (int i = 0; i < nStrips; i++)
                if (crcBase[i] != crcStrip[i])
                    printf("    strip %4d: base %08x != strip %08x\n", i, crcBase[i], crcStrip[i]);
        }
        free(crcBase); free(crcStrip);
    }

    printf("\ntargets: strip peak <= %d KB, slowdown <= %.0f%%, pixels identical\n",
           PEAK_TARGET / 1024, MAX_SLOWDOWN_PCT);
    printf(failures ? "RESULT: FAIL (%d sample(s) off target)\n" : "RESULT: PASS\n", failures);
    return failures ? 1 : 0;
}
