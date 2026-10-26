#!/bin/bash
# Makes the MPEG-4 Part 2 test streams in tests/data (mpeg4_*.m4v) with ffmpeg's mpeg4 encoder: 160x96, 12 pictures of a synthetic source with motion and noise. They are
# what the header parser's test reads (tests/Mpeg4HeadersTest.cpp). Usage: scripts/gen_mpeg4_test_streams.sh   (needs ffmpeg)
set -e
out=tests/data
src="testsrc2=size=160x96:rate=25,noise=alls=10:allf=t+u,format=yuv420p"
make() {   # name, encoder options
  local name=$1; shift
  ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "$src" -frames:v 12 -c:v mpeg4 "$@" -f m4v "$out/mpeg4_$name.m4v"
}
make plain   -g 12 -bf 0 -qscale:v 4
make bframes -g 12 -bf 2 -qscale:v 4
make mpegq   -g 12 -bf 2 -qscale:v 4 -mpeg_quant 1
make qpel    -g 12 -bf 2 -qscale:v 4 -flags +qpel
make interl  -g 12 -bf 2 -qscale:v 4 -flags +ilme+ildct
make matrix  -g 12 -bf 2 -qscale:v 4 -mpeg_quant 1 -intra_matrix "8,17,18,19,21,23,25,27,17,18,19,21,23,25,27,28,20,21,22,23,24,26,28,30,21,22,23,24,26,28,30,32,22,23,24,26,28,30,32,35,23,24,26,28,30,32,35,38,25,26,28,30,32,35,38,41,27,28,30,32,35,38,41,46" -inter_matrix "16,17,18,19,20,21,22,23,17,18,19,20,21,22,23,24,18,19,20,21,22,23,24,25,19,20,21,22,23,24,26,27,20,21,22,23,25,26,27,28,21,22,23,24,26,27,28,30,22,23,24,26,27,28,30,31,23,24,25,27,28,30,31,34"
ls -l $out/mpeg4_*.m4v | awk '{print $5, $9}'
