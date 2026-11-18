//
// Tests for the standard-Huffman-table cache in jpeg.inl
//
// Self-contained: builds its own tiny JPEG samples (standard and custom
// Huffman tables) and verifies:
//   - table build counts with/without reuse (standard vs custom tables)
//   - pixel-exact decode output vs. the no-cache path
//   - decode time with/without reuse
//   - cache memory usage
//   - detection + handling of a corrupted cache entry
//   - behavior for invalid parameters (capacity 0, negative, too large)
//
#include <JPEGDEC.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <vector>
#include "../test_images/tulips.h" // 640x480 standard-table test image from the repo

// Runtime configuration API implemented in jpeg.inl (not part of the
// public header; declared here so the test can link to it)
extern int JPEGHuffCacheSetCapacity(int iCapacity);
extern int JPEGHuffCacheGetCapacity(void);
extern int JPEGHuffCacheGetMemory(void);
extern void JPEGHuffCacheSetStatsEnable(int bEnable);
extern void JPEGHuffCacheGetStats(uint32_t *pulHits, uint32_t *pulMisses, uint32_t *pulBuilds, uint32_t *pulCorruptions);
extern void JPEGHuffCacheResetStats(void);
extern void JPEGHuffCacheFlush(void);
extern uint8_t *JPEGHuffCacheGetEntryData(int iEntry, int *piSize);

static int g_iFailures = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); g_iFailures++; } \
} while (0)
#define INFO(msg) do { printf("  INFO: %s\n", msg); } while (0)

static long micros(void)
{
    struct timespec res;
    clock_gettime(CLOCK_MONOTONIC, &res);
    return 1000000L*res.tv_sec + res.tv_nsec/1000;
}

// ---------------------------------------------------------------------------
// Minimal JPEG sample builder
// ---------------------------------------------------------------------------
static void put8(std::vector<uint8_t> &v, int b) { v.push_back((uint8_t)b); }
static void put16(std::vector<uint8_t> &v, int w) { put8(v, w>>8); put8(v, w&0xff); }

// standard (Annex K) tables: 16 counts + symbols
static const uint8_t std_dc_luma[28] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0, 0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t std_dc_chroma[28] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0, 0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t std_ac_luma[178] = {
  0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d,
  0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
  0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
  0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
  0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
  0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
  0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
  0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
  0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
  0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
  0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
  0xf9,0xfa };
static const uint8_t std_ac_chroma[178] = {
  0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77,
  0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
  0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
  0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
  0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
  0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
  0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
  0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
  0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
  0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
  0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
  0xf9,0xfa };
// custom tables: a single 2-bit code each (DC: category 0, AC: EOB)
static const uint8_t cust_dc[17] = {0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0x00};
static const uint8_t cust_ac[17] = {0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0x00};

