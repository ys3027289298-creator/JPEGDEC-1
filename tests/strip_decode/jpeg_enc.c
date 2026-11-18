//
// Minimal baseline JPEG encoder - see jpeg_enc.h for the API.
// Written from scratch for the JPEGDEC strip-decode tests; no external code.
//
#include "jpeg_enc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// JPEG standard (Annex K) quantization and Huffman tables
// ---------------------------------------------------------------------------
static const uint8_t std_qt_luma[64] = {
    16,11,10,16, 24, 40, 51, 61,
    12,12,14,19, 26, 58, 60, 55,
    14,13,16,24, 40, 57, 69, 56,
    14,17,22,29, 51, 87, 80, 62,
    18,22,37,56, 68,109,103, 77,
    24,35,55,64, 81,104,113, 92,
    49,64,78,87,103,121,120,101,
    72,92,95,98,112,100,103, 99
};
static const uint8_t std_qt_chroma[64] = {
    17,18,24,47, 99, 99, 99, 99,
    18,21,26,66, 99, 99, 99, 99,
    24,26,56,99, 99, 99, 99, 99,
    47,66,99,99, 99, 99, 99, 99,
    99,99,99,99, 99, 99, 99, 99,
    99,99,99,99, 99, 99, 99, 99,
    99,99,99,99, 99, 99, 99, 99,
    99,99,99,99, 99, 99, 99, 99
};
// zigzag scan order: position in zigzag sequence -> natural (row-major) index
static const uint8_t zigzag[64] = {
     0, 1, 8,16, 9, 2, 3,10,
    17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,
    27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,
    29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};
static const uint8_t std_dc_luma_bits[16]   = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t std_dc_luma_vals[12]   = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t std_dc_chroma_bits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t std_dc_chroma_vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t std_ac_luma_bits[16]   = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t std_ac_luma_vals[162]  = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,
    0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,
    0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,
    0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,
    0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
    0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,
    0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,
    0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
    0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,
    0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,
    0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};
static const uint8_t std_ac_chroma_bits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t std_ac_chroma_vals[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,
    0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,
    0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,
    0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,
    0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,
    0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,
    0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,
    0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,
    0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,
    0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,
    0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

// ---------------------------------------------------------------------------
// Huffman table (derived codes from bits/vals)
// ---------------------------------------------------------------------------
typedef struct {
    uint16_t code[256];
    uint8_t  size[256];
} hufftab_t;

static void build_huff(hufftab_t *t, const uint8_t *bits, const uint8_t *vals, int nvals)
{
    int i, j, k = 0;
    uint16_t code = 0;
    memset(t, 0, sizeof(*t));
    for (i = 0; i < 16; i++) {
        for (j = 0; j < bits[i]; j++) {
            if (k >= nvals) return;
            t->code[vals[k]] = code;
            t->size[vals[k]] = (uint8_t)(i + 1);
            code++;
            k++;
        }
        code <<= 1;
    }
}

// ---------------------------------------------------------------------------
// Bit writer with 0xFF byte stuffing, growing byte buffer
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t *buf;
    size_t len, cap;
    uint32_t acc;
    int nbits;
} bitw_t;

