#!/bin/sh
# One-shot build+run of the strip-decode test. No repo build files are touched.
set -e
cd "$(dirname "$0")"
python3 gen_images.py
g++ -O2 -Wall -D__LINUX__ -DTESTDIR='"."' -I../src \
    strip_test.cpp ../src/JPEGDEC.cpp \
    -Wl,--wrap=malloc -Wl,--wrap=free -o strip_test
./strip_test
