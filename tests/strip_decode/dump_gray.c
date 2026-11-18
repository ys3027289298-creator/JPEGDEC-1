// Dumps a JPEG as 8-bit grayscale (JPEG_LUMA_ONLY) raw bytes for
// cross-checking against an independent decoder. Usage: dump_gray in.jpg out.gray
#include <JPEGDEC.h>
#include <stdio.h>
#include <stdlib.h>

static FILE *f;
static uint8_t *pFrame;
static int iW;

static int draw(JPEGDRAW *p)
{
    const uint8_t *src = (const uint8_t *)p->pPixels; // 8-bit pixels
    for (int r = 0; r < p->iHeight; r++)
        memcpy(&pFrame[(size_t)(p->y + r) * iW + p->x],
               &src[(size_t)r * p->iWidth], (size_t)p->iWidthUsed);
    return 1;
}

int main(int argc, char *argv[])
{
    JPEGDEC dec;
    if (argc != 3) { fprintf(stderr, "usage: %s in.jpg out.gray\n", argv[0]); return 1; }
    if (!dec.open(argv[1], draw)) { fprintf(stderr, "open failed\n"); return 1; }
    iW = dec.getWidth();
    pFrame = (uint8_t *)malloc((size_t)iW * dec.getHeight());
    if (!dec.decode(0, 0, JPEG_LUMA_ONLY)) { fprintf(stderr, "decode failed\n"); return 1; }
    f = fopen(argv[2], "wb");
    fwrite(pFrame, 1, (size_t)iW * dec.getHeight(), f);
    fclose(f);
    printf("%d x %d\n", iW, dec.getHeight());
    return 0;
}
