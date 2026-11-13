#!/bin/bash
# Makes the streams of the MPEG-4 Part 2 / H.263 decoder's tests in tests/data (m4vdec_*.m4v, h263dec_*.h263) and the pictures FFmpeg's decoder makes of them
# (*.yuv, I420, all frames in display order). The encoders are FFmpeg's mpeg4 and h263, and libxvid when FFmpeg has it. The references are decoded with FFmpeg's C
# code (-cpuflags 0), its "simple" IDCT (the one tests/VideoCodecTest.cpp's decoder uses; FFmpeg would otherwise switch to Xvid's IDCT for Xvid streams),
# -flags bitexact (mismatch control for intra blocks under the MPEG quantiser, as the standard has it) and ONE thread: FFmpeg's frame threads do not share the
# field vectors that direct mode in interlaced B planes needs (p_field_mv_table), so a threaded decode of an interlaced stream with B planes differs from run to run.
# Xvid streams are decoded with FFmpeg's default IDCT for them (Xvid's), which the decoder picks too. Overlapped block motion compensation (H.263 Annex F)
# cannot be checked against FFmpeg's decoder: its look-ahead for the vectors of the next macroblock (preview_obmc) predicts them from stale vectors, so it
# drifts from its own encoder. Those streams are checked against the encoder instead: the PSNR of each picture against the source, which FFmpeg's encoder
# reports of its reconstruction (-vstats), goes to *.psnr with the source (*.src.yuv) and the decoder must reach it.
# Usage: scripts/gen_mpeg4_decoder_streams.sh   (needs ffmpeg; libxvid optional)
set -e
out=tests/data
n=8
src() { echo "testsrc2=size=$1:rate=25,noise=alls=12:allf=t+u,format=yuv420p"; }
ref() { ffmpeg -nostdin -hide_banner -loglevel error -y -cpuflags 0 -flags bitexact ${IDCT--idct simple} -threads 1 ${INFMT:-} -i "$1" -f rawvideo -pix_fmt yuv420p -fps_mode passthrough "${1%.*}.yuv"; }
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
# Data partitioning (with video packets), B planes, four vectors
m4v partitioned 96x64 -c:v mpeg4 -bf 2 -g 8 -qscale:v 3 -data_partitioning 1 -ps 200 -flags +mv4
if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libxvid; then
  IDCT= m4v xvid 96x64 -c:v libxvid -bf 2 -g 8 -qscale:v 4 -flags +qpel+mv4
  # GMC (S planes): a picture that pans and turns, so that Xvid finds global motion
  ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=640x480:rate=25,format=yuv420p" \
    -vf "rotate=a='t*0.5':c=black,crop=400:300,scale=128:96,noise=alls=6:allf=t+u" -frames:v $n \
    -c:v libxvid -gmc 1 -bf 1 -g 8 -qscale:v 4 -f m4v "$out/m4vdec_gmc.m4v"
  IDCT= ref "$out/m4vdec_gmc.m4v"
fi
# H.263 baseline: intra and inter pictures; a higher quantiser with an intra picture every 4
h263 ip -g 8 -qscale:v 4
h263 q  -g 4 -qscale:v 14
# H.263 version 2 (PLUSPTYPE), custom size: unrestricted vectors (D), advanced intra coding (I) with modified quantisation (T), the deblocking filter (J),
# slices (K), the alternative inter VLC (S)
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "$(src 112x80)" -frames:v $n -c:v h263p -threads 1 -g 8 -qscale:v 5 -umv 1 -aiv 1 \
  -flags +aic+loop -structured_slices 1 -ps 300 -f rawvideo "$out/h263dec_plus.h263"
INFMT="-f h263" ref "$out/h263dec_plus.h263"
# Advanced prediction (F: four vectors, overlapped motion compensation): version 1, and version 2 with D, I, K, S - checked against the encoder (see above)
obmc() {
  local name=$1 codec=$2; shift 2
  ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "$(src 128x96)" -frames:v $n -c:v $codec -g 8 -qscale:v 4 -obmc 1 -flags +mv4+psnr "$@" \
    -vstats_file "$out/h263dec_$name.vstats" -f rawvideo "$out/h263dec_$name.h263"
  grep -o 'PSNR= *[0-9.]*' "$out/h263dec_$name.vstats" | awk '{print $2}' > "$out/h263dec_$name.psnr"
  rm "$out/h263dec_$name.vstats"
}
obmc obmc h263
obmc obmcplus h263p -umv 1 -aiv 1 -flags +mv4+psnr+aic -structured_slices 1 -ps 400
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "$(src 128x96)" -frames:v $n -f rawvideo -pix_fmt yuv420p "$out/h263dec_obmc.src.yuv"
# Data partitioning with reversible VLCs: no encoder writes them; a synthetic stream (scripts/gen_mpeg4_rvlc_stream.py), decoded by FFmpeg
python3 scripts/gen_mpeg4_rvlc_stream.py > /dev/null
ref "$out/m4vdec_rvlc.m4v"
# H.263 PB-frames (Annex G), Improved PB-frames (M) and B pictures (O): no encoder writes them; scripts/gen_h263_bframe_streams.py writes them and
# reconstructs the pictures from its own model of the Recommendation
python3 scripts/gen_h263_bframe_streams.py > /dev/null
# The player's files (tests/VideoPlayerTest.cpp): the MPEG-4 stream with B planes remuxed into MP4 and AVI; H.263 encoded straight into 3GP and AVI (a remux of
# the raw H.263 stream would mark every picture a key frame)
ffmpeg -nostdin -hide_banner -loglevel error -y -r 25 -i "$out/m4vdec_bframes.m4v" -c copy "$out/player_mpeg4.mp4"
ffmpeg -nostdin -hide_banner -loglevel error -y -r 25 -i "$out/m4vdec_bframes.m4v" -c copy -vtag XVID "$out/player_mpeg4.avi"
for ext in 3gp avi; do
  ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "$(src 128x96)" -frames:v $n -c:v h263 -g 8 -qscale:v 4 "$out/player_h263.$ext"
done
ref "$out/player_h263.3gp"
cmp -s "$out/player_h263.yuv" <(ffmpeg -nostdin -hide_banner -loglevel error -cpuflags 0 -flags bitexact -idct simple -threads 1 -i "$out/player_h263.avi" -f rawvideo -pix_fmt yuv420p -)
ls -l $out/m4vdec_* $out/h263dec_* | awk '{print $5, $9}'