static int bw_reserve(bitw_t *w, size_t extra)
{
    if (w->len + extra <= w->cap) return 0;
    size_t ncap = w->cap ? w->cap * 2 : 65536;
    while (ncap < w->len + extra) ncap *= 2;
    uint8_t *nb = (uint8_t *)realloc(w->buf, ncap);
    if (!nb) return -1;
    w->buf = nb;
    w->cap = ncap;
    return 0;
}
static int bw_byte(bitw_t *w, int b)
{
    if (bw_reserve(w, 2)) return -1;
    w->buf[w->len++] = (uint8_t)b;
    if (b == 0xFF) w->buf[w->len++] = 0x00; // byte stuffing
    return 0;
}
static int bw_raw(bitw_t *w, int b) // marker bytes: no stuffing
{
    if (bw_reserve(w, 1)) return -1;
    w->buf[w->len++] = (uint8_t)b;
    return 0;
}
static int bw_bits(bitw_t *w, uint32_t code, int n)
{
    if (n == 0) return 0;
    w->acc = (w->acc << n) | (code & ((n >= 32) ? 0xFFFFFFFFu : ((1u << n) - 1)));
    w->nbits += n;
    while (w->nbits >= 8) {
        w->nbits -= 8;
        if (bw_byte(w, (w->acc >> w->nbits) & 0xFF)) return -1;
    }
    return 0;
}
static int bw_flush_pad1(bitw_t *w) // pad with 1-bits to byte boundary
{
    if (w->nbits > 0) {
        int pad = 8 - w->nbits;
        if (bw_bits(w, (1u << pad) - 1, pad)) return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// DCT (separable, float) and quantization
// ---------------------------------------------------------------------------
static double cos_tbl[8][8];
static int cos_init_done = 0;
static void init_cos(void)
{
    int u, x;
    if (cos_init_done) return;
    for (u = 0; u < 8; u++)
        for (x = 0; x < 8; x++)
            cos_tbl[u][x] = cos((2.0 * x + 1.0) * u * M_PI / 16.0);
    cos_init_done = 1;
}

// forward DCT of an 8x8 block of level-shifted samples, output natural order
// F[v][u] = 0.25*C(u)*C(v) * sum f[y][x]*cos((2x+1)u*pi/16)*cos((2y+1)v*pi/16)
static void fdct(const double in[64], double out[64])
{
    double tmp[64];
    int u, v, x, y;
    for (y = 0; y < 8; y++) {           // row pass
        for (u = 0; u < 8; u++) {
            double s = 0;
            for (x = 0; x < 8; x++) s += in[y * 8 + x] * cos_tbl[u][x];
            tmp[y * 8 + u] = s;
        }
    }
    for (v = 0; v < 8; v++) {           // column pass
        double cv = (v == 0) ? (1.0 / 1.4142135623730951) : 1.0;
        for (u = 0; u < 8; u++) {
            double s = 0;
            double cu = (u == 0) ? (1.0 / 1.4142135623730951) : 1.0;
            for (y = 0; y < 8; y++) s += tmp[y * 8 + u] * cos_tbl[v][y];
            out[v * 8 + u] = 0.25 * cu * cv * s;
        }
    }
}

// ---------------------------------------------------------------------------
// Encoder state
// ---------------------------------------------------------------------------
typedef struct {
    bitw_t bw;
    hufftab_t dc[2], ac[2];
    int qt[2][64];        // scaled quant tables, natural order
    int dc_pred[3];
    int restart_interval;
    int mcu_since_restart;
    int rst_num;
} enc_t;

static int emit_marker(enc_t *e, int m)
{
    if (bw_raw(&e->bw, 0xFF) || bw_raw(&e->bw, m)) return -1;
    return 0;
}
static int emit_u16(enc_t *e, int v)
{
    if (bw_raw(&e->bw, (v >> 8) & 0xFF) || bw_raw(&e->bw, v & 0xFF)) return -1;
    return 0;
}

static int encode_block(enc_t *e, const double samples[64], int comp,
                        int dctab, int actab)
{
    double freq[64];
    int zz[64];
    int i, diff, nbits;
    fdct(samples, freq);
    for (i = 0; i < 64; i++) {
        double q = freq[i] / e->qt[comp ? 1 : 0][i];
        zz[zigzag[i]] = (int)(q < 0 ? q - 0.5 : q + 0.5); // round to nearest
    }
    // DC
    diff = zz[0] - e->dc_pred[comp];
    e->dc_pred[comp] = zz[0];
    nbits = 0;
    {
        int a = diff < 0 ? -diff : diff;
        while (a) { nbits++; a >>= 1; }
    }
    if (bw_bits(&e->bw, e->dc[dctab].code[nbits], e->dc[dctab].size[nbits])) return -1;
    if (nbits) {
        uint32_t v = (diff < 0) ? (uint32_t)(diff + (1 << nbits) - 1) : (uint32_t)diff;
        if (bw_bits(&e->bw, v, nbits)) return -1;
    }
    // AC: run-length
    {
        int run = 0;
        for (i = 1; i < 64; i++) {
            if (zz[i] == 0) { run++; continue; }
            while (run > 15) {
                if (bw_bits(&e->bw, e->ac[actab].code[0xF0], e->ac[actab].size[0xF0])) return -1;
                run -= 16;
            }
            {
                int v = zz[i], nb = 0, a = v < 0 ? -v : v;
                while (a) { nb++; a >>= 1; }
                if (bw_bits(&e->bw, e->ac[actab].code[(run << 4) | nb], e->ac[actab].size[(run << 4) | nb])) return -1;
                {
                    uint32_t enc = (v < 0) ? (uint32_t)(v + (1 << nb) - 1) : (uint32_t)v;
                    if (bw_bits(&e->bw, enc, nb)) return -1;
                }
            }
            run = 0;
        }
        if (run > 0) { // EOB
            if (bw_bits(&e->bw, e->ac[actab].code[0x00], e->ac[actab].size[0x00])) return -1;
        }
    }
    return 0;
}

static int do_restart(enc_t *e)
{
    if (bw_flush_pad1(&e->bw)) return -1;
    if (emit_marker(e, 0xD0 + (e->rst_num & 7))) return -1;
    e->rst_num++;
    e->dc_pred[0] = e->dc_pred[1] = e->dc_pred[2] = 0;
    e->mcu_since_restart = 0;
    return 0;
}

// sample planes (level-shifted) with clamped edge access
static double get_sample(const double *plane, int w, int h, int x, int y)
{
    if (x < 0) x = 0; else if (x >= w) x = w - 1;
    if (y < 0) y = 0; else if (y >= h) y = h - 1;
    return plane[y * w + x];
}

static int encode_image(enc_t *e, const double *py, const double *pcb, const double *pcr,
                        int w, int h, int subsample)
{
    int mcux, mcuy, mx, my, bx, by, i, j;
    int mcus_per_row, mcu_rows;
    double blk[64];

    if (subsample == JPE_SUBSAMPLE_420) { mcux = 16; mcuy = 16; }
    else                                { mcux = 8;  mcuy = 8;  }
    mcus_per_row = (w + mcux - 1) / mcux;
    mcu_rows = (h + mcuy - 1) / mcuy;

    for (my = 0; my < mcu_rows; my++) {
        for (mx = 0; mx < mcus_per_row; mx++) {
            if (e->restart_interval && e->mcu_since_restart == e->restart_interval) {
                if (do_restart(e)) return -1;
            }
            if (subsample == JPE_SUBSAMPLE_420) {
                for (i = 0; i < 2; i++) for (j = 0; j < 2; j++) {
                    for (by = 0; by < 8; by++) for (bx = 0; bx < 8; bx++)
                        blk[by * 8 + bx] = get_sample(py, w, h, mx * 16 + j * 8 + bx, my * 16 + i * 8 + by);
                    if (encode_block(e, blk, 0, 0, 0)) return -1;
                }
                // chroma: average 2x2 of the full-res plane
                for (by = 0; by < 8; by++) for (bx = 0; bx < 8; bx++) {
                    int sx = mx * 16 + bx * 2, sy = my * 16 + by * 2;
                    blk[by * 8 + bx] = 0.25 * (get_sample(pcb, w, h, sx, sy) +
                                               get_sample(pcb, w, h, sx + 1, sy) +
                                               get_sample(pcb, w, h, sx, sy + 1) +
                                               get_sample(pcb, w, h, sx + 1, sy + 1));
                }
                if (encode_block(e, blk, 1, 1, 1)) return -1;
                for (by = 0; by < 8; by++) for (bx = 0; bx < 8; bx++) {
                    int sx = mx * 16 + bx * 2, sy = my * 16 + by * 2;
                    blk[by * 8 + bx] = 0.25 * (get_sample(pcr, w, h, sx, sy) +
                                               get_sample(pcr, w, h, sx + 1, sy) +
                                               get_sample(pcr, w, h, sx, sy + 1) +
                                               get_sample(pcr, w, h, sx + 1, sy + 1));
                }
                if (encode_block(e, blk, 2, 1, 1)) return -1;
            } else { // 4:4:4
                for (by = 0; by < 8; by++) for (bx = 0; bx < 8; bx++)
                    blk[by * 8 + bx] = get_sample(py, w, h, mx * 8 + bx, my * 8 + by);
                if (encode_block(e, blk, 0, 0, 0)) return -1;
                for (by = 0; by < 8; by++) for (bx = 0; bx < 8; bx++)
                    blk[by * 8 + bx] = get_sample(pcb, w, h, mx * 8 + bx, my * 8 + by);
                if (encode_block(e, blk, 1, 1, 1)) return -1;
                for (by = 0; by < 8; by++) for (bx = 0; bx < 8; bx++)
                    blk[by * 8 + bx] = get_sample(pcr, w, h, mx * 8 + bx, my * 8 + by);
                if (encode_block(e, blk, 2, 1, 1)) return -1;
            }
            e->mcu_since_restart++;
        }
    }
    return bw_flush_pad1(&e->bw);
}

static void scale_qt(int *dst, const uint8_t *src, int quality)
{
    int i, scale;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    scale = (quality < 50) ? (5000 / quality) : (200 - quality * 2);
    for (i = 0; i < 64; i++) {
        int v = (src[i] * scale + 50) / 100;
        if (v < 1) v = 1;
        if (v > 255) v = 255;
        dst[i] = v;
    }
}

static int emit_header(enc_t *e, int w, int h, int subsample, int restart_interval)
{
    int i;
    // SOI
    if (emit_marker(e, 0xD8)) return -1;
    // APP0 JFIF
    if (emit_marker(e, 0xE0) || emit_u16(e, 16)) return -1;
    {
        const char *jfif = "JFIF";
        for (i = 0; jfif[i]; i++) if (bw_raw(&e->bw, jfif[i])) return -1;
        if (bw_raw(&e->bw, 0) || bw_raw(&e->bw, 1) || bw_raw(&e->bw, 1) || bw_raw(&e->bw, 0)) return -1;
        if (emit_u16(e, 1) || emit_u16(e, 1)) return -1; // density
        if (bw_raw(&e->bw, 0) || bw_raw(&e->bw, 0)) return -1; // no thumbnail
    }
    // DQT (two tables)
    if (emit_marker(e, 0xDB) || emit_u16(e, 2 + 2 * (1 + 64))) return -1;
    if (bw_raw(&e->bw, 0)) return -1; // table 0: 8-bit precision, id 0
    for (i = 0; i < 64; i++) if (bw_raw(&e->bw, e->qt[0][zigzag[i]])) return -1;
    if (bw_raw(&e->bw, 1)) return -1; // table 1
    for (i = 0; i < 64; i++) if (bw_raw(&e->bw, e->qt[1][zigzag[i]])) return -1;
    // SOF0
    if (emit_marker(e, 0xC0) || emit_u16(e, 8 + 3 * 3)) return -1;
    if (bw_raw(&e->bw, 8) || emit_u16(e, h) || emit_u16(e, w) || bw_raw(&e->bw, 3)) return -1;
    if (bw_raw(&e->bw, 1) || bw_raw(&e->bw, subsample == JPE_SUBSAMPLE_420 ? 0x22 : 0x11) || bw_raw(&e->bw, 0)) return -1;
    if (bw_raw(&e->bw, 2) || bw_raw(&e->bw, 0x11) || bw_raw(&e->bw, 1)) return -1;
    if (bw_raw(&e->bw, 3) || bw_raw(&e->bw, 0x11) || bw_raw(&e->bw, 1)) return -1;
    // DRI
    if (restart_interval > 0) {
        if (emit_marker(e, 0xDD) || emit_u16(e, 4) || emit_u16(e, restart_interval)) return -1;
    }
    // DHT: DC luma, AC luma, DC chroma, AC chroma
    if (emit_marker(e, 0xC4)) return -1;
    {
        int len = 2;
        len += 1 + 16 + 12;  // dc luma
        len += 1 + 16 + 162; // ac luma
        len += 1 + 16 + 12;  // dc chroma
        len += 1 + 16 + 162; // ac chroma
        if (emit_u16(e, len)) return -1;
        if (bw_raw(&e->bw, 0x00)) return -1;
        for (i = 0; i < 16; i++) if (bw_raw(&e->bw, std_dc_luma_bits[i])) return -1;
        for (i = 0; i < 12; i++) if (bw_raw(&e->bw, std_dc_luma_vals[i])) return -1;
        if (bw_raw(&e->bw, 0x10)) return -1;
        for (i = 0; i < 16; i++) if (bw_raw(&e->bw, std_ac_luma_bits[i])) return -1;
        for (i = 0; i < 162; i++) if (bw_raw(&e->bw, std_ac_luma_vals[i])) return -1;
        if (bw_raw(&e->bw, 0x01)) return -1;
        for (i = 0; i < 16; i++) if (bw_raw(&e->bw, std_dc_chroma_bits[i])) return -1;
        for (i = 0; i < 12; i++) if (bw_raw(&e->bw, std_dc_chroma_vals[i])) return -1;
        if (bw_raw(&e->bw, 0x11)) return -1;
        for (i = 0; i < 16; i++) if (bw_raw(&e->bw, std_ac_chroma_bits[i])) return -1;
        for (i = 0; i < 162; i++) if (bw_raw(&e->bw, std_ac_chroma_vals[i])) return -1;
    }
    // SOS
    if (emit_marker(e, 0xDA) || emit_u16(e, 6 + 2 * 3) || bw_raw(&e->bw, 3)) return -1;
    if (bw_raw(&e->bw, 1) || bw_raw(&e->bw, 0x00)) return -1;
    if (bw_raw(&e->bw, 2) || bw_raw(&e->bw, 0x11)) return -1;
    if (bw_raw(&e->bw, 3) || bw_raw(&e->bw, 0x11)) return -1;
    if (bw_raw(&e->bw, 0) || bw_raw(&e->bw, 63) || bw_raw(&e->bw, 0)) return -1;
    return 0;
}

int jpe_encode_file(const char *path, const uint8_t *rgb, int w, int h,
                    int quality, int subsample, int restart_interval)
{
    enc_t e;
    double *py, *pcb, *pcr;
    int x, y, rc = -1;
    FILE *f;
    size_t npix = (size_t)w * (size_t)h;

    init_cos();
    memset(&e, 0, sizeof(e));
    e.restart_interval = restart_interval;
    scale_qt(e.qt[0], std_qt_luma, quality);
    scale_qt(e.qt[1], std_qt_chroma, quality);
    build_huff(&e.dc[0], std_dc_luma_bits, std_dc_luma_vals, 12);
    build_huff(&e.dc[1], std_dc_chroma_bits, std_dc_chroma_vals, 12);
    build_huff(&e.ac[0], std_ac_luma_bits, std_ac_luma_vals, 162);
    build_huff(&e.ac[1], std_ac_chroma_bits, std_ac_chroma_vals, 162);

    py  = (double *)malloc(npix * sizeof(double));
    pcb = (double *)malloc(npix * sizeof(double));
    pcr = (double *)malloc(npix * sizeof(double));
    if (!py || !pcb || !pcr) goto done;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const uint8_t *p = &rgb[((size_t)y * w + x) * 3];
            double r = p[0], g = p[1], b = p[2];
            double yy =  0.299 * r + 0.587 * g + 0.114 * b;
            double cb = -0.168735892 * r - 0.331264108 * g + 0.5 * b + 128.0;
            double cr =  0.5 * r - 0.418687589 * g - 0.081312411 * b + 128.0;
            size_t o = (size_t)y * w + x;
            py[o]  = yy - 128.0;
            pcb[o] = cb - 128.0;
            pcr[o] = cr - 128.0;
        }
    }

    if (emit_header(&e, w, h, subsample, restart_interval)) goto done;
    if (encode_image(&e, py, pcb, pcr, w, h, subsample)) goto done;
    if (emit_marker(&e, 0xD9)) goto done; // EOI

    f = fopen(path, "wb");
    if (!f) goto done;
    if (fwrite(e.bw.buf, 1, e.bw.len, f) != e.bw.len) { fclose(f); goto done; }
    fclose(f);
    rc = 0;
done:
    free(py); free(pcb); free(pcr);
    free(e.bw.buf);
    return rc;
}
