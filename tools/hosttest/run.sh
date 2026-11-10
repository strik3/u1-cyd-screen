#!/bin/sh
# Desktop test of the streaming PNG decoder + scaler (no ESP32 needed).
# Usage: PNGDEC=/path/to/PNGdec/src ./run.sh
set -e
cd "$(dirname "$0")"
SRC=../../src
python3 gen.py 480 320 320 213 0 u1fit
python3 gen.py 480 320 320 240 0 u1str
g++ -O2 -w -D__LINUX__ -DPNG_MAX_BUFFERED_PIXELS=8200 -I"$PNGDEC" -I$SRC test.cpp $SRC/frame_decoder.cpp "$PNGDEC"/PNGdec.cpp "$PNGDEC"/*.c -o hosttest
./hosttest u1fit 0 && ./hosttest u1str 1
