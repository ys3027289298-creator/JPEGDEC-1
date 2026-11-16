//
// Strip (banded) decode test for JPEGDEC
//
// Verifies, for a small and a large image:
//  - peak heap memory of strip decoding vs whole-image decoding
//  - number of strip callbacks (== ceil(height / strip_height))
//  - pixel-exact agreement with the whole-image decode() path
//  - error reporting for invalid strip heights / NULL callback
//
// Heap is measured by wrapping malloc/free (link with -Wl,--wrap=malloc,--wrap=free).
//
#include <JPEGDEC.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ---------------- heap tracking (via --wrap=malloc/--wrap=free) ----------------
static size_t g_heapCur, g_heapPeak;
extern "C" {
    void *__real_malloc(size_t n);
    void __real_free(void *p);
    void *__wrap_malloc(size_t n) {
        // keep returned pointers 16-byte aligned (max_align_t on x86-64)
        size_t *p = (size_t *)__real_malloc(n + 2 * sizeof(size_t));
        if (!p) return NULL;
        *p = n;
        g_heapCur += n;
        if (g_heapCur > g_heapPeak) g_heapPeak = g_heapCur;
        return (void *)(p + 2);
    }
    void __wrap_free(void *ptr) {
        if (!ptr) return;
        size_t *p = ((size_t *)ptr) - 2;
        g_heapCur -= *p;
        __real_free(p);
    }
}
static void heapReset() { g_heapCur = g_heapPeak = 0; }

// ---------------- test state ----------------
static uint8_t *g_pRef;    // whole-image reference pixels
static uint8_t *g_pReasm;  // reassembled strip output
static int g_width, g_height, g_bpp; // bytes per pixel of current test
static int g_cbCount;
static int g_pixelType;
static int g_nextY;        // expected y of next strip (ordering check)
static int g_orderOK;
static int g_failures;
static FILE *g_outFile;    // when set, strips are written to disk as they arrive

static int DummyDraw(JPEGDRAW *pDraw) { (void)pDraw; return 1; }

static int StripCB(JPEGSTRIP *pStrip) {
    g_cbCount++;
    if (pStrip->y != g_nextY) g_orderOK = 0;   // strips must arrive top-to-bottom, contiguous
    g_nextY = pStrip->y + pStrip->iHeight;
    if (pStrip->iWidth != g_width || pStrip->iPitch != g_width * g_bpp) g_orderOK = 0;
    if (g_outFile) // the "small-RAM device" use case: land each strip on disk immediately
        fwrite(pStrip->pPixels, 1, (size_t)pStrip->iHeight * pStrip->iPitch, g_outFile);
    for (int r = 0; r < pStrip->iHeight; r++)
        memcpy(g_pReasm + ((size_t)(pStrip->y + r) * g_width) * g_bpp,
               pStrip->pPixels + (size_t)r * pStrip->iPitch,
               (size_t)g_width * g_bpp);
    return 1; // continue
}

static uint8_t *readFile(const char *szName, long *pSize) {
    char szPath[256];
    snprintf(szPath, sizeof(szPath), "%s/%s", TESTDIR, szName);
    FILE *f = fopen(szPath, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", szPath); g_failures++; return NULL; }
    fseek(f, 0, SEEK_END);
    *pSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = (uint8_t *)malloc(*pSize);
    if (fread(p, 1, *pSize, f) != (size_t)*pSize) { fclose(f); free(p); return NULL; }
    fclose(f);
    return p;
}

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); g_failures++; } \
} while (0)

// Run one strip-decode case and compare against the whole-image reference.
// Returns peak heap used by decodeStrips().
static size_t runStripCase(JPEGDEC *jpeg, uint8_t *pJPG, long lSize, int iStripH,
                           int iOptions, int *pCbCount, int *pExpectOK) {
    size_t peak = 0;
    int rc;
    jpeg->openRAM(pJPG, lSize, DummyDraw);
    if (g_pixelType) jpeg->setPixelType(g_pixelType);
    g_cbCount = 0; g_nextY = 0; g_orderOK = 1;
    memset(g_pReasm, 0xA5, (size_t)g_width * g_height * g_bpp);
    heapReset();
    rc = jpeg->decodeStrips(iStripH, StripCB, NULL, iOptions);
    peak = g_heapPeak;
    *pCbCount = g_cbCount;
    *pExpectOK = rc && g_orderOK;
    jpeg->close();
    return peak;
}

