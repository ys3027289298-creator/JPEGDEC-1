#!/usr/bin/env bash
# One-shot build + test for the JPEGDEC dual-output feature.
#
#   1. Generates the four tiny self-made JPEG samples (grayscale, 4:4:4,
#      4:2:2 and odd-sized 4:2:0).
#   2. Builds the test runner against the MODIFIED library and runs the full
#      functional / invalid-parameter / timing suite.
#   3. Builds the runner against the PRISTINE baseline library (git HEAD copy
#      in a scratch dir) and byte-compares every single-output reference dump,
#      proving the original single-output path is pixel-for-pixel unchanged.
#   4. Builds and runs the repository's own jpeg_perf_test example against the
#      modified library as the "existing tests" smoke run.
#   5. Runs a stable min/median timing benchmark on the repo's 640x480 image.
#
# No build script or public header in the repo is touched; everything this
# script creates stays under test_dual/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TDIR="$ROOT/test_dual"
BUILD="$TDIR/build"
BASE="$BUILD/baseline_src"
CXXFLAGS="-D__LINUX__ -Wall -O2 -g -I$ROOT/src"

rm -rf "$BUILD"
mkdir -p "$BUILD" "$BASE"

echo "=== 1. generate samples ==="
python3 -I "$TDIR/gen_samples.py"

echo
echo "=== 2. build modified library ==="
g++ $CXXFLAGS -c "$ROOT/src/JPEGDEC.cpp" -o "$BUILD/JPEGDEC_new.o"
g++ $CXXFLAGS -I"$ROOT" -c "$TDIR/dual_test.cpp" -o "$BUILD/dual_test_new.o"
g++ "$BUILD/JPEGDEC_new.o" "$BUILD/dual_test_new.o" -o "$BUILD/dual_test_new"

echo
echo "=== 3. run full suite against modified library ==="
mkdir -p "$BUILD/new_refs"
"$BUILD/dual_test_new" "$TDIR/samples" "$BUILD/new_refs" | tee "$BUILD/new_run.log"

echo
echo "=== 4. pristine baseline single-output comparison ==="
# Copy the library sources as of the working tree's HEAD into a scratch dir.
# Only src/JPEGDEC.cpp + its includes are needed; jpeg.inl is #included.
git -C "$ROOT" show HEAD:src/JPEGDEC.cpp > "$BASE/JPEGDEC.cpp"
git -C "$ROOT" show HEAD:src/JPEGDEC.h   > "$BASE/JPEGDEC.h"
git -C "$ROOT" show HEAD:src/jpeg.inl    > "$BASE/jpeg.inl"
g++ $CXXFLAGS -c "$BASE/JPEGDEC.cpp" -o "$BUILD/JPEGDEC_base.o"
g++ $CXXFLAGS -I"$ROOT" -c "$TDIR/dual_test.cpp" -o "$BUILD/dual_test_base.o"
g++ "$BUILD/JPEGDEC_base.o" "$BUILD/dual_test_base.o" -o "$BUILD/dual_test_base"
mkdir -p "$BUILD/base_refs"
# The baseline has no dual-output code: dumps-only mode stops before those tests.
JPEGDEC_TEST_DUMPS_ONLY=1 "$BUILD/dual_test_base" "$TDIR/samples" "$BUILD/base_refs" > "$BUILD/base_run.log"
cat "$BUILD/base_run.log"
mismatch=0
for f in "$BUILD/base_refs"/ref_*.bin; do
    name="$(basename "$f")"
    if cmp -s "$f" "$BUILD/new_refs/$name"; then
        echo "  identical: $name"
    else
        echo "  MISMATCH:  $name"; mismatch=1
    fi
done
if [ "$mismatch" -ne 0 ]; then
    echo "BASELINE SINGLE-OUTPUT PATH CHANGED - FAIL"
    exit 1
fi

echo
echo "=== 5. repository example build + run (existing test) ==="
g++ $CXXFLAGS -I"$ROOT" -c "$ROOT/linux/examples/jpeg_perf_test/main.cpp" -o "$BUILD/perf_main.o"
g++ "$BUILD/JPEGDEC_new.o" "$BUILD/perf_main.o" -o "$BUILD/jpeg_perf_test"
"$BUILD/jpeg_perf_test" | tee "$BUILD/perf_run.log"

echo
echo "=== 6. stable timing on the repo's 640x480 tulips image ==="
g++ $CXXFLAGS -I"$ROOT" -c "$TDIR/bench_tulips.cpp" -o "$BUILD/bench_tulips.o"
g++ "$BUILD/JPEGDEC_new.o" "$BUILD/bench_tulips.o" -o "$BUILD/bench_tulips"
"$BUILD/bench_tulips" | tee "$BUILD/bench_tulips.log"

echo
echo "ALL GREEN"
