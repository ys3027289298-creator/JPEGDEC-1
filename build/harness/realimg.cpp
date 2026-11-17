// Real-image robustness: corrupt a large photo and confirm recovery
// never crashes and always returns a full frame (build-dir only).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>
#include "../../src/JPEGDEC.cpp"

extern "C" {
int JPEG_RECOVERY_setEnabled(int);
int JPEG_RECOVERY_getBadMCUs(void);
int JPEG_RECOVERY_getRSTRecoveries(void);
int JPEG_RECOVERY_getSkipRecoveries(void);
int JPEG_RECOVERY_getHuffErrors(void);
}
static int DrawCB(JPEGDRAW *) { return 1; }

static int EntropyStart(const uint8_t *p, int len) {
    for (int i = 0; i < len - 1; i++)
        if (p[i] == 0xff && p[i + 1] == 0xda)
            return i + 2 + ((p[i+2] << 8) | p[i+3]);
    return -1;
}
static bool isMarker(const std::vector<uint8_t> &f, int i) {
    return i+1 < (int)f.size() && f[i] == 0xff && f[i+1] != 0x00;
}
int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s image.jpg\n", argv[0]); return 2; }
    FILE *fh = fopen(argv[1], "rb");
    if (!fh) return 2;
    fseek(fh, 0, SEEK_END); long n = ftell(fh); fseek(fh, 0, SEEK_SET);
    std::vector<uint8_t> orig(n);
    if (fread(orig.data(), 1, n, fh) != (size_t)n) return 1;
    fclose(fh);
    int es = EntropyStart(orig.data(), (int)orig.size());
    int elen = (int)orig.size() - es - 2;
    printf("%s: %ld bytes, entropy at %d\n", argv[1], n, es);

    auto decode = [&](const std::vector<uint8_t> &f, int &w, int &h) {
        std::vector<uint8_t> fb(2048 * 2048 * 2);
        JPEGDEC jpg;
        if (!jpg.openRAM((uint8_t*)f.data(), (int)f.size(), DrawCB)) return -2;
        w = jpg.getWidth(); h = jpg.getHeight();
        int pw = (w + 15) & ~15, ph = (h + 15) & ~15;
        if ((size_t)pw*ph*2 > fb.size()) return -3;
        jpg.setPixelType(RGB565_LITTLE_ENDIAN);
        jpg.setFramebuffer(fb.data());
        int rc = jpg.decode(0,0,0);
        jpg.close();
        return rc ? 0 : -1;
    };

    int w, h, rc;
    rc = decode(orig, w, h);
    printf("pristine decode: rc=%d %dx%d\n", rc, w, h);
    if (rc != 0) return 1;

    // sweep single byte flips across entropy at a stride; all must recover
    int tried = 0, recovered = 0, totalBad = 0;
    for (int off = 0; off < elen - 1; off += 37) {
        int pos = es + off;
        if (isMarker(orig, pos)) continue;
        std::vector<uint8_t> c = orig;
        c[pos] ^= 0x7c;
        int ww, hh;
        tried++;
        if (decode(c, ww, hh) == 0) {
            recovered++;
            totalBad += JPEG_RECOVERY_getBadMCUs();
        }
    }
    printf("single-byte sweep: %d tried, %d recovered full frame, total bad MCUs %d\n",
           tried, recovered, totalBad);
    if (recovered != tried) return 1;

    // multi-point and EOF-adjacent corruption
    std::vector<uint8_t> multi = orig;
    int spots[] = {es + elen/4, es + elen/2, es + 3*elen/4,
                   (int)orig.size() - 8};
    for (int p : spots)
        if (p > es && p < (int)orig.size()-2 && !isMarker(multi, p))
            multi[p] ^= 0x5a;
    rc = decode(multi, w, h);
    printf("multi/EOF corruption: rc=%d badMCU=%d rst=%d skip=%d huff=%d\n",
           rc, JPEG_RECOVERY_getBadMCUs(), JPEG_RECOVERY_getRSTRecoveries(),
           JPEG_RECOVERY_getSkipRecoveries(), JPEG_RECOVERY_getHuffErrors());
    if (rc != 0) return 1;

    // recovery disabled must still fail-stop cleanly (no crash)
    JPEG_RECOVERY_setEnabled(0);
    rc = decode(multi, w, h);
    JPEG_RECOVERY_setEnabled(1);
    printf("recovery disabled on corrupt file: rc=%d (expect nonzero)\n", rc);
    if (rc == 0) return 1;
    printf("REAL-IMAGE ROBUSTNESS: PASS\n");
    return 0;
}
