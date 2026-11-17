#!/bin/sh
# Build and run the IDCT precision tier test (self-contained, no third-party deps).
set -e
cd "$(dirname "$0")"
g++ -O2 -Wall -D__LINUX__ -I../../src idct_tier_test.cpp ../../src/JPEGDEC.cpp -o idct_tier_test -lm
./idct_tier_test
