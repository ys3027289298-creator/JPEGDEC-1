#!/bin/sh
# One-shot test runner: Huffman cache tests + the repo's existing tests.
# Does not modify any existing build script; the perf test is linked
# directly against ../src/JPEGDEC.cpp because its Makefile expects the
# library to be installed system-wide (sudo make in linux/).
set -e
cd "$(dirname "$0")"
ROOT=..

echo "=== [1/3] Huffman table cache tests ==="
g++ -O2 -Wall -D__LINUX__ -I$ROOT/src huffcache_test.cpp $ROOT/src/JPEGDEC.cpp -o huffcache_test
./huffcache_test

echo ""
echo "=== [2/3] Existing test: linux/examples/c_cmdline ==="
(cd $ROOT/linux/examples/c_cmdline && make >/dev/null 2>&1 && ./jpegdec && \
 ./jpegdec ../../../demo.jpg /tmp/huffcache_demo.bmp >/dev/null && \
 echo "demo.jpg -> BMP sha256:" && sha256sum /tmp/huffcache_demo.bmp)

echo ""
echo "=== [3/3] Existing test: linux/examples/jpeg_perf_test ==="
g++ -ggdb -D__LINUX__ -Wall -O2 -I$ROOT/src -c $ROOT/linux/examples/jpeg_perf_test/main.cpp -o /tmp/jpt_main.o
g++ -ggdb -D__LINUX__ -Wall -O2 -I$ROOT/src -c $ROOT/src/JPEGDEC.cpp -o /tmp/JPEGDEC.o 2>/dev/null
g++ /tmp/jpt_main.o /tmp/JPEGDEC.o -lm -o /tmp/jpeg_perf_test
/tmp/jpeg_perf_test

echo ""
echo "=== ALL SUITES DONE ==="
