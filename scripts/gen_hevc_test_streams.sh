#!/bin/bash
# Makes the H.265 test streams in tests/data (hevc_*.h265) with ffmpeg + libx265. The source is a synthetic picture with motion, edges and noise.
#
# Every stream carries the encoder's own MD5 of each reconstructed picture (x265 hash=1: a decoded picture hash SEI message after every picture). The decoder
# (H265Decoder) checks the pictures it makes against these, so a stream is its own reference: it needs no copy of the pictures. As a sanity check of the streams
# this script also runs libde265 (libde265-dec265, with its hash check) and FFmpeg's decoder (-err_detect crccheck) and reports a disagreement: FFmpeg 6.1 gets a few
# chroma samples of hevc_cip wrong, and libde265 1.0.15 gets the RASL pictures of hevc_cra wrong; both fail the stream's hashes there.
# Usage: scripts/gen_hevc_test_streams.sh   (needs ffmpeg with libx265; libde265-dec265 for the cross-check, e.g. apt install libde265-examples)
set -e
out=tests/data
w=160; h=96; n=12
src="testsrc2=size=${w}x${h}:rate=30,noise=alls=12:allf=t+u,format=yuv420p"
make() {   # name, x265 params
  local name=$1 params=$2
  ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "$src" -frames:v $n -c:v libx265 -pix_fmt yuv420p -x265-params "log-level=error:repeat-headers=1:info=0:hash=1:$params" -f hevc "$out/hevc_$name.h265"
  rm -f "$out/hevc_$name.yuv"
  if command -v libde265-dec265 >/dev/null; then
    libde265-dec265 -q -c "$out/hevc_$name.h265" 2>&1 | grep -qi "mismatch" && echo "note: libde265 fails the hashes of $name" >&2
  fi
  ffmpeg -nostdin -hide_banner -loglevel verbose -err_detect crccheck -i "$out/hevc_$name.h265" -f null - 2>&1 | grep -qi "mismatching checksum" && echo "note: FFmpeg fails the hashes of $name" >&2
  return 0
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
make lossless   "keyint=30:bframes=2:ctu=32:lossless=1"
make cra        "keyint=5:bframes=3:ctu=32:open-gop=1:ref=3"
make cip        "keyint=30:bframes=2:ctu=16:constrained-intra=1:b-intra=1"
make highrate   "keyint=30:bframes=2:ctu=32:crf=4:ref=3"
make lowrate    "keyint=30:bframes=3:ctu=64:crf=45:ref=2"
make maxref     "keyint=60:bframes=5:ctu=32:ref=5:b-pyramid=1"
make cutree_off "keyint=15:bframes=5:ref=5:ctu=32:rc-lookahead=10:b-adapt=2:open-gop=0"
# 200x120: not a multiple of the CTB size, and 8 lines above a multiple of 16 (cropped by the encoder's conformance window)
src="testsrc2=size=200x120:rate=30,noise=alls=12:allf=t+u,format=yuv420p"
make oddsize    "keyint=30:bframes=2:ctu=64:ref=2"
src2="testsrc2=size=176x144:rate=30,noise=alls=12:allf=t+u,format=yuv420p"
src="$src2"
make qcif       "keyint=30:bframes=2:ctu=16:ref=2"
ls -l $out/hevc_*.h265 | awk '{print $5, $9}'

# The stream the VCN H.265 self-test decodes (src/Hardware/VideoCodec/AMD/AmdVcnSelfTestHevcStream.inc): six 64x64 pictures, B pictures with reordering. Not kept in
# tests/data; only the generated C++ array is.
selftest_dir=$(mktemp -d)
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=64x64:rate=30,noise=alls=12:allf=t+u,format=yuv420p" -frames:v 6 -c:v libx265 -pix_fmt yuv420p \
  -x265-params "log-level=error:repeat-headers=1:info=0:hash=1:keyint=30:bframes=2:ref=2:ctu=32:min-cu-size=8:crf=30:b-adapt=0:no-scenecut=1:weightp=0:weightb=0:frame-threads=1:pools=none" \
  -f hevc "$selftest_dir/selftest.h265"
python3 -I scripts/gen_hevc_selftest_inc.py "$selftest_dir/selftest.h265" src/Hardware/VideoCodec/AMD/AmdVcnSelfTestHevcStream.inc
rm -rf "$selftest_dir"