static void addDQT(std::vector<uint8_t> &v)
{
    put16(v, 0xffdb); put16(v, 67); put8(v, 0);
    for (int i = 0; i < 64; i++) put8(v, 1);
}
static void padTo(std::vector<uint8_t> &v, size_t minSize) // COM segment padding
{
    if (v.size() + 4 >= minSize) return;
    size_t payload = minSize - v.size() - 4;
    put16(v, 0xfffe); put16(v, (int)(2 + payload));
    for (size_t i = 0; i < payload; i++) put8(v, 0x20);
}
static void addDHT(std::vector<uint8_t> &v, int id, const uint8_t *pTable, int iLen)
{
    put16(v, 0xffc4); put16(v, 2 + 1 + iLen); put8(v, id);
    for (int i = 0; i < iLen; i++) put8(v, pTable[i]);
}
// Build an 8x8 baseline JPEG, all-zero coefficients.
// nComp: 1 (grayscale) or 3 (4:4:4 color); bStd: standard or custom tables
static std::vector<uint8_t> makeJPEG(int nComp, bool bStd)
{
    std::vector<uint8_t> v;
    const uint8_t *dc0 = bStd ? std_dc_luma : cust_dc;
    const uint8_t *ac0 = bStd ? std_ac_luma : cust_ac;
    const uint8_t *dc1 = bStd ? std_dc_chroma : cust_dc;
    const uint8_t *ac1 = bStd ? std_ac_chroma : cust_ac;
    int dc0len = bStd ? 28 : 17, ac0len = bStd ? 178 : 17;
    int dc1len = bStd ? 28 : 17, ac1len = bStd ? 178 : 17;

    put16(v, 0xffd8); // SOI
    addDQT(v);
    padTo(v, 320); // the parser rejects files smaller than 256 bytes
    put16(v, 0xffc0); put16(v, 8 + 3*nComp); // SOF0
    put8(v, 8); put16(v, 8); put16(v, 8); put8(v, nComp);
    for (int i = 0; i < nComp; i++) { put8(v, i+1); put8(v, 0x11); put8(v, 0); }
    addDHT(v, 0x00, dc0, dc0len);
    addDHT(v, 0x10, ac0, ac0len);
    if (nComp == 3) {
        addDHT(v, 0x01, dc1, dc1len);
        addDHT(v, 0x11, ac1, ac1len);
    }
    put16(v, 0xffda); put16(v, 6 + 2*nComp); // SOS
    put8(v, nComp);
    for (int i = 0; i < nComp; i++) { put8(v, i+1); put8(v, (i == 0) ? 0x00 : 0x11); }
    put8(v, 0); put8(v, 63); put8(v, 0);
    // entropy data: DC diff 0 + EOB per block
    if (bStd) {
        if (nComp == 1) { put8(v, 0x2b); }          // '00'+'1010' + pad
        else { put8(v, 0x28); put8(v, 0x03); }       // '001010'+'0000'+'0000' + pad
    } else {
        if (nComp == 1) { put8(v, 0x0f); }           // '00'+'00' + pad
        else { put8(v, 0x00); put8(v, 0x0f); }       // 3x'0000' + pad
    }
    put16(v, 0xffd9); // EOI
    return v;
}

// ---------------------------------------------------------------------------
// Decode driver with pixel capture / comparison
// ---------------------------------------------------------------------------
static std::vector<uint8_t> g_refPix;   // reference pixels (empty = capture)
static size_t g_refOff = 0;             // compare offset within the reference
static int g_iPixMismatch = 0;          // set if a frame differs from reference
static bool g_bCheckPix = true;         // false = ignore pixels entirely
static bool g_bCaptureFrame = false;    // true = capture this frame as reference

static int DrawCB(JPEGDRAW *pDraw)
{
    const uint8_t *src = (const uint8_t *)pDraw->pPixels;
    size_t n = (size_t)pDraw->iWidth * pDraw->iHeight * (size_t)(pDraw->iBpp/8);
    if (!g_bCheckPix)
        return 1;
    if (g_bCaptureFrame) {
        g_refPix.insert(g_refPix.end(), src, src + n); // capture the whole frame
    } else {
        if (g_refOff + n > g_refPix.size() || memcmp(&g_refPix[g_refOff], src, n) != 0)
            g_iPixMismatch = 1;
        g_refOff += n;
        if (g_refOff >= g_refPix.size()) g_refOff = 0; // full frame compared
    }
    return 1;
}
static void resetCheck(void) { g_refPix.clear(); g_refOff = 0; g_iPixMismatch = 0; g_bCheckPix = true; }
static void setRef(const std::vector<uint8_t> &ref) { g_refPix = ref; g_refOff = 0; g_iPixMismatch = 0; g_bCheckPix = true; }

struct Stats { uint32_t hits, misses, builds, corrupt; };
static Stats getStats(void)
{
    Stats s;
    JPEGHuffCacheGetStats(&s.hits, &s.misses, &s.builds, &s.corrupt);
    return s;
}

