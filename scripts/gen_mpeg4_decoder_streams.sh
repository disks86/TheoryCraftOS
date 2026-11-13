#!/bin/bash
# Makes the streams of the MPEG-4 Part 2 / H.263 decoder's tests in tests/data (m4vdec_*.m4v, h263dec_*.h263) and the pictures FFmpeg's decoder makes of them
# (*.yuv, I420, all frames in display order). The encoders are FFmpeg's mpeg4 and h263, and libxvid when FFmpeg has it. The references are decoded with FFmpeg's C
# code (-cpuflags 0), its "simple" IDCT (the one tests/VideoCodecTest.cpp's decoder uses; FFmpeg would otherwise switch to Xvid's IDCT for Xvid streams),
# -flags bitexact (mismatch control for intra blocks under the MPEG quantiser, as the standard has it) and ONE thread: FFmpeg's frame threads do not share the
# field vectors that direct mode in interlaced B planes needs (p_field_mv_table), so a threaded decode of an interlaced stream with B planes differs from run to run.
# Usage: scripts/gen_mpeg4_decoder_streams.sh   (needs ffmpeg; libxvid optional)
set -e
out=tests/data
n=8
src() { echo "testsrc2=size=$1:rate=25,noise=alls=12:allf=t+u,format=yuv420p"; }
ref() { ffmpeg -nostdin -hide_banner -loglevel error -y -cpuflags 0 -flags bitexact -idct simple -threads 1 -i "$1" -f rawvideo -pix_fmt yuv420p -fps_mode passthrough "${1%.*}.yuv"; }
m4v() {   # name, size, encoder and options
  local name=$1 size=$2; shift 2
  ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "$(src $size)" -frames:v $n "$@" -f m4v "$out/m4vdec_$name.m4v"
  ref "$out/m4vdec_$name.m4v"
}
h263() {
  local name=$1; shift
  ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "$(src 128x96)" -frames:v $n -c:v h263 "$@" -f h263 "$out/h263dec_$name.h263"
  ref "$out/h263dec_$name.h263"
}
# I and P planes with video packets (resync markers every ~150 bytes) and four vectors per macroblock
m4v packets   96x64 -c:v mpeg4 -bf 0 -g 8 -qscale:v 4 -ps 150 -flags +mv4
# B planes (direct, forward, backward, interpolated) with four vectors per macroblock
m4v bframes   96x64 -c:v mpeg4 -bf 2 -g 8 -qscale:v 3 -flags +mv4
# The MPEG quantiser, quarter-sample motion, B planes
m4v qpel      96x64 -c:v mpeg4 -bf 2 -g 8 -qscale:v 4 -mpeg_quant 1 -flags +qpel+mv4
# Interlaced: field DCT, field motion (also in B planes and in direct mode), quarter-sample
m4v interlaced 96x64 -c:v mpeg4 -bf 2 -g 8 -qscale:v 4 -flags +ilme+ildct+qpel
# Not a multiple of the macroblock size; a low rate (large quantisers, skipped macroblocks)
m4v oddsize   90x54 -c:v mpeg4 -bf 1 -g 8 -b:v 60k -flags +mv4
if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libxvid; then
  m4v xvid    96x64 -c:v libxvid -bf 2 -g 8 -qscale:v 4 -flags +qpel+mv4
fi
# H.263 baseline: intra and inter pictures; a higher quantiser with an intra picture every 4
h263 ip -g 8 -qscale:v 4
h263 q  -g 4 -qscale:v 14
ls -l $out/m4vdec_* $out/h263dec_* | awk '{print $5, $9}'
