// Dual-output feature test/benchmark for JPEGDEC.
//
// Built twice from the same source:
//   - against the modified library (full functional + timing + error suite)
//   - against the pristine baseline library (single-output reference dumps)
// The run script byte-compares the single-output dumps of both builds, which
// proves the original single-output path is pixel-for-pixel unchanged.
//
#include <JPEGDEC.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <string>
#include <algorithm>

// ---- Copied public descriptor (kept out of JPEGDEC.h on purpose) ----------
// Must match the layout in src/jpeg.inl exactly; the library re-validates the
// magic and the 20-byte size at every dual-output decode call.
#define JPEG_DUAL_OUTPUT   0x00000100
#define JPEG_DUAL_MAGIC    0x44554144u /* 'DUAL' */
typedef struct jpeg_dual_plane_tag
{
    uint32_t u32Magic;
    uint8_t *pYPlane;
    int iYPitch;
    int iYWidth;
    int iYHeight;
    int bOutputY;
} JPEG_DUAL_PLANE;

static long micros_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static std::vector<uint8_t> read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(n);
    if (fread(buf.data(), 1, n, f) != (size_t)n) { perror("fread"); exit(2); }
    fclose(f);
    return buf;
}

struct Frame {
    int w = 0, h = 0, bpp = 0;
    std::vector<uint8_t> px;
    void init(int width, int height, int b) {
        w = width; h = height; bpp = b;
        px.assign((size_t)w * h * (b / 8), 0xcc);
    }
    void put(const JPEGDRAW *d) {
        int bytespp = d->iBpp / 8;
        for (int row = 0; row < d->iHeight; row++) {
            const uint8_t *src = (const uint8_t *)d->pPixels + (size_t)row * d->iWidth * bytespp;
            uint8_t *dst = px.data() + ((size_t)(d->y + row) * w + d->x) * bytespp;
            memcpy(dst, src, (size_t)d->iWidthUsed * bytespp);
        }
    }
};

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); g_failures++; } \
    else { printf("  ok:   %s\n", msg); } \
} while (0)

// pUser carries the JPEG_DUAL_PLANE descriptor during dual-output decodes, so
// the frame sink lives in this file-scope pointer (single-threaded test).
static Frame *g_frame = NULL;
static int capture_cb(JPEGDRAW *d)
{
    g_frame->put(d);
    return 1;
}

// Decode one full frame. Returns 1 on success.
static bool decode_frame(JPEGDEC &jpeg, const uint8_t *data, size_t len,
                         int pixel_type, int options, void *user, Frame &out)
{
    if (!jpeg.openRAM((uint8_t *)data, (int)len, capture_cb))
        return false;
    jpeg.setPixelType(pixel_type);
    int bytespp = (pixel_type == RGB8888) ? 4 : (pixel_type == EIGHT_BIT_GRAYSCALE ? 1 : 2);
    out.init(jpeg.getWidth(), jpeg.getHeight(), bytespp * 8);
    g_frame = &out;
    if (user) jpeg.setUserPointer(user);
    int rc = jpeg.decode(0, 0, options);
    jpeg.close();
    return rc != 0;
}

static void dump(const char *path, const Frame &f)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) { perror(path); exit(2); }
    fwrite(f.px.data(), 1, f.px.size(), fp);
    fclose(fp);
}

