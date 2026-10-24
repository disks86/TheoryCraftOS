#!/bin/bash
# Makes the stream the VCN MPEG-2 self-test decodes (src/Hardware/VideoCodec/AMD/AmdVcnSelfTestMpeg2Stream.inc): eight 64x64 pictures from ffmpeg's MPEG-2 encoder with
# B pictures (so the engine needs both of its reference slots). Not kept in tests/data; only the generated C++ array is. Usage: scripts/gen_mpeg2_selftest.sh (needs ffmpeg)
set -e
dir=$(mktemp -d)
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=64x64:rate=10,format=yuv420p" -frames:v 8 -c:v mpeg2video -bf 2 -g 12 -qscale:v 3 -f mpeg2video "$dir/selftest.m2v"
python3 -I scripts/gen_mpeg2_selftest_inc.py "$dir/selftest.m2v" src/Hardware/VideoCodec/AMD/AmdVcnSelfTestMpeg2Stream.inc
rm -rf "$dir"