static void testImage(const char *szName, int iPixelType, const int *pStripHeights, int nCases) {
    long lSize;
    uint8_t *pJPG = readFile(szName, &lSize);
    if (!pJPG) return;
    JPEGDEC jpeg;
    size_t peakWhole, peakStrip;
    int w, h;
    g_pixelType = iPixelType;

    jpeg.openRAM(pJPG, lSize, DummyDraw);
    if (iPixelType) jpeg.setPixelType(iPixelType);
    w = jpeg.getWidth(); h = jpeg.getHeight();
    g_width = w; g_height = h;
    g_bpp = (iPixelType == RGB8888) ? 4 : (iPixelType == EIGHT_BIT_GRAYSCALE) ? 1 : 2;
    g_pRef = (uint8_t *)malloc((size_t)w * h * g_bpp);
    g_pReasm = (uint8_t *)malloc((size_t)w * h * g_bpp);

    // ---- whole-image decode (reference): full framebuffer + decode() ----
    // NB: the framebuffer path writes whole MCU row bands, so the buffer must
    // be padded up to a multiple of 16 rows (library's existing behavior).
    int hPadded = (h + 15) & ~15;
    heapReset();
    uint8_t *pFrame = (uint8_t *)malloc((size_t)w * hPadded * g_bpp); // caller's full bitmap
    jpeg.setFramebuffer(pFrame);
    int rc = jpeg.decode(0, 0, 0);
    peakWhole = g_heapPeak;
    jpeg.setFramebuffer(NULL);
    memcpy(g_pRef, pFrame, (size_t)w * h * g_bpp); // only the first h rows are valid
    free(pFrame);
    jpeg.close();
    printf("%s: %dx%d, %d bpp output, decoder struct sizeof(JPEGDEC)=%zu bytes\n",
           szName, w, h, g_bpp * 8, sizeof(JPEGDEC));
    CHECK(rc == 1, "whole-image decode() succeeds");
    printf("  whole-image path peak heap: %zu bytes (full bitmap %d bytes)\n",
           peakWhole, w * h * g_bpp);

    // ---- strip decode cases ----
    for (int c = 0; c < nCases; c++) {
        int sh = pStripHeights[c];
        int expectCB = (h + sh - 1) / sh;
        int cb, ok;
        peakStrip = runStripCase(&jpeg, pJPG, lSize, sh, 0, &cb, &ok);
        printf("  strip height %4d -> callbacks %2d (expected %2d), peak heap %5zu bytes"
               " (%.1f%% of whole-image)\n",
               sh, cb, expectCB, peakStrip, 100.0 * peakStrip / peakWhole);
        CHECK(ok, "decodeStrips returns success and strips are contiguous/in-order");
        CHECK(cb == expectCB, "callback count == ceil(height/strip)");
        CHECK(g_nextY == h, "strips covered every output row exactly once");
        CHECK(memcmp(g_pReasm, g_pRef, (size_t)w * h * g_bpp) == 0,
              "strip output is pixel-identical to whole-image decode");
    }

    // ---- strip decode straight to a file (the small-RAM device use case) ----
    {
        char szOut[256];
        snprintf(szOut, sizeof(szOut), "%s/%s.raw", TESTDIR, szName);
        g_outFile = fopen(szOut, "wb");
        int cb, ok;
        runStripCase(&jpeg, pJPG, lSize, 32, 0, &cb, &ok);
        fclose(g_outFile);
        g_outFile = NULL;
        long fSize = 0;
        uint8_t *pFile = readFile(szOut, &fSize); // reuses readFile; name has no dir part missing
        CHECK(pFile && fSize == (long)w * h * g_bpp && memcmp(pFile, g_pRef, (size_t)w * h * g_bpp) == 0,
              "strips written to file during decode match whole-image decode");
        free(pFile);
        remove(szOut);
    }

    // ---- invalid strip heights / bad args ----
    jpeg.openRAM(pJPG, lSize, DummyDraw);
    if (iPixelType) jpeg.setPixelType(iPixelType);
    CHECK(jpeg.decodeStrips(0, StripCB) == 0 && jpeg.getLastError() == JPEG_INVALID_PARAMETER,
          "strip height 0 -> fails with JPEG_INVALID_PARAMETER");
    CHECK(jpeg.decodeStrips(-8, StripCB) == 0 && jpeg.getLastError() == JPEG_INVALID_PARAMETER,
          "strip height -8 -> fails with JPEG_INVALID_PARAMETER");
    CHECK(jpeg.decodeStrips(16, NULL) == 0 && jpeg.getLastError() == JPEG_INVALID_PARAMETER,
          "NULL callback -> fails with JPEG_INVALID_PARAMETER");
    jpeg.close();

    free(g_pRef); free(g_pReasm); free(pJPG);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("warmup\n"); // let stdio do its one-time allocations before we measure

    // small image: 96x64, 4:4:4 (8-row MCU bands). Cases: exact multiple,
    // non-multiple of MCU height, strip taller than the image (single strip).
    const int smallStrips[] = {16, 7, 1000};
    testImage("small.jpg", RGB565_LITTLE_ENDIAN, smallStrips, 3);

    // large image: 800x600, 4:2:0 (16-row MCU bands, clipped last band).
    const int largeStrips[] = {32, 48, 600};
    testImage("large.jpg", RGB565_LITTLE_ENDIAN, largeStrips, 3);

    // pixel-format coverage: RGB8888 and 8-bit grayscale, small image
    const int fmtStrips[] = {16};
    testImage("small.jpg", RGB8888, fmtStrips, 1);
    testImage("small.jpg", EIGHT_BIT_GRAYSCALE, fmtStrips, 1);

    printf("\n%s (%d failures)\n", g_failures ? "TEST FAILED" : "ALL TESTS PASSED", g_failures);
    return g_failures ? 1 : 0;
}
