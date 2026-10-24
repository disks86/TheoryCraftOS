#!/bin/bash
# Makes the H.265 test streams in tests/data (hevc_*.h265) with ffmpeg + libx265, and the pictures an independent decoder (ffmpeg's) makes of each (hevc_*.yuv).
# The decoder tests compare this OS's decoder with those pictures byte for byte. The source is a synthetic picture with motion, edges and noise.
# Usage: scripts/gen_hevc_test_streams.sh   (needs ffmpeg with libx265)
set -e
out=tests/data
w=160; h=96; n=12
src="testsrc2=size=${w}x${h}:rate=30,noise=alls=12:allf=t+u,format=yuv420p"
make() {   # name, x265 params
  local name=$1 params=$2
  ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$src" -frames:v $n -c:v libx265 -pix_fmt yuv420p -x265-params "log-level=error:repeat-headers=1:info=0:$params" -f hevc "$out/hevc_$name.h265"
  ffmpeg -hide_banner -loglevel error -y -i "$out/hevc_$name.h265" -f rawvideo -pix_fmt yuv420p "$out/hevc_$name.yuv"
}
make intra      "keyint=1:no-deblock=1:no-sao=1:ctu=16:min-cu-size=8:rect=0:amp=0:rdoq-level=0:no-strong-intra-smoothing=1"
make intra_filt "keyint=1:ctu=32:min-cu-size=8"
make ipp        "keyint=30:bframes=0:ref=2:ctu=32:min-cu-size=8:no-sao=1:no-deblock=1:rect=0:amp=0"
make ipp_filt   "keyint=30:bframes=0:ref=3:ctu=32:min-cu-size=8"
make bframes    "keyint=30:bframes=4:ref=4:ctu=32:b-pyramid=1:weightp=0:weightb=0"
make weighted   "keyint=30:bframes=2:ref=3:weightp=1:weightb=1:ctu=32"
make slices     "keyint=30:bframes=2:slices=3:ctu=16:ref=2"
make ctu64      "keyint=30:bframes=3:ctu=64:min-cu-size=8:ref=2"
make tools      "keyint=30:bframes=3:ctu=32:tskip=1:tskip-fast=0:sign-hide=1:amp=1:rect=1:tmvp=1:strong-intra-smoothing=1:cu-lossless=0:rdoq-level=2:psy-rd=2.0:b-intra=1"
make scaling    "keyint=30:bframes=2:ctu=32:scaling-list=default"
make wpp        "keyint=30:bframes=2:ctu=32:wpp=1"
make cutree_off "keyint=15:bframes=5:ref=5:ctu=32:rc-lookahead=10:b-adapt=2:open-gop=0"
ls -l $out/hevc_*.h265 | awk '{print $5, $9}'
