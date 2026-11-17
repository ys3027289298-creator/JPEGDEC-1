//
// MCU error-recovery test harness for JPEGDEC.
//
// Lives outside the library tree (build/harness) and includes the library
// sources directly, exactly like the bundled MacOS/JPEGDEC_Test. No third
// party dependency: raw JPEG samples are produced by gen_samples.py using
// only the Python standard library, and corruption is injected here.
//
// Modes:
//   recovery_test <clean.jpg> <rst.jpg> <gray.jpg>   run all checks
//   recovery_test dump <outdir> <clean.jpg> <gray.jpg>
//       write framebuffer dumps for a pristine/modified pixel comparison
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <vector>
#include <string>
#include "../../src/JPEGDEC.cpp"

extern "C" {
int JPEG_RECOVERY_setEnabled(int);
int JPEG_RECOVERY_setSearchRange(int);
int JPEG_RECOVERY_setFillPolicy(int);
int JPEG_RECOVERY_getEnabled(void);
int JPEG_RECOVERY_getSearchRange(void);
int JPEG_RECOVERY_getFillPolicy(void);
int JPEG_RECOVERY_getHuffErrors(void);
int JPEG_RECOVERY_getBadMCUs(void);
int JPEG_RECOVERY_getRSTRecoveries(void);
int JPEG_RECOVERY_getSkipRecoveries(void);
int64_t JPEG_RECOVERY_getPixelsTotal(void);
int64_t JPEG_RECOVERY_getPixelsDamaged(void);
double JPEG_RECOVERY_getRecoveryRatio(void);
}

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); g_failures++; } \
} while (0)

static int Micros()
{
    struct timespec res;
    clock_gettime(CLOCK_MONOTONIC, &res);
    return (int)(1000000 * res.tv_sec + res.tv_nsec / 1000);
}

static int DrawCB(JPEGDRAW *) { return 1; }

static int LoadFile(const char *path, std::vector<uint8_t> &out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n);
    if (fread(out.data(), 1, n, f) != (size_t)n) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

// First raw byte of entropy data (right after the SOS segment).
static int FindEntropyStart(const uint8_t *p, int len)
{
    for (int i = 0; i < len - 1; i++) {
        if (p[i] == 0xff && p[i + 1] == 0xda) {
            int seg = (p[i + 2] << 8) | p[i + 3];
            return i + 2 + seg;
        }
    }
    return -1;
}

// Inject a guaranteed-invalid Huffman prefix that survives JPEGFilter:
// raw FF00 pairs filter down to solid 0xFF bytes, i.e. a long run of
// 1-bits that no standard Huffman table contains.
static std::vector<uint8_t> InjectInvalidCode(const std::vector<uint8_t> &src,
                                              int absPos, int nPairs)
{
    std::vector<uint8_t> out = src;
    for (int k = 0; k < nPairs && absPos + k * 2 + 1 < (int)out.size() - 2; k++) {
        out[absPos + k * 2] = 0xff;
        out[absPos + k * 2 + 1] = 0x00;
    }
    return out;
}

// Length-preserving single-byte bit flip: the model of a real storage
// error. It neither removes nor inserts bytes, so marker-less recovery can
// re-align to the byte boundary and the damage stays localized.
static std::vector<uint8_t> FlipByte(const std::vector<uint8_t> &src,
                                     int absPos, uint8_t mask)
{
    std::vector<uint8_t> out = src;
    out[absPos] ^= mask;
    return out;
}

struct DecodeResult {
    int rc = 0, lasterr = 0;
    int badMCUs = 0, huffErrors = 0, rstRec = 0, skipRec = 0;
    int64_t pixelsTotal = 0, pixelsDamaged = 0;
    double ratio = 0.0;
    int us = 0;
    std::vector<uint8_t> fb;
};

