#!/bin/bash
# Makes the stream the UVD 6 MPEG-4 self-test decodes (src/Hardware/VideoCodec/AMD/AmdVcnSelfTestMpeg4Stream.inc): eight 32x32 pictures from ffmpeg's MPEG-4 encoder with
# B pictures (so the engine needs both of its anchors), and FFmpeg's decode of them. Not kept in tests/data; only the generated C++ arrays are.
# Usage: scripts/gen_mpeg4_selftest.sh (needs ffmpeg)
set -e
dir=$(mktemp -d)
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=32x32:rate=10,noise=alls=8:allf=t+u,format=yuv420p" -frames:v 8 -c:v mpeg4 -bf 2 -g 12 -qscale:v 3 -f m4v "$dir/selftest.m4v"
ffmpeg -nostdin -hide_banner -loglevel error -y -i "$dir/selftest.m4v" -f rawvideo -pix_fmt yuv420p "$dir/decoded.yuv"
python3 -I scripts/gen_mpeg4_selftest_inc.py "$dir/selftest.m4v" "$dir/decoded.yuv" src/Hardware/VideoCodec/AMD/AmdVcnSelfTestMpeg4Stream.inc
rm -rf "$dir"
