#!/usr/bin/env python3
# Build tiny JPEG fixtures with an EXIF APP1 segment carrying a specific
# Orientation tag (TIFF tag 274). Pure stdlib, no third-party dependency.
import struct
import sys


def build_app1(orientation, motorola):
    endian = b"MM" if motorola else b"II"
    bo = ">" if motorola else "<"
    # TIFF header: byte order, magic 42, offset to IFD0 (8 bytes)
    tiff = endian + struct.pack(bo + "HI", 42, 8)
    # IFD0: 1 entry (orientation), then next-IFD offset = 0
    ifd = struct.pack(bo + "H", 1)
    # tag=274 Orientation, type=3 (SHORT), count=1. A SHORT value is
    # stored in the first 2 bytes of the 4-byte value field, padded with
    # two zero bytes (per TIFF 6.0).
    # 12-byte entry: tag(H) type(H) count(L) value(2 bytes SHORT + 2 pad)
    ifd += struct.pack(bo + "HHIH", 274, 3, 1, orientation)
    ifd += b"\x00\x00"
    ifd += struct.pack(bo + "I", 0)
    tiff += ifd
    payload = b"Exif\x00\x00" + tiff
    return b"\xff\xe1" + struct.pack(">H", len(payload) + 2) + payload


def inject(base, out, orientation, motorola=False):
    data = open(base, "rb").read()
    assert data[:2] == b"\xff\xd8", "input must be a JPEG"
    app1 = build_app1(orientation, motorola)
    open(out, "wb").write(data[:2] + app1 + data[2:])


if __name__ == "__main__":
    base = sys.argv[1]
    jobs = [
        ("orient_1.jpg", 1, False),
        ("orient_3.jpg", 3, False),
        ("orient_6.jpg", 6, False),
        ("orient_8.jpg", 8, True),   # exercise Motorola byte order too
        ("orient_9_invalid.jpg", 9, False),
        ("orient_2_mirror.jpg", 2, False),
    ]
    for name, orient, moto in jobs:
        inject(base, name, orient, moto)
        print("wrote", name, "orientation =", orient,
              "byte order =", "MM" if moto else "II")
