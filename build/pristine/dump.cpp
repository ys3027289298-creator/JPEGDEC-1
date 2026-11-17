// Pristine-library framebuffer dumper (build dir only).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vector>
#include "JPEGDEC_wrapped.cpp"

static int DrawCB(JPEGDRAW *) { return 1; }

static int dump(const char *path, int w, int h, int pt, const char *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> jf(n);
    if (fread(jf.data(), 1, n, f) != (size_t)n) { fclose(f); return 1; }
    fclose(f);
    int bpp = (pt == RGB8888) ? 4 : (pt == EIGHT_BIT_GRAYSCALE) ? 1 : 2;
    int pw = (w + 15) & ~15, ph = (h + 15) & ~15;
    std::vector<uint8_t> fb((size_t)pw * ph * bpp + 64);
    memset(fb.data(), 0xCD, fb.size());
    JPEGDEC jpg;
    if (!jpg.openRAM(jf.data(), (int)jf.size(), DrawCB)) { printf("open fail %s\n", path); return 1; }
    jpg.setPixelType(pt);
    jpg.setFramebuffer(fb.data());
    if (!jpg.decode(0, 0, 0)) { printf("decode fail %s\n", path); return 1; }
    jpg.close();
    FILE *o = fopen(out, "wb");
    int pitch = pw * bpp;
    for (int y = 0; y < h; y++)
        fwrite(&fb[(size_t)y * pitch], 1, w * bpp, o);
    fclose(o);
    printf("wrote %s\n", out);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: %s outdir clean.jpg gray.jpg\n", argv[0]); return 2; }
    const char *dir = argv[1];
    char p[256];
    snprintf(p, sizeof(p), "%s/clean_rgb565.raw", dir);
    if (dump(argv[2], 48, 40, RGB565_LITTLE_ENDIAN, p)) return 1;
    snprintf(p, sizeof(p), "%s/clean_rgb8888.raw", dir);
    if (dump(argv[2], 48, 40, RGB8888, p)) return 1;
    snprintf(p, sizeof(p), "%s/gray_8bit.raw", dir);
    if (dump(argv[3], 32, 24, EIGHT_BIT_GRAYSCALE, p)) return 1;
    return 0;
}