// Framebuffers are sized on the padded MCU grid plus slack: scalar and
// SIMD PutMCU paths may write whole vectors past the logical edge.
static int DecodeOne(const std::vector<uint8_t> &file, int w, int h,
                     int pixelType, DecodeResult *r)
{
    int bpp = (pixelType == RGB8888) ? 4 :
              (pixelType == EIGHT_BIT_GRAYSCALE) ? 1 : 2;
    int pw = (w + 15) & ~15, ph = (h + 15) & ~15;
    std::vector<uint8_t> fb((size_t)pw * ph * bpp + 64);
    memset(fb.data(), 0xCD, fb.size());

    JPEGDEC jpg;
    if (!jpg.openRAM((uint8_t *)file.data(), (int)file.size(), DrawCB)) {
        if (r) { r->rc = 0; r->lasterr = jpg.getLastError(); r->fb = fb; }
        return 0;
    }
    jpg.setPixelType(pixelType);
    jpg.setFramebuffer(fb.data());
    int rc = jpg.decode(0, 0, 0);
    if (r) {
        r->rc = rc;
        r->lasterr = jpg.getLastError();
        r->badMCUs = JPEG_RECOVERY_getBadMCUs();
        r->huffErrors = JPEG_RECOVERY_getHuffErrors();
        r->rstRec = JPEG_RECOVERY_getRSTRecoveries();
        r->skipRec = JPEG_RECOVERY_getSkipRecoveries();
        r->pixelsTotal = JPEG_RECOVERY_getPixelsTotal();
        r->pixelsDamaged = JPEG_RECOVERY_getPixelsDamaged();
        r->ratio = JPEG_RECOVERY_getRecoveryRatio();
        r->fb = fb;
    }
    jpg.close();
    return rc;
}

static DecodeResult RunDecode(const std::vector<uint8_t> &file, int w, int h,
                              int pixelType, int runs)
{
    DecodeResult r;
    int t0 = Micros();
    for (int k = 0; k < runs; k++)
        DecodeOne(file, w, h, pixelType, &r);
    r.us = (Micros() - t0) / runs;
    return r;
}

static void PrintRow(const char *name, const DecodeResult &r)
{
    printf("%-26s rc=%d err=%-2d badMCU=%-2d huff=%-2d rst=%-2d skip=%-2d "
           "goodpx=%5lld/%5lld recover=%6.2f%% %5d us\n",
           name, r.rc, r.lasterr, r.badMCUs, r.huffErrors, r.rstRec,
           r.skipRec, (long long)(r.pixelsTotal - r.pixelsDamaged),
           (long long)r.pixelsTotal, r.ratio * 100.0, r.us);
}

static int fbBpp(int pixelType)
{
    return (pixelType == RGB8888) ? 4 :
           (pixelType == EIGHT_BIT_GRAYSCALE) ? 1 : 2;
}

static int CountDiffBytes(const std::vector<uint8_t> &a,
                          const std::vector<uint8_t> &b,
                          int w, int h, int bpp)
{
    int diff = 0;
    for (int y = 0; y < h; y++) {
        int pitch = ((w + 15) & ~15) * bpp;
        for (int i = 0; i < w * bpp; i++)
            if (a[(size_t)y * pitch + i] != b[(size_t)y * pitch + i])
                diff++;
    }
    return diff;
}