// decode the sample iCount times; returns total microseconds
static long decodeN(JPEGDEC &jpeg, const uint8_t *pData, int iSize, int iCount, int *piDecodes)
{
    long lStart = micros();
    int ok = 0;
    for (int i = 0; i < iCount; i++) {
        int prevMis = g_iPixMismatch;
        g_bCaptureFrame = (g_bCheckPix && g_refPix.empty()); // first checked frame is the reference
        if (jpeg.openFLASH((uint8_t *)pData, iSize, DrawCB) && jpeg.decode(0, 0, 0))
            ok++;
        g_bCaptureFrame = false;
        if (g_iPixMismatch && !prevMis) {
            Stats s = getStats();
            printf("  DEBUG: mismatch first seen at iteration %d (hits=%u misses=%u builds=%u corrupt=%u)\n",
                   i, s.hits, s.misses, s.builds, s.corrupt);
        }
        jpeg.close();
    }
    if (piDecodes) *piDecodes = ok;
    return micros() - lStart;
}

int main(void)
{
    JPEGDEC jpeg;
    std::vector<uint8_t> stdGray   = makeJPEG(1, true);
    std::vector<uint8_t> stdColor  = makeJPEG(3, true);
    std::vector<uint8_t> custGray  = makeJPEG(1, false);
    std::vector<uint8_t> custColor = makeJPEG(3, false);
    const int N = 20000; // decode iterations for the tiny samples
    int iDec;
    long t0, t1;
    char sz[256];

    printf("JPEGDEC Huffman table cache tests\n");
    printf("sample sizes: std-gray=%zu std-color=%zu cust-gray=%zu cust-color=%zu bytes\n",
           stdGray.size(), stdColor.size(), custGray.size(), custColor.size());

    // ------------------------------------------------------------------
    printf("\n[1] Invalid parameters and capacity 0\n");
    JPEGHuffCacheSetCapacity(4);
    CHECK(JPEGHuffCacheSetCapacity(-1) == -1, "capacity -1 rejected");
    CHECK(JPEGHuffCacheSetCapacity(1000000) == -1, "capacity 1000000 rejected");
    CHECK(JPEGHuffCacheGetCapacity() == 4, "capacity unchanged after invalid calls");
    CHECK(JPEGHuffCacheSetCapacity(0) == 0, "capacity 0 accepted (cache disabled)");
    CHECK(JPEGHuffCacheGetMemory() == 0, "cache memory is 0 when disabled");
    JPEGHuffCacheResetStats();
    g_bCheckPix = false;
    decodeN(jpeg, stdColor.data(), (int)stdColor.size(), 100, &iDec);
    { Stats s = getStats();
      snprintf(sz, sizeof(sz), "capacity 0: %d decodes, %u builds, %u hits (expect 100/100/0)", iDec, s.builds, s.hits);
      CHECK(iDec == 100 && s.builds == 100 && s.hits == 0, sz); }
    // stats collection disabled
    JPEGHuffCacheSetCapacity(4);
    JPEGHuffCacheSetStatsEnable(0);
    JPEGHuffCacheResetStats();
    decodeN(jpeg, stdColor.data(), (int)stdColor.size(), 10, &iDec);
    { Stats s = getStats();
      snprintf(sz, sizeof(sz), "stats disabled: counters stay 0 (got hits=%u misses=%u builds=%u corrupt=%u)",
               s.hits, s.misses, s.builds, s.corrupt);
      CHECK(s.hits == 0 && s.misses == 0 && s.builds == 0 && s.corrupt == 0, sz); }
    JPEGHuffCacheSetStatsEnable(1);

    // ------------------------------------------------------------------
    printf("\n[2] Standard-table files: reuse vs rebuild (N=%d)\n", N);
    JPEGHuffCacheFlush();
    JPEGHuffCacheResetStats();
    resetCheck();
    t1 = decodeN(jpeg, stdColor.data(), (int)stdColor.size(), N, &iDec);
    std::vector<uint8_t> pixStdColor = g_refPix;
    { Stats s = getStats();
      snprintf(sz, sizeof(sz), "std-color cached: %d decodes, builds=%u hits=%u misses=%u (expect %d/1/%d/1)",
               iDec, s.builds, s.hits, s.misses, N, N-1);
      CHECK(iDec == N && s.builds == 1 && s.hits == (uint32_t)(N-1) && s.misses == 1, sz); }
    CHECK(g_iPixMismatch == 0, "std-color: all cached frames pixel-identical");
    snprintf(sz, sizeof(sz), "std-color: %d decodes with reuse in %ld us (%.2f us/frame)", N, t1, (double)t1/N);
    INFO(sz);

    JPEGHuffCacheSetCapacity(0); // no reuse
    JPEGHuffCacheResetStats();
    resetCheck();
    t0 = decodeN(jpeg, stdColor.data(), (int)stdColor.size(), N, &iDec);
    { Stats s = getStats();
      snprintf(sz, sizeof(sz), "std-color no-cache: builds=%u (expect %d)", s.builds, N);
      CHECK(s.builds == (uint32_t)N, sz); }
    snprintf(sz, sizeof(sz), "std-color: %d decodes without reuse in %ld us (%.2f us/frame)", N, t0, (double)t0/N);
    INFO(sz);
    CHECK(g_refPix == pixStdColor, "std-color: cached output pixel-identical to no-cache");
    JPEGHuffCacheSetCapacity(4);

    // grayscale standard file (different table subset -> second cache entry)
    JPEGHuffCacheFlush();
    JPEGHuffCacheResetStats();
    resetCheck();
    t1 = decodeN(jpeg, stdGray.data(), (int)stdGray.size(), N, &iDec);
    std::vector<uint8_t> pixStdGray = g_refPix;
    { Stats s = getStats();
      snprintf(sz, sizeof(sz), "std-gray cached: builds=%u hits=%u (expect 1/%d)", s.builds, s.hits, N-1);
      CHECK(s.builds == 1 && s.hits == (uint32_t)(N-1), sz); }
    CHECK(g_iPixMismatch == 0, "std-gray: all cached frames pixel-identical");
    snprintf(sz, sizeof(sz), "std-gray: %d decodes with reuse in %ld us (%.2f us/frame)", N, t1, (double)t1/N);
    INFO(sz);
    JPEGHuffCacheSetCapacity(0);
    resetCheck();
    t0 = decodeN(jpeg, stdGray.data(), (int)stdGray.size(), N, &iDec);
    snprintf(sz, sizeof(sz), "std-gray: %d decodes without reuse in %ld us (%.2f us/frame)", N, t0, (double)t0/N);
    INFO(sz);
    CHECK(g_refPix == pixStdGray, "std-gray: cached output pixel-identical to no-cache");
    JPEGHuffCacheSetCapacity(4);

    // ------------------------------------------------------------------
    printf("\n[3] Custom-table files: always rebuilt (N=%d)\n", N);
    JPEGHuffCacheFlush();
    JPEGHuffCacheResetStats();
    resetCheck();
    t1 = decodeN(jpeg, custColor.data(), (int)custColor.size(), N, &iDec);
    std::vector<uint8_t> pixCustColor = g_refPix;
    { Stats s = getStats();
      snprintf(sz, sizeof(sz), "cust-color cache-enabled run: builds=%u hits=%u misses=%u (expect %d/0/0)",
               s.builds, s.hits, s.misses, N);
      CHECK(iDec == N && s.builds == (uint32_t)N && s.hits == 0 && s.misses == 0, sz); }
    CHECK(g_iPixMismatch == 0, "cust-color: all frames pixel-identical");
    snprintf(sz, sizeof(sz), "cust-color: %d decodes (cache enabled) in %ld us (%.2f us/frame)", N, t1, (double)t1/N);
    INFO(sz);
    JPEGHuffCacheSetCapacity(0);
    resetCheck();
    t0 = decodeN(jpeg, custColor.data(), (int)custColor.size(), N, &iDec);
    snprintf(sz, sizeof(sz), "cust-color: %d decodes (cache disabled) in %ld us (%.2f us/frame)", N, t0, (double)t0/N);
    INFO(sz);
    CHECK(g_refPix == pixCustColor, "cust-color: cache-enabled output pixel-identical to no-cache");
    JPEGHuffCacheSetCapacity(4);
    // custom grayscale too
    JPEGHuffCacheResetStats();
    resetCheck();
    decodeN(jpeg, custGray.data(), (int)custGray.size(), 1000, &iDec);
    std::vector<uint8_t> pixCustGray = g_refPix;
    { Stats s = getStats();
      snprintf(sz, sizeof(sz), "cust-gray: builds=%u hits=%u (expect 1000/0)", s.builds, s.hits);
      CHECK(s.builds == 1000 && s.hits == 0, sz); }
    JPEGHuffCacheSetCapacity(0);
    resetCheck();
    decodeN(jpeg, custGray.data(), (int)custGray.size(), 1, &iDec);
    CHECK(g_refPix == pixCustGray, "cust-gray: cache-enabled output pixel-identical to no-cache");
    JPEGHuffCacheSetCapacity(4);

    // ------------------------------------------------------------------
    printf("\n[4] Cache corruption: detection and handling\n");
    JPEGHuffCacheFlush();
    JPEGHuffCacheResetStats();
    resetCheck();
    decodeN(jpeg, stdColor.data(), (int)stdColor.size(), 1, &iDec); // capture reference + insert entry
    { int iSize = 0;
      uint8_t *p = JPEGHuffCacheGetEntryData(0, &iSize);
      snprintf(sz, sizeof(sz), "entry data exposed for test (%d bytes)", iSize);
      CHECK(p != NULL && iSize > 0, sz);
      p[0] ^= 0xff;            // scribble on the cached tables
      p[iSize/2] ^= 0xff;
      p[iSize-1] ^= 0xff; }
    decodeN(jpeg, stdColor.data(), (int)stdColor.size(), 1, &iDec); // detects + rebuilds + compares
    { Stats s = getStats();
      snprintf(sz, sizeof(sz), "corruption detected: corrupt=%u (expect 1)", s.corrupt);
      CHECK(s.corrupt == 1, sz);
      snprintf(sz, sizeof(sz), "corrupted entry not reused: hits=%u builds=%u (expect 0/2)", s.hits, s.builds);
      CHECK(s.hits == 0 && s.builds == 2, sz); }
    CHECK(g_iPixMismatch == 0 && iDec == 1, "decode output still pixel-correct after corruption");
    decodeN(jpeg, stdColor.data(), (int)stdColor.size(), 1, &iDec); // entry healed
    { Stats s = getStats();
      snprintf(sz, sizeof(sz), "entry healed and reused: hits=%u (expect 1)", s.hits);
      CHECK(s.hits == 1, sz); }
    CHECK(JPEGHuffCacheGetEntryData(-1, NULL) == NULL && JPEGHuffCacheGetEntryData(999, NULL) == NULL,
          "entry hook rejects invalid index");

    // ------------------------------------------------------------------
    printf("\n[5] Capacity / LRU behavior\n");
    JPEGHuffCacheSetCapacity(1);
    JPEGHuffCacheResetStats();
    g_bCheckPix = false;
    for (int i = 0; i < 1000; i++) {
        decodeN(jpeg, stdColor.data(), (int)stdColor.size(), 1, &iDec);
        decodeN(jpeg, stdGray.data(), (int)stdGray.size(), 1, &iDec);
    }
    { Stats s = getStats();
      snprintf(sz, sizeof(sz), "capacity 1, alternating 2 std files: builds=%u (expect 2000)", s.builds);
      CHECK(s.builds == 2000, sz); }
    setRef(pixStdColor); // capacity-1 output must still be pixel-correct
    decodeN(jpeg, stdColor.data(), (int)stdColor.size(), 1, &iDec);
    CHECK(g_iPixMismatch == 0, "capacity 1: std-color output pixel-correct");
    setRef(pixStdGray);
    decodeN(jpeg, stdGray.data(), (int)stdGray.size(), 1, &iDec);
    CHECK(g_iPixMismatch == 0, "capacity 1: std-gray output pixel-correct");
    JPEGHuffCacheSetCapacity(4);
    JPEGHuffCacheFlush();
    JPEGHuffCacheResetStats();
    g_bCheckPix = false;
    for (int i = 0; i < 1000; i++) {
        decodeN(jpeg, stdColor.data(), (int)stdColor.size(), 1, &iDec);
        decodeN(jpeg, stdGray.data(), (int)stdGray.size(), 1, &iDec);
    }
    { Stats s = getStats();
      snprintf(sz, sizeof(sz), "capacity 4, alternating 2 std files: builds=%u (expect 2)", s.builds);
      CHECK(s.builds == 2, sz); }
    // mixed standard + custom with capacity 4
    JPEGHuffCacheFlush();
    JPEGHuffCacheResetStats();
    for (int i = 0; i < 1000; i++) {
        decodeN(jpeg, stdColor.data(), (int)stdColor.size(), 1, &iDec);
        decodeN(jpeg, custColor.data(), (int)custColor.size(), 1, &iDec);
    }
    { Stats s = getStats();
      snprintf(sz, sizeof(sz), "capacity 4, std+custom mixed: builds=%u (expect 1001)", s.builds);
      CHECK(s.builds == 1001, sz); }

    // ------------------------------------------------------------------
    printf("\n[6] Cache memory\n");
    JPEGHuffCacheSetCapacity(2);
    { int m2 = JPEGHuffCacheGetMemory();
      JPEGHuffCacheSetCapacity(4);
      int m4 = JPEGHuffCacheGetMemory();
      snprintf(sz, sizeof(sz), "capacity 2: %d bytes, capacity 4: %d bytes (%.1f KB)", m2, m4, m4/1024.0);
      INFO(sz);
      CHECK(m2 > 0 && m4 == 2*m2, "memory scales linearly with capacity"); }

    // ------------------------------------------------------------------
    printf("\n[7] Real image (tulips 640x480, standard tables, N=200)\n");
    JPEGHuffCacheSetCapacity(4);
    JPEGHuffCacheFlush();
    JPEGHuffCacheResetStats();
    resetCheck();
    t1 = decodeN(jpeg, tulips, (int)sizeof(tulips), 200, &iDec);
    std::vector<uint8_t> pixTulips = g_refPix;
    { Stats s = getStats();
      snprintf(sz, sizeof(sz), "tulips cached: %d decodes, builds=%u hits=%u (expect 200/1/199)", iDec, s.builds, s.hits);
      CHECK(iDec == 200 && s.builds == 1 && s.hits == 199, sz); }
    CHECK(g_iPixMismatch == 0, "tulips: all cached frames pixel-identical");
    snprintf(sz, sizeof(sz), "tulips: 200 decodes with reuse in %ld us (%.2f us/frame)", t1, t1/200.0);
    INFO(sz);
    JPEGHuffCacheSetCapacity(0);
    resetCheck();
    t0 = decodeN(jpeg, tulips, (int)sizeof(tulips), 200, &iDec);
    snprintf(sz, sizeof(sz), "tulips: 200 decodes without reuse in %ld us (%.2f us/frame)", t0, t0/200.0);
    INFO(sz);
    CHECK(g_refPix == pixTulips, "tulips: cached output pixel-identical to no-cache");
    JPEGHuffCacheSetCapacity(4);

    printf("\n%s (%d failures)\n", g_iFailures ? "TESTS FAILED" : "ALL TESTS PASSED", g_iFailures);
    return g_iFailures ? 1 : 0;
}