// Median microseconds for one open()+decode() cycle over iters runs. The
// median survives the scheduling jitter that dominates tiny-image timing.
static long bench(JPEGDEC &jpeg, const uint8_t *data, size_t len,
                  int pixel_type, int options, void *user, int iters)
{
    Frame tmp;
    g_frame = &tmp;
    if (!decode_frame(jpeg, data, len, pixel_type, options, user, tmp))
        return -1;
    std::vector<long> samples;
    samples.reserve(iters);
    for (int i = 0; i < iters; i++) {
        JPEGDEC one;
        long t0 = micros_now();
        if (!decode_frame(one, data, len, pixel_type, options, user, tmp)) {
            printf("    benchmark decode failed!\n");
            return -1;
        }
        samples.push_back(micros_now() - t0);
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

struct Img {
    const char *name;
    std::vector<uint8_t> data;
    int w, h, ss;
};

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "samples";
    const char *outdir = (argc > 2) ? argv[2] : "build";
    const int ITERS = 101;

    struct Sample { const char *name; const char *file; };
    Sample sample_list[] = {
        { "color420_31x17", "color_420_31x17.jpg" },
        { "color444_17x11", "color_444_17x11.jpg" },
        { "color422_19x9",  "color_422_19x9.jpg"  },
        { "gray_24x16",     "gray_24x16.jpg"      },
    };
    std::vector<Img> imgs;
    for (auto &s : sample_list) {
        Img im;
        im.name = s.name;
        im.data = read_file((std::string(dir) + "/" + s.file).c_str());
        imgs.push_back(std::move(im));
    }

    printf("sizeof(JPEGIMAGE) = %zu bytes (decoder-state footprint)\n", sizeof(JPEGIMAGE));
    for (auto &im : imgs) {
        JPEGDEC probe;
        if (!probe.openRAM(im.data.data(), (int)im.data.size(), capture_cb)) {
            printf("cannot open %s\n", im.name); return 2;
        }
        im.w = probe.getWidth(); im.h = probe.getHeight();
        im.ss = probe.getSubSample();
        probe.close();
        printf("image %-16s %2dx%-2d subsample=0x%02x jpeg=%zu bytes, Y plane=%d bytes (packed), %d bytes (pitch %d)\n",
               im.name, im.w, im.h, im.ss, im.data.size(),
               im.w * im.h, (im.w + 8) * im.h, im.w + 8);
    }

    // ================= baseline single-output dumps =================
    // Built against either library version; run.sh compares new vs old.
    for (auto &im : imgs) {
        struct Mode { int pt; int opt; const char *tag; };
        Mode modes[] = {
            { RGB565_LITTLE_ENDIAN, 0,                     "rgb565" },
            { RGB8888,              0,                     "rgb8888" },
            { EIGHT_BIT_GRAYSCALE,  JPEG_LUMA_ONLY,        "yonly_gray" },
            { RGB565_LITTLE_ENDIAN, JPEG_SCALE_HALF,       "rgb565_half" },
            { RGB565_LITTLE_ENDIAN, JPEG_SCALE_QUARTER,    "rgb565_quarter" },
            { RGB565_LITTLE_ENDIAN, JPEG_SCALE_EIGHTH,     "rgb565_eighth" },
        };
        for (auto &m : modes) {
            Frame f;
            JPEGDEC jd;
            if (!decode_frame(jd, im.data.data(), im.data.size(),
                              m.pt, m.opt, NULL, f)) {
                printf("  FAIL: baseline decode %s %s\n", im.name, m.tag);
                g_failures++;
                continue;
            }
            dump((std::string(outdir) + "/ref_" + im.name + "_" + m.tag + ".bin").c_str(), f);
        }
    }
    printf("single-output reference dumps written\n");

    // Against the pristine baseline library the dual-output code does not
    // exist; run.sh sets this to consume only the reference dumps.
    if (getenv("JPEGDEC_TEST_DUMPS_ONLY") != NULL) {
        printf("dumps-only mode: stopping before dual-output tests\n");
        return 0;
    }

    // ================= dual-output functional tests =================
    for (auto &im : imgs) {
        printf("\n== %s ==\n", im.name);
        const int PITCH = im.w + 8; // exercise a non-packed stride too
        std::vector<uint8_t> yplane((size_t)PITCH * im.h, 0xa5);
        std::vector<uint8_t> yplane_pack((size_t)im.w * im.h, 0x5a);

        // --- single-output references for pixel comparison ---
        Frame ref565, ref8888, refgray;
        {
            JPEGDEC j1, j2, j3;
            decode_frame(j1, im.data.data(), im.data.size(),
                         RGB565_LITTLE_ENDIAN, 0, NULL, ref565);
            decode_frame(j2, im.data.data(), im.data.size(),
                         RGB8888, 0, NULL, ref8888);
            decode_frame(j3, im.data.data(), im.data.size(),
                         EIGHT_BIT_GRAYSCALE, JPEG_LUMA_ONLY, NULL, refgray);
        }

        // --- dual: RGB565 + Y, padded pitch ---
        {
            JPEG_DUAL_PLANE dual = { JPEG_DUAL_MAGIC, yplane.data(), PITCH, 0, 0, 1 };
            Frame rgb; g_frame = &rgb;
            JPEGDEC j;
            bool ok = decode_frame(j, im.data.data(), im.data.size(),
                                   RGB565_LITTLE_ENDIAN, JPEG_DUAL_OUTPUT, &dual, rgb);
            CHECK(ok, "dual RGB565+Y decode succeeds");
            CHECK(dual.iYWidth == im.w && dual.iYHeight == im.h, "dual reports Y dimensions");
            CHECK(rgb.px == ref565.px, "dual RGB565 pixels identical to single output");
            bool ymatch = true;
            for (int yy = 0; yy < im.h && ymatch; yy++)
                for (int xx = 0; xx < im.w; xx++)
                    if (yplane[(size_t)yy * PITCH + xx] !=
                        refgray.px[(size_t)yy * refgray.w + xx]) { ymatch = false; break; }
            CHECK(ymatch, "dual Y plane identical to luma-only grayscale output");
            // untouched padding must remain at its fill pattern
            bool pad_ok = true;
            for (int yy = 0; yy < im.h; yy++)
                for (int xx = im.w; xx < PITCH; xx++)
                    if (yplane[(size_t)yy * PITCH + xx] != 0xa5) pad_ok = false;
            CHECK(pad_ok, "Y plane stride padding untouched");
        }

        // --- dual: RGB8888 + Y, packed pitch (0 => width) ---
        {
            JPEG_DUAL_PLANE dual = { JPEG_DUAL_MAGIC, yplane_pack.data(), 0, 0, 0, 1 };
            Frame rgb; g_frame = &rgb;
            JPEGDEC j;
            bool ok = decode_frame(j, im.data.data(), im.data.size(),
                                   RGB8888, JPEG_DUAL_OUTPUT, &dual, rgb);
            CHECK(ok, "dual RGB8888+Y decode succeeds (pitch 0 => packed)");
            CHECK(dual.iYPitch == im.w, "pitch 0 resolved to image width");
            CHECK(rgb.px == ref8888.px, "dual RGB8888 pixels identical to single output");
            bool ymatch = true;
            for (size_t i = 0; i < yplane_pack.size(); i++)
                if (yplane_pack[i] != refgray.px[i]) { ymatch = false; break; }
            CHECK(ymatch, "packed Y plane identical to grayscale output");
        }

        // --- dual: big-endian RGB565 + Y (all packed pixel types supported) ---
        {
            Frame refbe;
            {
                JPEGDEC jr;
                decode_frame(jr, im.data.data(), im.data.size(),
                             RGB565_BIG_ENDIAN, 0, NULL, refbe);
            }
            std::fill(yplane_pack.begin(), yplane_pack.end(), 0x5a);
            JPEG_DUAL_PLANE dual = { JPEG_DUAL_MAGIC, yplane_pack.data(), im.w, 0, 0, 1 };
            Frame rgb; g_frame = &rgb;
            JPEGDEC j;
            bool ok = decode_frame(j, im.data.data(), im.data.size(),
                                   RGB565_BIG_ENDIAN, JPEG_DUAL_OUTPUT, &dual, rgb);
            CHECK(ok, "dual RGB565-BE+Y decode succeeds");
            CHECK(rgb.px == refbe.px, "dual RGB565-BE pixels identical to single output");
            bool ymatch = true;
            for (size_t i = 0; i < yplane_pack.size(); i++)
                if (yplane_pack[i] != refgray.px[i]) { ymatch = false; break; }
            CHECK(ymatch, "RGB565-BE dual Y plane identical to grayscale output");
        }

        // --- dual configured RGB-only (bOutputY = 0): plane may be NULL ---
        {
            JPEG_DUAL_PLANE dual = { JPEG_DUAL_MAGIC, NULL, 0, 0, 0, 0 };
            Frame rgb; g_frame = &rgb;
            JPEGDEC j;
            bool ok = decode_frame(j, im.data.data(), im.data.size(),
                                   RGB565_LITTLE_ENDIAN, JPEG_DUAL_OUTPUT, &dual, rgb);
            CHECK(ok, "dual decode with bOutputY=0 succeeds without Y buffer");
            CHECK(rgb.px == ref565.px, "RGB-only dual path pixels identical to single output");
        }

        // --- Y-only decode (no color conversion at all) ---
        {
            std::vector<uint8_t> y((size_t)im.w * im.h, 0x11);
            JPEG_DUAL_PLANE dual = { JPEG_DUAL_MAGIC, y.data(), im.w, 0, 0, 1 };
            Frame gray; g_frame = &gray;
            JPEGDEC j;
            bool ok = decode_frame(j, im.data.data(), im.data.size(),
                                   EIGHT_BIT_GRAYSCALE,
                                   JPEG_LUMA_ONLY | JPEG_DUAL_OUTPUT, &dual, gray);
            CHECK(ok, "Y-only (LUMA_ONLY|DUAL) decode succeeds");
            bool ymatch = (gray.px == refgray.px);
            for (size_t i = 0; i < y.size() && ymatch; i++)
                if (y[i] != refgray.px[i]) ymatch = false;
            CHECK(ymatch, "Y-only plane identical to grayscale reference");
        }

        // ================= invalid layout parameters =================
        struct Bad { JPEG_DUAL_PLANE d; int pt; int opt; const char *msg; };
        std::vector<uint8_t> scratch((size_t)im.w * im.h);
        JPEG_DUAL_PLANE good = { JPEG_DUAL_MAGIC, scratch.data(), im.w, 0, 0, 1 };
        Bad bads[] = {
            { {0, NULL, 0, 0, 0, 0}, RGB565_LITTLE_ENDIAN, JPEG_DUAL_OUTPUT,
              "NULL user pointer rejected" },
            { {0xdeadbeef, scratch.data(), im.w, 0, 0, 1}, RGB565_LITTLE_ENDIAN,
              JPEG_DUAL_OUTPUT, "bad magic rejected" },
            { {JPEG_DUAL_MAGIC, NULL, im.w, 0, 0, 1}, RGB565_LITTLE_ENDIAN,
              JPEG_DUAL_OUTPUT, "bOutputY=1 with NULL Y plane rejected" },
            { {JPEG_DUAL_MAGIC, scratch.data(), -4, 0, 0, 1}, RGB565_LITTLE_ENDIAN,
              JPEG_DUAL_OUTPUT, "negative pitch rejected" },
            { {JPEG_DUAL_MAGIC, scratch.data(), im.w - 1, 0, 0, 1}, RGB565_LITTLE_ENDIAN,
              JPEG_DUAL_OUTPUT, "pitch smaller than width rejected" },
            { {JPEG_DUAL_MAGIC, scratch.data(), im.w, 0, 0, 2}, RGB565_LITTLE_ENDIAN,
              JPEG_DUAL_OUTPUT, "illegal bOutputY value rejected" },
            { good, RGB565_LITTLE_ENDIAN, JPEG_DUAL_OUTPUT | JPEG_SCALE_HALF,
              "dual output combined with scaling rejected" },
            { good, FOUR_BIT_DITHERED, JPEG_DUAL_OUTPUT,
              "dual output combined with dithered pixel type rejected" },
        };
        for (auto &b : bads) {
            Frame f; g_frame = &f;
            JPEGDEC j;
            bool opened = j.openRAM(im.data.data(), (int)im.data.size(), capture_cb);
            j.setPixelType(b.pt);
            j.setUserPointer(&b.d);
            int rc = opened ? j.decode(0, 0, b.opt) : 0;
            int err = j.getLastError();
            j.close();
            CHECK(rc == 0 && err == JPEG_INVALID_PARAMETER, b.msg);
        }
    }

    // ================= timing =================
    printf("\n== timing: median microseconds/open+decode over %d iterations (small images; see bench_tulips for stable numbers) ==\n", ITERS);
    printf("%-16s %-22s %10s %10s\n", "image", "mode", "median_us", "delta%");
    for (auto &im : imgs) {
        struct TMode { int pt; int opt; bool dual; bool yout; const char *tag; };
        TMode modes[] = {
            { RGB565_LITTLE_ENDIAN, 0,                    false, false, "single RGB565" },
            { RGB8888,              0,                    false, false, "single RGB8888" },
            { EIGHT_BIT_GRAYSCALE,  JPEG_LUMA_ONLY,       false, false, "Y-only (no dual)" },
            { RGB565_LITTLE_ENDIAN, JPEG_DUAL_OUTPUT,     true,  true,  "dual RGB565+Y" },
            { RGB8888,              JPEG_DUAL_OUTPUT,     true,  true,  "dual RGB8888+Y" },
            { RGB565_LITTLE_ENDIAN, JPEG_DUAL_OUTPUT,     true,  false, "dual RGB only" },
            { EIGHT_BIT_GRAYSCALE,  JPEG_LUMA_ONLY | JPEG_DUAL_OUTPUT, true, true, "Y-only dual plane" },
        };
        double base565 = 0;
        for (auto &m : modes) {
            std::vector<uint8_t> ybuf((size_t)(im.w + 8) * im.h);
            JPEG_DUAL_PLANE dual = { JPEG_DUAL_MAGIC, ybuf.data(), im.w + 8, 0, 0, m.yout ? 1 : 0 };
            Frame f; g_frame = &f;
            JPEGDEC j;
            long med = bench(j, im.data.data(), im.data.size(), m.pt, m.opt,
                             m.dual ? &dual : NULL, ITERS);
            if (strcmp(m.tag, "single RGB565") == 0) base565 = (double)med;
            double delta = base565 ? (med - base565) * 100.0 / base565 : 0.0;
            printf("%-16s %-22s %10ld %+9.1f%%\n", im.name, m.tag, med, delta);
        }
    }

    printf("\n%s\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
    return g_failures ? 1 : 0;
}
