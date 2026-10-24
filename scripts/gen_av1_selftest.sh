#!/bin/bash
# Makes the stream the VCN AV1 self-test decodes (src/Hardware/VideoCodec/AMD/AmdVcnSelfTestAv1Stream.inc): ten 64x64 pictures from libaom with hidden frames and
# show_existing_frame. Not kept in tests/data; only the generated C++ array is. Usage: scripts/gen_av1_selftest.sh (needs ffmpeg with libaom)
set -e
dir=$(mktemp -d)
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=64x64:rate=30,noise=alls=12:allf=t+u,format=yuv420p" -frames:v 10 -c:v libaom-av1 \
  -cpu-used 4 -crf 36 -b:v 0 -lag-in-frames 16 -auto-alt-ref 1 -g 30 -tiles 1x1 -aq-mode 0 -f ivf "$dir/selftest.ivf"
python3 -I scripts/gen_ivf_selftest_inc.py av1 "$dir/selftest.ivf" src/Hardware/VideoCodec/AMD/AmdVcnSelfTestAv1Stream.inc
rm -rf "$dir"
