// Internal-buffer (callback, no framebuffer) recovery test. This exercises
// JPEGRecFillMCU against the reused draw buffer where ABOVE cannot work and
// LEFT must fall back at every draw-group boundary. Build-dir only.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>
#include "../../src/JPEGDEC.cpp"

extern "C" {
int JPEG_RECOVERY_setFillPolicy(int);
int JPEG_RECOVERY_getBadMCUs(void);
int JPEG_RECOVERY_getRSTRecoveries(void);
int JPEG_RECOVERY_getSkipRecoveries(void);
}

static int g_calls = 0, g_maxW = 0, g_maxH = 0;
static int DrawCB(JPEGDRAW *p)
{
    g_calls++;
    if (p->iWidthUsed > g_maxW) g_maxW = p->iWidthUsed;
    if (p->iHeight > g_maxH) g_maxH = p->iHeight;
    // touch every delivered pixel to catch wild pointers
    int n = p->iWidthUsed * p->iHeight;
    volatile uint16_t sink = 0;
    for (int i = 0; i < n; i++) sink ^= p->pPixels[i];
    (void)sink;
    return 1;
}
static int EntropyStart(const uint8_t *p, int len) {
    for (int i = 0; i < len - 1; i++)
        if (p[i] == 0xff && p[i + 1] == 0xda)
            return i + 2 + ((p[i+2] << 8) | p[i+3]);
    return -1;
}
static std::vector<uint8_t> FlipByte(const std::vector<uint8_t> &s, int pos, uint8_t m) {
    std::vector<uint8_t> o = s; o[pos] ^= m; return o;
}
static int decode(const std::vector<uint8_t> &f) {
    g_calls = 0; g_maxW = g_maxH = 0;
    JPEGDEC j;
    if (!j.openRAM((uint8_t*)f.data(), (int)f.size(), DrawCB)) return -2;
    int rc = j.decode(0, 0, 0);
    j.close();
    return rc ? 0 : -1;
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *fh = fopen(argv[1], "rb");
    fseek(fh, 0, SEEK_END); long n = ftell(fh); fseek(fh, 0, SEEK_SET);
    std::vector<uint8_t> orig(n);
    if (fread(orig.data(), 1, n, fh) != (size_t)n) return 1;
    fclose(fh);
    int es = EntropyStart(orig.data(), (int)orig.size());
    int elen = (int)orig.size() - es - 2;
    int fails = 0;

    // clean file in callback mode must still work under every policy
    for (int pol = 0; pol <= 2; pol++) {
        JPEG_RECOVERY_setFillPolicy(pol);
        if (decode(orig) != 0) { printf("clean callback decode failed pol=%d\n", pol); fails++; }
    }
    // sweep corruption in callback mode; every attempt must complete
    int recovered = 0, tried = 0;
    for (int off = 0; off < elen - 2; off += 11) {
        int pos = es + off;
        if (orig[pos] == 0xff || orig[pos] == 0x00) continue;
        tried++;
        if (decode(FlipByte(orig, pos, 0x7c)) == 0) recovered++;
    }
    printf("callback mode: clean OK, corruption %d/%d recovered full frame, "
           "draw calls=%d max block %dx%d, badMCU=%d skip=%d rst=%d\n",
           recovered, tried, g_calls, g_maxW, g_maxH,
           JPEG_RECOVERY_getBadMCUs(), JPEG_RECOVERY_getSkipRecoveries(),
           JPEG_RECOVERY_getRSTRecoveries());
    if (recovered != tried) { printf("FAIL: some callback decodes failed\n"); fails++; }
    printf(fails ? "CALLBACK MODE: FAIL\n" : "CALLBACK MODE: PASS\n");
    return fails ? 1 : 0;
}
