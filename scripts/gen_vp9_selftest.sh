#!/bin/bash
# Makes the stream the VCN VP9 self-test decodes (src/Hardware/VideoCodec/AMD/AmdVcnSelfTestVp9Stream.inc): twenty 64x64 pictures from libvpx (two passes, so that the
# encoder uses hidden alt-ref frames: superframes). Not kept in tests/data; only the generated C++ array is. Usage: scripts/gen_vp9_selftest.sh (needs ffmpeg with libvpx)
set -e
dir=$(mktemp -d)
src="testsrc2=size=64x64:rate=10:duration=2"
opts="-pix_fmt yuv420p -c:v libvpx-vp9 -b:v 0 -crf 40 -cpu-used 1 -auto-alt-ref 1 -lag-in-frames 16"
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "$src" $opts -pass 1 -passlogfile "$dir/pass" -f null -
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "$src" $opts -pass 2 -passlogfile "$dir/pass" -f ivf "$dir/selftest.ivf"
python3 -I scripts/gen_ivf_selftest_inc.py vp9 "$dir/selftest.ivf" src/Hardware/VideoCodec/AMD/AmdVcnSelfTestVp9Stream.inc
rm -rf "$dir"