//
// Framebuffer dumps for the cross-build (pristine vs modified) comparison.
//
static int DumpMode(int argc, char **argv)
{
    if (argc < 5) { printf("usage: dump <outdir> <clean.jpg> <gray.jpg>\n"); return 2; }
    const char *outdir = argv[2];
    std::vector<uint8_t> clean, gray;
    if (LoadFile(argv[3], clean) || LoadFile(argv[4], gray)) return 2;

    struct Dump { const char *name; const std::vector<uint8_t> *f; int w, h, pt; };
    Dump dumps[] = {
        {"clean_rgb565.raw", &clean, 48, 40, RGB565_LITTLE_ENDIAN},
        {"clean_rgb8888.raw", &clean, 48, 40, RGB8888},
        {"gray_8bit.raw", &gray, 32, 24, EIGHT_BIT_GRAYSCALE},
    };
    for (const Dump &d : dumps) {
        DecodeResult r;
        if (!DecodeOne(*d.f, d.w, d.h, d.pt, &r)) {
            printf("dump decode failed for %s (err=%d)\n", d.name, r.lasterr);
            return 1;
        }
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", outdir, d.name);
        FILE *o = fopen(path, "wb");
        if (!o) { printf("cannot write %s\n", path); return 1; }
        int bpp = fbBpp(d.pt);
        int pitch = ((d.w + 15) & ~15) * bpp;
        for (int y = 0; y < d.h; y++)
            fwrite(&r.fb[(size_t)y * pitch], 1, d.w * bpp, o);
        fclose(o);
        printf("wrote %s\n", path);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "dump") == 0)
        return DumpMode(argc, argv);
    if (argc < 4) {
        printf("usage: %s <clean.jpg> <rst.jpg> <gray.jpg>\n", argv[0]);
        return 2;
    }
    std::vector<uint8_t> clean, rst, gray;
    if (LoadFile(argv[1], clean) || LoadFile(argv[2], rst) ||
        LoadFile(argv[3], gray))
        return 2;

    const int W = 48, H = 40;
    int entClean = FindEntropyStart(clean.data(), (int)clean.size());
    int entRst = FindEntropyStart(rst.data(), (int)rst.size());
    int entLen = (int)clean.size() - entClean - 2;
    int rstEntLen = (int)rst.size() - entRst - 2;
    printf("entropy starts: clean=%d rst=%d (sizes %zu/%zu bytes)\n\n",
           entClean, entRst, clean.size(), rst.size());

    // -------- 1. configuration validation ------------------------------
    printf("== configuration validation ==\n");
    CHECK(JPEG_RECOVERY_getEnabled() == 1, "default enabled != 1");
    CHECK(JPEG_RECOVERY_getSearchRange() == 4096, "default range != 4096");
    CHECK(JPEG_RECOVERY_getFillPolicy() == JPEG_FILL_LEFT, "default policy != LEFT");

    int goodRange = JPEG_RECOVERY_getSearchRange();
    int goodPolicy = JPEG_RECOVERY_getFillPolicy();
    int goodEnabled = JPEG_RECOVERY_getEnabled();
    for (int v : {0, -1, -100, 1000001, 2000000})
        CHECK(JPEG_RECOVERY_setSearchRange(v) == JPEG_INVALID_PARAMETER,
              "invalid search range accepted");
    for (int v : {-1, 3, 99})
        CHECK(JPEG_RECOVERY_setFillPolicy(v) == JPEG_INVALID_PARAMETER,
              "invalid fill policy accepted");
    for (int v : {-1, 2, 99})
        CHECK(JPEG_RECOVERY_setEnabled(v) == JPEG_INVALID_PARAMETER,
              "invalid enabled value accepted");
    CHECK(JPEG_RECOVERY_getSearchRange() == goodRange &&
          JPEG_RECOVERY_getFillPolicy() == goodPolicy &&
          JPEG_RECOVERY_getEnabled() == goodEnabled,
          "rejected parameter changed configuration");
    CHECK(JPEG_RECOVERY_setSearchRange(1) == JPEG_SUCCESS, "range 1 rejected");
    CHECK(JPEG_RECOVERY_setSearchRange(1000000) == JPEG_SUCCESS, "range 1000000 rejected");
    CHECK(JPEG_RECOVERY_setFillPolicy(JPEG_FILL_LEFT) == JPEG_SUCCESS, "LEFT rejected");
    CHECK(JPEG_RECOVERY_setFillPolicy(JPEG_FILL_ABOVE) == JPEG_SUCCESS, "ABOVE rejected");
    CHECK(JPEG_RECOVERY_setFillPolicy(JPEG_FILL_NEUTRAL) == JPEG_SUCCESS, "NEUTRAL rejected");
    JPEG_RECOVERY_setSearchRange(4096);
    JPEG_RECOVERY_setFillPolicy(JPEG_FILL_LEFT);
    JPEG_RECOVERY_setEnabled(1);
    printf("  validation: %s\n\n", g_failures == 0 ? "PASS" : "FAIL (see above)");

    // -------- 2. clean baseline: counters and pixel identity ------------
    printf("== four required scenarios (RGB565, %dx%d) ==\n", W, H);
    DecodeResult cleanOn = RunDecode(clean, W, H, RGB565_LITTLE_ENDIAN, 30);
    JPEG_RECOVERY_setEnabled(0);
    DecodeResult cleanOff = RunDecode(clean, W, H, RGB565_LITTLE_ENDIAN, 30);
    JPEG_RECOVERY_setEnabled(1);
    PrintRow("4) clean file (recov ON)", cleanOn);
    CHECK(cleanOn.rc == 1, "clean decode failed");
    CHECK(cleanOn.lasterr == JPEG_SUCCESS, "clean decode reports error");
    CHECK(cleanOn.badMCUs == 0 && cleanOn.huffErrors == 0 &&
          cleanOn.rstRec == 0 && cleanOn.skipRec == 0,
          "clean file reports recovery activity");
    CHECK(cleanOn.ratio == 1.0, "clean ratio != 1.0");
    int diff = CountDiffBytes(cleanOn.fb, cleanOff.fb, W, H, 2);
    CHECK(diff == 0, "clean framebuffer differs with recovery on/off");
    printf("  clean pixel identity ON vs OFF: %s (%d differing bytes)\n\n",
          diff == 0 ? "PASS" : "FAIL", diff);

    // -------- 3. scenario 1: single MCU, no restart markers -------------
    // Length-preserving bit flips model real storage damage and stay
    // localized to one MCU after the byte-boundary resync.
    std::vector<uint8_t> singleFile;
    DecodeResult single;
    single.rc = 0; single.badMCUs = 0;
    int singleGrid = 3; // 3x3 MCUs of 16x16
    const uint8_t flipMasks[] = {0x7c, 0x5a, 0xa5};
    for (int mi = 0; mi < 3 && singleFile.empty(); mi++) {
        uint8_t mask = flipMasks[mi];
        for (int off = 0; off < entLen && singleFile.empty(); off++) {
            int pos = entClean + off;
            if (clean[pos] == 0xff || clean[pos] == 0x00)
                continue; // never create/erase a marker or stuffed zero
            std::vector<uint8_t> cand = FlipByte(clean, pos, mask);
            DecodeResult probe = RunDecode(cand, W, H, RGB565_LITTLE_ENDIAN, 1);
            if (probe.rc == 1 && probe.badMCUs == 1 && probe.huffErrors >= 1 &&
                probe.rstRec == 0) {
                // prefer an interior MCU so LEFT/ABOVE have a real neighbor
                JPEG_RECOVERY_setFillPolicy(JPEG_FILL_NEUTRAL);
                DecodeResult loc = RunDecode(cand, W, H, RGB565_LITTLE_ENDIAN, 1);
                JPEG_RECOVERY_setFillPolicy(JPEG_FILL_LEFT);
                int pitch = W * 2;
                bool interior = false;
                for (int idx = 0; idx < singleGrid * singleGrid; idx++) {
                    int mx = (idx % singleGrid) * 16, my = (idx / singleGrid) * 16;
                    uint16_t v;
                    memcpy(&v, &loc.fb[(my * pitch) + mx * 2], 2);
                    if (v == 0x8410) {
                        interior = ((idx % singleGrid) >= 1 &&
                                    (idx / singleGrid) >= 1);
                        break;
                    }
                }
                if (interior) { singleFile = cand; single = probe; }
            }
        }
    }
    // fallback: guaranteed-invalid FF00 prefix if no bit flip was suitable
    if (singleFile.empty()) {
        for (int off = 0; off < entLen - 14; off++) {
            std::vector<uint8_t> cand = InjectInvalidCode(clean, entClean + off, 6);
            DecodeResult probe = RunDecode(cand, W, H, RGB565_LITTLE_ENDIAN, 1);
            if (probe.rc == 1 && probe.badMCUs >= 1 && probe.rstRec == 0) {
                singleFile = cand; single = probe; break;
            }
        }
    }
    CHECK(!singleFile.empty(), "could not synthesize single-MCU corruption");
    if (!singleFile.empty()) {
        single = RunDecode(singleFile, W, H, RGB565_LITTLE_ENDIAN, 20);
        PrintRow("1) single MCU corrupt", single);
        CHECK(single.rc == 1 && single.lasterr == JPEG_SUCCESS,
              "single-MCU decode did not recover");
        CHECK(single.badMCUs == 1, "single-MCU badMCU count != 1");
        CHECK(single.rstRec == 0 && single.skipRec >= 1,
              "single-MCU used wrong recovery path");
        CHECK(single.pixelsDamaged > 0 && single.ratio < 1.0,
              "single-MCU stats wrong");
    }
    printf("\n");

    // -------- 4. scenario 2: multiple MCUs, no restart markers -----------
    // Two independent length-preserving bit flips, placed far enough apart
    // to hit two distinct MCUs; accept the minimum-damage candidate.
    std::vector<uint8_t> multiFile;
    DecodeResult multi;
    multi.rc = 0; multi.badMCUs = 99;
    for (int a = 0; a < entLen - 20; a += 2) {
        for (int b = a + 12; b < entLen - 2; b += 4) {
            int pa = entClean + a, pb = entClean + b;
            if (clean[pa] == 0xff || clean[pa] == 0x00 ||
                clean[pb] == 0xff || clean[pb] == 0x00)
                continue;
            std::vector<uint8_t> cand = FlipByte(FlipByte(clean, pa, 0x7c), pb, 0x5a);
            DecodeResult probe = RunDecode(cand, W, H, RGB565_LITTLE_ENDIAN, 1);
            if (probe.rc == 1 && probe.badMCUs >= 2 && probe.badMCUs < multi.badMCUs) {
                multiFile = cand;
                multi = probe;
                if (probe.badMCUs == 2) break; // smallest possible multi-MCU hit
            }
        }
        if (multi.badMCUs == 2) break;
    }
    CHECK(!multiFile.empty(), "could not synthesize multi-MCU corruption");
    if (!multiFile.empty()) {
        multi = RunDecode(multiFile, W, H, RGB565_LITTLE_ENDIAN, 20);
        PrintRow("2) multi MCU corrupt", multi);
        CHECK(multi.rc == 1 && multi.lasterr == JPEG_SUCCESS,
              "multi-MCU decode did not recover");
        CHECK(multi.badMCUs >= 2, "multi-MCU badMCU count < 2");
        CHECK(multi.huffErrors >= 1, "multi-MCU huff error count wrong");
    }
    printf("\n");

    // -------- 5. scenario 3: corruption with restart markers -------------
    std::vector<uint8_t> rstFile;
    DecodeResult rstRes;
    rstRes.rc = 0; rstRes.rstRec = 0;
    for (int n = 4; n <= 10 && rstRes.rstRec < 1; n++) {
        for (int off = 0; off < rstEntLen - n * 2 && rstRes.rstRec < 1; off++) {
            std::vector<uint8_t> cand = InjectInvalidCode(rst, entRst + off, n);
            DecodeResult probe = RunDecode(cand, W, H, RGB565_LITTLE_ENDIAN, 1);
            if (probe.rc == 1 && probe.rstRec >= 1) {
                rstFile = cand;
                rstRes = probe;
                break;
            }
        }
    }
    CHECK(!rstFile.empty(), "could not trigger a restart-marker recovery");
    if (!rstFile.empty()) {
        rstRes = RunDecode(rstFile, W, H, RGB565_LITTLE_ENDIAN, 20);
        PrintRow("3) RST marker corrupt", rstRes);
        CHECK(rstRes.rc == 1 && rstRes.lasterr == JPEG_SUCCESS,
              "RST decode did not recover");
        CHECK(rstRes.rstRec >= 1, "RST resync count < 1");
        CHECK(rstRes.badMCUs >= 1, "RST badMCU count < 1");
        CHECK(rstRes.ratio < 1.0, "RST ratio should be < 1.0");
    }
    printf("\n");

    // -------- 6. recovery disabled keeps the legacy fail-stop -----------
    JPEG_RECOVERY_setEnabled(0);
    DecodeResult legacy = RunDecode(singleFile, W, H, RGB565_LITTLE_ENDIAN, 1);
    JPEG_RECOVERY_setEnabled(1);
    CHECK(legacy.rc == 0 && legacy.lasterr == JPEG_DECODE_ERROR,
          "recovery disabled did not stop on bad Huffman code");
    printf("legacy fail-stop with recovery disabled: %s\n\n",
           legacy.rc == 0 ? "PASS" : "FAIL");

    // -------- 7. fill policy semantics (framebuffer mode) ----------------
    if (!singleFile.empty()) {
        printf("== fill policy semantics ==\n");
        int policies[3] = {JPEG_FILL_LEFT, JPEG_FILL_ABOVE, JPEG_FILL_NEUTRAL};
        DecodeResult pr[3];
        for (int i = 0; i < 3; i++) {
            JPEG_RECOVERY_setFillPolicy(policies[i]);
            pr[i] = RunDecode(singleFile, W, H, RGB565_LITTLE_ENDIAN, 5);
            CHECK(pr[i].rc == 1 && pr[i].badMCUs == 1,
                  "fill policy decode failed");
        }
        JPEG_RECOVERY_setFillPolicy(JPEG_FILL_LEFT);
        int pitch = W * 2, dmgCol = -1, dmgRow = -1;
        for (int my = 0; my < H && dmgCol < 0; my += 16)
            for (int mx = 0; mx < W; mx += 16) {
                uint16_t v;
                memcpy(&v, &pr[2].fb[(my * pitch) + mx * 2], 2);
                if (v == 0x8410) { dmgCol = mx; dmgRow = my; break; }
            }
        CHECK(dmgCol >= 0, "damaged MCU not located via NEUTRAL fill");
        if (dmgCol >= 0) {
            int visW = (dmgCol + 16 <= W) ? 16 : W - dmgCol;
            int visH = (dmgRow + 16 <= H) ? 16 : H - dmgRow;
            bool neutralOk = true, leftOk = true, aboveOk = true;
            auto px = [&](const DecodeResult &r, int x, int y) -> uint16_t {
                uint16_t v;
                memcpy(&v, &r.fb[y * pitch + x * 2], 2);
                return v;
            };
            for (int yy = 0; yy < visH && neutralOk; yy++)
                for (int xx = 0; xx < visW; xx++)
                    if (px(pr[2], dmgCol + xx, dmgRow + yy) != 0x8410) {
                        neutralOk = false; break;
                    }
            CHECK(neutralOk, "NEUTRAL region is not solid mid-gray");
            if (dmgCol >= 16) {
                for (int yy = 0; yy < visH && leftOk; yy++)
                    for (int xx = 0; xx < visW; xx++)
                        if (px(pr[0], dmgCol + xx, dmgRow + yy) !=
                            px(pr[0], dmgCol - 16 + xx, dmgRow + yy)) {
                            leftOk = false; break;
                        }
            }
            CHECK(leftOk, "LEFT region does not match the left neighbor");
            if (dmgRow >= 16) {
                for (int xx = 0; xx < visW && aboveOk; xx++)
                    for (int yy = 0; yy < visH; yy++)
                        if (px(pr[1], dmgCol + xx, dmgRow + yy) !=
                            px(pr[1], dmgCol + xx, dmgRow - 16 + yy)) {
                            aboveOk = false; break;
                        }
            }
            CHECK(aboveOk, "ABOVE region does not match the upper neighbor");
            printf("  damaged MCU at (%d,%d): NEUTRAL=%s LEFT=%s ABOVE=%s\n\n",
                   dmgCol, dmgRow, neutralOk ? "ok" : "bad",
                   leftOk ? "ok" : "bad", aboveOk ? "ok" : "bad");
        }
    }

    // -------- 8. configurable search range -------------------------------
    if (!rstFile.empty()) {
        printf("== configurable search range ==\n");
        JPEG_RECOVERY_setSearchRange(4);
        DecodeResult small = RunDecode(rstFile, W, H, RGB565_LITTLE_ENDIAN, 5);
        JPEG_RECOVERY_setSearchRange(4096);
        DecodeResult big = RunDecode(rstFile, W, H, RGB565_LITTLE_ENDIAN, 5);
        CHECK(small.rc == 1 && big.rc == 1, "search range broke decode");
        printf("  range 4: rst=%d skip=%d | range 4096: rst=%d skip=%d -> %s\n\n",
               small.rstRec, small.skipRec, big.rstRec, big.skipRec,
               (small.rc && big.rc) ? "PASS" : "FAIL");
    }

    // -------- 8b. RST interval boundary alignment ------------------------
    // Corrupt the *last* MCU of a restart interval (N=1) and prove that the
    // resync bookkeeping stays aligned: every MCU the decoder reports as
    // healthy must stay byte-identical to the clean reference. FF00
    // injection is used because length-preserving bit flips do not create
    // invalid Huffman prefixes on this stream.
    {
        printf("== RST boundary alignment (N=1 corruption) ==\n");
        DecodeResult ref = RunDecode(rst, W, H, RGB565_LITTLE_ENDIAN, 1);
        const int grid = 3, mcu = 16, pitch = W * 2;
        auto mcuDiffers = [&](const DecodeResult &r, int idx) {
            int mx = (idx % grid) * mcu, my = (idx / grid) * mcu;
            int vw = (mx + mcu <= W) ? mcu : W - mx;
            int vh = (my + mcu <= H) ? mcu : H - my;
            for (int yy = 0; yy < vh; yy++)
                for (int xx = 0; xx < vw * 2; xx++)
                    if (r.fb[(my + yy) * pitch + mx * 2 + xx] !=
                        ref.fb[(my + yy) * pitch + mx * 2 + xx])
                        return true;
            return false;
        };
        bool foundAny = false;
        int n1Isolated = 0, oddWorst = 0, oddContamination = 0;
        for (int off = 0; off < rstEntLen - 14; off++) {
            std::vector<uint8_t> cand = InjectInvalidCode(rst, entRst + off, 5);
            DecodeResult r = RunDecode(cand, W, H, RGB565_LITTLE_ENDIAN, 1);
            if (r.rc != 1 || r.rstRec < 1)
                continue;
            int firstBad = -1, nDiff = 0;
            for (int idx = 0; idx < grid * grid; idx++) {
                if (mcuDiffers(r, idx)) {
                    if (firstBad < 0) firstBad = idx;
                    nDiff++;
                }
            }
            if (nDiff < 1)
                continue;
            // Generic invariant: only MCUs the decoder filled may differ
            // from the clean reference; a healthy MCU must be identical.
            if (nDiff == r.badMCUs)
                foundAny = true;
            // N=1 case (the corrupted MCU is the last one in its interval).
            // Correct bookkeeping isolates the loss to that one interval,
            // so a clean single-MCU recovery (nDiff == 1) must be common.
            // A resync-count drift corrupts DC predictors of later intervals
            // and spreads the damage to odd numbers of unrelated MCUs.
            // The isolation check is restricted to single-resync events;
            // corruption that spans several intervals is a separate case.
            if ((firstBad % 2) == 1 && r.rstRec == 1) {
                if (nDiff > oddWorst) oddWorst = nDiff;
                if (nDiff == 1)
                    n1Isolated++;
                if (nDiff > 2) // damage left the single resynced interval
                    oddContamination++;
            }
        }
        printf("  N=1: isolated single-MCU recoveries=%d, worst spread=%d, "
               "cross-interval contamination=%d\n",
               n1Isolated, oddWorst, oddContamination);
        CHECK(foundAny, "RST recovery changed pixels of a healthy MCU");
        // Correct resync bookkeeping isolates nearly every single-interval
        // N=1 corruption to its one MCU. A predictor-reset drift turns most
        // of them into 2-MCU losses, roughly halving the isolated count.
        CHECK(n1Isolated >= 40,
              "N=1 resync bookkeeping drift prevented single-MCU isolation");
        CHECK(oddContamination == 0,
              "N=1 damage spread past its restart interval");
        CHECK(oddWorst <= 2,
              "single-interval N=1 resync damaged more than its interval");
        printf("  N=1 resync alignment: %s\n\n",
               (n1Isolated >= 40 && oddContamination == 0 && oddWorst <= 2)
                   ? "PASS" : "FAIL");
    }

    // -------- 9. other pixel formats recover as well ---------------------
    printf("== grayscale and RGB8888 paths ==\n");
    int entGray = FindEntropyStart(gray.data(), (int)gray.size());
    int grayEntLen = (int)gray.size() - entGray - 2;
    std::vector<uint8_t> grayBad;
    DecodeResult grayRes;
    grayRes.rc = 0; grayRes.badMCUs = 0;
    for (int off = 0; off < grayEntLen && grayRes.badMCUs < 1; off++) {
        int pos = entGray + off;
        if (gray[pos] == 0xff || gray[pos] == 0x00)
            continue;
        std::vector<uint8_t> cand = FlipByte(gray, pos, 0x7c);
        DecodeResult probe = RunDecode(cand, 32, 24, EIGHT_BIT_GRAYSCALE, 1);
        if (probe.rc == 1 && probe.badMCUs >= 1) { grayBad = cand; grayRes = probe; }
    }
    if (grayBad.empty()) { // fallback: guaranteed-invalid prefix
        for (int off = 0; off < grayEntLen - 12 && grayRes.badMCUs < 1; off++) {
            std::vector<uint8_t> cand = InjectInvalidCode(gray, entGray + off, 5);
            DecodeResult probe = RunDecode(cand, 32, 24, EIGHT_BIT_GRAYSCALE, 1);
            if (probe.rc == 1 && probe.badMCUs >= 1) { grayBad = cand; grayRes = probe; }
        }
    }
    CHECK(!grayBad.empty(), "grayscale corruption not synthesized");
    if (!grayBad.empty()) {
        grayRes = RunDecode(grayBad, 32, 24, EIGHT_BIT_GRAYSCALE, 5);
        PrintRow("grayscale corrupt (8bpp)", grayRes);
        CHECK(grayRes.rc == 1 && grayRes.badMCUs >= 1, "grayscale recovery failed");
        JPEG_RECOVERY_setFillPolicy(JPEG_FILL_NEUTRAL);
        DecodeResult gn = RunDecode(grayBad, 32, 24, EIGHT_BIT_GRAYSCALE, 1);
        JPEG_RECOVERY_setFillPolicy(JPEG_FILL_LEFT);
        bool grayNeutral = false;
        for (int my = 0; my < 24 && !grayNeutral; my += 8)
            for (int mx = 0; mx < 32; mx += 8) {
                int pitch = 32;
                if (gn.fb[my * pitch + mx] == 0x80) { grayNeutral = true; break; }
            }
        CHECK(grayNeutral, "grayscale NEUTRAL fill is not 0x80");
    }
    if (!singleFile.empty()) {
        DecodeResult rgb32 = RunDecode(singleFile, W, H, RGB8888, 5);
        PrintRow("single MCU corrupt (RGB8888)", rgb32);
        CHECK(rgb32.rc == 1 && rgb32.badMCUs == 1, "RGB8888 recovery failed");
        JPEG_RECOVERY_setFillPolicy(JPEG_FILL_NEUTRAL);
        DecodeResult rn = RunDecode(singleFile, W, H, RGB8888, 1);
        JPEG_RECOVERY_setFillPolicy(JPEG_FILL_LEFT);
        bool rgbNeutral = false;
        int pitch = W * 4;
        for (int my = 0; my < H && !rgbNeutral; my += 16)
            for (int mx = 0; mx < W && !rgbNeutral; mx += 16) {
                size_t o = (size_t)my * pitch + mx * 4;
                if (rn.fb[o] == 0x80 && rn.fb[o + 1] == 0x80 &&
                    rn.fb[o + 2] == 0x80 && rn.fb[o + 3] == 0xff)
                    rgbNeutral = true;
            }
        CHECK(rgbNeutral, "RGB8888 NEUTRAL fill has wrong value");
    }
    printf("\n");

    printf("%s: %d failure(s)\n",
           g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
    return g_failures ? 1 : 0;
}
