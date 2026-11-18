#!/bin/sh
# One-shot build + run for the strip-decode test.
# - generates the three sample JPEGs (self-contained encoder, no deps)
# - builds the unmodified JPEGDEC library from ../../src
# - builds the comparison harness with allocator instrumentation
# - runs baseline (full framebuffer) vs strip (row-reuse buffer) decode
# - finishes with the repo's existing Linux examples as a regression check
#
# Nothing here touches the repo's build files; all artifacts go to ./build
# and ./samples.
set -e
cd "$(dirname "$0")"
ROOT=../..
mkdir -p build samples

echo "=== 1/4 generate samples ==="
cc -O2 -Wall -c jpeg_enc.c -o build/jpeg_enc.o
cc -O2 -Wall gen_samples.c build/jpeg_enc.o -lm -o build/gen_samples
./build/gen_samples samples

echo "=== 2/4 build JPEGDEC (unmodified) + harness ==="
c++ -O2 -Wall -D__LINUX__ -I$ROOT/src -c $ROOT/src/JPEGDEC.cpp -o build/JPEGDEC.o
c++ -O2 -Wall -c tracked_alloc.cpp -o build/tracked_alloc.o
c++ -O2 -Wall -D__LINUX__ -I$ROOT/src -I. main.cpp build/JPEGDEC.o build/tracked_alloc.o \
    -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free \
    -o build/strip_test

echo "=== 3/4 run strip-decode comparison ==="
./build/strip_test samples

echo "=== 4/4 repo's existing tests (linux examples) ==="
c++ -O2 -Wall -D__LINUX__ -I$ROOT/src $ROOT/linux/examples/jpeg_perf_test/main.cpp \
    build/JPEGDEC.o -o build/jpeg_perf_test
./build/jpeg_perf_test
cc -O2 -Wall -D__LINUX__ $ROOT/linux/examples/c_cmdline/main.c -lm -o build/c_cmdline
./build/c_cmdline $ROOT/demo.jpg build/demo_out.bmp
ls -l build/demo_out.bmp
echo "existing tests OK"
