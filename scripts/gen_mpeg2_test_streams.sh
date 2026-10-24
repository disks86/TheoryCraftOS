#!/bin/bash
# Makes the MPEG-2 test streams in tests/data (mpeg2_*.m2v, elementary streams) and the pictures a reference decoder makes of them (mpeg2_*.yuv, I420, all frames
# in display order) with ffmpeg's mpeg2video encoder and decoder. The source is a synthetic picture with motion, edges and noise. The decoders' inverse transforms round
# differently (the standard allows it within the IEEE 1180 limits), so tests/Mpeg2Test.cpp accepts a difference of a few levels, not an exact match; mpeg2dec (libmpeg2,
# -c: its C code) is a second reference to compare by hand: scripts/compare_mpeg2.sh.
# Usage: scripts/gen_mpeg2_test_streams.sh   (needs ffmpeg with mpeg2video)
set -e
out=tests/data
w=96; h=64; n=8
src="testsrc2=size=${w}x${h}:rate=30,noise=alls=12:allf=t+u,format=yuv420p"
make() {   # name, encoder options
  local name=$1; shift
  ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "${SRC:-$src}" -frames:v $n -c:v mpeg2video "$@" -f mpeg2video "$out/mpeg2_$name.m2v"
  ffmpeg -nostdin -hide_banner -loglevel error -y -idct simple -i "$out/mpeg2_$name.m2v" -f rawvideo -pix_fmt yuv420p "$out/mpeg2_$name.yuv"
}
make intra          -g 1 -qscale:v 3
make ipp            -bf 0 -g 12 -qscale:v 4
make ibp            -bf 2 -g 12 -qscale:v 3
make interlaced     -flags +ildct+ilme -top 1 -bf 2 -g 12 -qscale:v 3
make interlaced_bff -flags +ildct+ilme -top 0 -bf 2 -g 12 -qscale:v 3
make alt            -flags +ildct+ilme -alternate_scan 1 -non_linear_quant 1 -intra_vlc 1 -bf 1 -g 12 -qscale:v 5 -qmax 28
make dc10           -dc 10 -bf 1 -g 12 -qscale:v 3
make highrate       -bf 2 -g 12 -qscale:v 1
make lowrate        -bf 2 -g 12 -qscale:v 24
make matrix         -intra_matrix 8,16,19,22,26,27,29,34,16,16,22,24,27,29,34,37,19,22,26,27,29,34,34,38,22,22,26,27,29,34,37,40,22,26,27,29,32,35,40,48,26,27,29,32,35,40,48,58,26,27,29,34,38,46,56,69,27,29,35,38,46,56,69,83 \
                    -inter_matrix 16,17,18,19,20,21,22,23,17,18,19,20,21,22,23,24,18,19,20,21,22,23,24,25,19,20,21,22,23,24,25,26,20,21,22,23,24,25,26,27,21,22,23,24,25,26,27,28,22,23,24,25,26,27,28,29,23,24,25,26,27,28,29,30 \
                    -bf 1 -g 12 -qscale:v 4
# 90x54: not a multiple of the macroblock size (cropped by the decoder)
SRC="testsrc2=size=90x54:rate=30,noise=alls=12:allf=t+u,format=yuv420p" make oddsize -flags +ildct+ilme -bf 2 -g 12 -qscale:v 4
# Field pictures, 16x8 and dual prime prediction, skipped macroblocks, B fields: streams ffmpeg's encoder cannot produce (see gen_mpeg2_synthetic.py)
python3 "$(dirname "$0")/gen_mpeg2_synthetic.py" $out/mpeg2_syn
for n in frames fields fields_bff; do ffmpeg -nostdin -hide_banner -loglevel error -y -idct simple -i $out/mpeg2_syn_$n.m2v -f rawvideo -pix_fmt yuv420p $out/mpeg2_syn_$n.yuv; done
ls -l $out/mpeg2_*.m2v | awk '{print $5, $9}'
