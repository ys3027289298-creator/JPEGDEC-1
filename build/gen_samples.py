#!/usr/bin/env python3
"""Generate tiny baseline JPEG samples for the MCU recovery tests.

Standard library only. Two 48x40 YCbCr 4:2:0 baseline images are emitted:
one without restart markers and one with a DRI of 2 MCUs. The C test harness
corrupts copies of these files at runtime; this script only writes the clean
references plus a grayscale variant used for pixel-identity checks.
"""
import math
import os
import struct
import sys

# Standard Annex K luminance/chrominance quantization tables
Q_LUM = [
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99,
]
Q_CHR = [
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
]
ZIGZAG = [
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
]


def build_huff_table(counts, symbols):
    """Return (category -> (code, length), encode table for coefficients)."""
    table = {}
    code = 0
    k = 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            table[symbols[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    return table


# Standard JPEG Huffman tables (Annex K.3)
DC_BITS = [0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0]
DC_SYMS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
AC_BITS = [0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d]
AC_SYMS = [
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
    0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
]
DC_ENC = build_huff_table(DC_BITS, DC_SYMS)
AC_ENC = build_huff_table(AC_BITS, AC_SYMS)

# Annex K.3 chrominance tables (table id 1). The chroma DC table in the
# reference data set happens to use the same code lengths as the luma DC
# table but starts with a different symbol ordering.
DC1_BITS = [0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0]
DC1_SYMS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
AC1_BITS = [0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77]
AC1_SYMS = [
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
    0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
    0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
]
DC1_ENC = build_huff_table(DC1_BITS, DC1_SYMS)
AC1_ENC = build_huff_table(AC1_BITS, AC1_SYMS)


class BitWriter:
    def __init__(self):
        self.buf = bytearray()
        self.acc = 0
        self.nbits = 0

    def write(self, value, length):
        self.acc = (self.acc << length) | (value & ((1 << length) - 1))
        self.nbits += length
        while self.nbits >= 8:
            self.nbits -= 8
            byte = (self.acc >> self.nbits) & 0xff
            self.buf.append(byte)
            if byte == 0xff:
                self.buf.append(0x00)  # JPEG byte stuffing

    def flush(self):
        if self.nbits:
            pad = 8 - self.nbits
            self.write((1 << pad) - 1, pad)  # pad with 1-bits

    def marker(self, marker):
        self.buf.append(0xff)
        self.buf.append(marker)


def dct_1d(block):
    out = [0.0] * 8
    for u in range(8):
        s = 0.0
        for x in range(8):
            s += block[x] * math.cos((2 * x + 1) * u * math.pi / 16.0)
        cu = 0.70710678 if u == 0 else 1.0
        out[u] = 0.5 * cu * s
    return out


def fdct(block8):
    # separable 2D DCT: transform rows first, then columns
    tmp = [dct_1d(block8[r * 8:(r + 1) * 8]) for r in range(8)]
    out = [0] * 64
    for u in range(8):
        col = [tmp[r][u] for r in range(8)]
        col_t = dct_1d(col)
        for v in range(8):
            out[v * 8 + u] = int(round(col_t[v]))
    return out


def category(value):
    if value == 0:
        return 0, 0
    mag = abs(value)
    cat = mag.bit_length()
    if value < 0:
        code = value - 1 + (1 << cat)
    else:
        code = value
    return cat, code & ((1 << cat) - 1)


def encode_block(bw, coeffs, qtable, dc_pred, dc_enc=None, ac_enc=None):
    if dc_enc is None:
        dc_enc = DC_ENC
    if ac_enc is None:
        ac_enc = AC_ENC
    # Entropy coding scans the coefficient array in zigzag order, so the
    # element at array position k must hold the coefficient whose natural
    # 8x8 index is ZIGZAG[k].
    quant = [0] * 64
    for k in range(64):
        quant[k] = int(round(coeffs[ZIGZAG[k]] / qtable[ZIGZAG[k]]))
    # DC
    diff = quant[0] - dc_pred
    cat, bits = category(diff)
    code, length = dc_enc[cat]
    bw.write(code, length)
    if cat:
        bw.write(bits, cat)
    # AC
    run = 0
    for k in range(1, 64):
        val = quant[k]
        if val == 0:
            run += 1
            continue
        while run > 15:
            c, l = ac_enc[0xf0]
            bw.write(c, l)
            run -= 16
        cat, bits = category(val)
        c, l = ac_enc[(run << 4) | cat]
        bw.write(c, l)
        bw.write(bits, cat)
        run = 0
    c, l = ac_enc[0x00]  # EOB
    bw.write(c, l)
    return quant[0]


def marker_segment(marker, payload):
    return bytes([0xff, marker]) + struct.pack(">H", len(payload) + 2) + payload


def make_image(width, height, gray=False):
    """Return Y (and downsampled Cb/Cr) sample planes, edge-padded to 16."""
    mcu_w = (width + 15) // 16
    mcu_h = (height + 15) // 16
    pw, ph = mcu_w * 16, mcu_h * 16
    ys = [[0] * pw for _ in range(ph)]
    cbs = [[0] * (pw // 2) for _ in range(ph // 2)]
    crs = [[0] * (pw // 2) for _ in range(ph // 2)]
    for r in range(ph):
        for cidx in range(pw):
            # smooth gradient + banding: easy to see, easy to compress
            y = 40 + (cidx * 150 // max(1, pw - 1))
            y = (y + 30 * int(r / 8)) & 0xff
            ys[r][cidx] = y
            if not gray and r % 2 == 0 and cidx % 2 == 0:
                cb = 128 + 40 * math.sin(cidx / 6.0)
                cr = 128 + 40 * math.cos(r / 6.0)
                cbs[r // 2][cidx // 2] = int(cb)
                crs[r // 2][cidx // 2] = int(cr)
    if gray:
        return ys, None, None, mcu_w, mcu_h
    return ys, cbs, crs, mcu_w, mcu_h


def encode_jpeg(width, height, out_path, dri=0, gray=False):
    ys, cbs, crs, mcu_w, mcu_h = make_image(width, height, gray)
    bw = BitWriter()
    bw.marker(0xd8)  # SOI
    # APP0 JFIF
    app0 = b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00"
    bw.buf += marker_segment(0xe0, app0)
    # DQT
    if gray:
        payload = bytes([0x00]) + bytes(Q_LUM)
        bw.buf += marker_segment(0xdb, payload)
    else:
        payload = bytes([0x00]) + bytes(Q_LUM) + bytes([0x01]) + bytes(Q_CHR)
        bw.buf += marker_segment(0xdb, payload)
    # SOF0
    ncomp = 1 if gray else 3
    sof = struct.pack(">BHHB", 8, height, width, ncomp)
    if gray:
        sof += bytes([1, 0x11, 0])
    else:
        sof += bytes([1, 0x22, 0]) + bytes([2, 0x11, 1]) + bytes([3, 0x11, 1])
    bw.buf += marker_segment(0xc0, sof)
    # DHT - luma tables (id 0) plus chroma tables (id 1) for color images
    bw.buf += marker_segment(0xc4, bytes([0x00]) + bytes(DC_BITS) + bytes(DC_SYMS))
    bw.buf += marker_segment(0xc4, bytes([0x10]) + bytes(AC_BITS) + bytes(AC_SYMS))
    if not gray:
        bw.buf += marker_segment(0xc4, bytes([0x01]) + bytes(DC1_BITS) + bytes(DC1_SYMS))
        bw.buf += marker_segment(0xc4, bytes([0x11]) + bytes(AC1_BITS) + bytes(AC1_SYMS))
    if dri:
        bw.buf += marker_segment(0xdd, struct.pack(">H", dri))
    # SOS
    if gray:
        sos = bytes([1, 1, 0x00, 0, 63, 0])
    else:
        sos = bytes([3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0])
    bw.buf += marker_segment(0xda, sos)
    # entropy data
    dc_pred = [0, 0, 0]
    mcu_total = mcu_w * mcu_h
    mcu_index = 0
    rst = 0
    for my in range(mcu_h):
        for mx in range(mcu_w):
            if gray:
                for by in range(2):
                    for bx in range(2):
                        block = []
                        for r in range(8):
                            for cidx in range(8):
                                block.append(ys[my * 16 + by * 8 + r][mx * 16 + bx * 8 + cidx] - 128)
                        dc_pred[0] = encode_block(bw, fdct(block), Q_LUM, dc_pred[0])
            else:
                # 4 Y blocks
                for by in range(2):
                    for bx in range(2):
                        block = []
                        for r in range(8):
                            for cidx in range(8):
                                block.append(ys[my * 16 + by * 8 + r][mx * 16 + bx * 8 + cidx] - 128)
                        dc_pred[0] = encode_block(bw, fdct(block), Q_LUM, dc_pred[0])
                # Cb, Cr (one 8x8 each, duplicated samples from 16x16)
                for plane, pi in ((cbs, 1), (crs, 2)):
                    block = []
                    for r in range(8):
                        for cidx in range(8):
                            block.append(plane[my * 8 + r][mx * 8 + cidx] - 128)
                    dc_pred[pi] = encode_block(bw, fdct(block), Q_CHR,
                                               dc_pred[pi], DC1_ENC, AC1_ENC)
            mcu_index += 1
            if dri and (mcu_index % dri) == 0 and mcu_index != mcu_total:
                bw.flush()
                bw.marker(0xd0 | (rst & 7))
                rst += 1
                dc_pred = [0, 0, 0]
    bw.flush()
    bw.marker(0xd9)  # EOI
    with open(out_path, "wb") as f:
        f.write(bw.buf)
    print(f"wrote {out_path}: {len(bw.buf)} bytes, "
          f"{mcu_w}x{mcu_h} MCUs, dri={dri}, gray={gray}")


if __name__ == "__main__":
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(outdir, exist_ok=True)
    encode_jpeg(48, 40, os.path.join(outdir, "sample_clean.jpg"), dri=0)
    encode_jpeg(48, 40, os.path.join(outdir, "sample_rst.jpg"), dri=2)
    encode_jpeg(32, 24, os.path.join(outdir, "sample_gray.jpg"), dri=0, gray=True)
