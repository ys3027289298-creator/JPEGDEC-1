// Exploration: how far does marker-less recovery cascade for
// length-preserving bit-flip corruption vs FF00 injection?
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>
#include "../../src/JPEGDEC.cpp"

extern "C" {
int JPEG_RECOVERY_getBadMCUs(void);
int JPEG_RECOVERY_getHuffErrors(void);
int JPEG_RECOVERY_getRSTRecoveries(void);
int JPEG_RECOVERY_getSkipRecoveries(void);
}
static int DrawCB(JPEGDRAW *) { return 1; }

static int EntropyStart(const uint8_t *p, int len) {
    for (int i = 0; i < len - 1; i++)
        if (p[i] == 0xff && p[i + 1] == 0xda)
            return i + 2 + ((p[i+2] << 8) | p[i+3]);
    return -1;
}
static int probe(const std::vector<uint8_t> &f, int w, int h) {
    std::vector<uint8_t> fb(((w+15)&~15)*((h+15)&~15)*2 + 64);
    JPEGDEC jpg;
    if (!jpg.openRAM((uint8_t*)f.data(), (int)f.size(), DrawCB)) return -1;
    jpg.setPixelType(RGB565_LITTLE_ENDIAN);
    jpg.setFramebuffer(fb.data());
    int rc = jpg.decode(0,0,0);
    jpg.close();
    return rc ? JPEG_RECOVERY_getBadMCUs() : -1;
}
int main(int argc, char **argv) {
    FILE *fh = fopen(argv[1], "rb");
    std::vector<uint8_t> f;
    fseek(fh, 0, SEEK_END); long n = ftell(fh); fseek(fh, 0, SEEK_SET);
    f.resize(n); fread(f.data(), 1, n, fh); fclose(fh);
    int es = EntropyStart(f.data(), (int)f.size());
    int elen = (int)f.size() - es - 2;
    // histogram of bad-MCU counts for single-point bit flips (length preserving)
    int hist[16] = {0};
    for (int off = 0; off < elen; off++) {
        for (int mask : {0x7c, 0x5a, 0xa5, 0xff}) {
            std::vector<uint8_t> c = f;
            if (c[es+off] == 0xff || c[es+off] == 0x00) continue;
            c[es+off] ^= (uint8_t)mask;
            int b = probe(c, 48, 40);
            if (b >= 0 && b < 16) hist[b]++;
        }
    }
    printf("bit-flip single-point badMCU histogram (0..15):\n");
    for (int i = 0; i < 16; i++) if (hist[i]) printf("  bad=%d: %d cases\n", i, hist[i]);
    // two-point flips: count candidates yielding exactly 2 bad MCUs
    int exact2 = 0, ge2 = 0, samples = 0;
    for (int a = 0; a < elen - 20; a += 3)
        for (int b = a + 12; b < elen - 2; b += 5) {
            std::vector<uint8_t> c = f;
            c[es+a] ^= 0x7c; c[es+b] ^= 0x5a;
            int bad = probe(c, 48, 40);
            if (bad >= 2) { ge2++; if (bad == 2) exact2++; }
            samples++;
        }
    printf("two-point flips: %d tried, %d with >=2 bad MCUs, %d with exactly 2\n",
           samples, ge2, exact2);
    return 0;
}
