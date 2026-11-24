#!/bin/bash
# Makes the 10-bit (Main 10) and Still Picture H.265 test streams in tests/data with ffmpeg + libx265, next to the 8-bit ones of gen_hevc_test_streams.sh.
#
#   tests/data/hevc10_<name>.h265   Main 10, 160x96 (oddsize: 200x120), 4:2:0, each carrying the encoder's MD5 of every reconstructed picture (hash=1), which the
#                                   decoder checks over the 16-bit samples
#   tests/data/hevc10_<name>.md5    FFmpeg's decode of it: one MD5 per picture in display order, of the picture as yuv420p10le (16-bit little-endian words, planar)
#   tests/data/hevc_still.h265      Main Still Picture (8-bit, one picture); hevc10_still.h265 one Main 10 intra picture
# tests/data/hevc10_pcm.h265 comes from scripts/gen_hevc10_pcm_stream.py (x265 has no PCM). Its .md5 is libde265's decode (dec265), not FFmpeg's: FFmpeg n9.0.2 gets
# some chroma blocks of it wrong (at the low and high quantisers), where libde265 and the AMD hardware decoder (VAAPI, checked by hand) agree with each other and with ours.
# Usage: scripts/gen_hevc10_test_streams.sh   (needs ffmpeg with libx265)
set -e
out=tests/data
n=12
src="testsrc2=size=160x96:rate=30,noise=alls=12:allf=t+u,format=yuv420p10le"
reference() {   # name: FFmpeg's per-picture MD5s of the stream
  ffmpeg -nostdin -hide_banner -loglevel error -y -i "$out/$1.h265" -f framemd5 -pix_fmt yuv420p10le - | grep -v '^#' | awk -F', *' '{print $NF}' > "$out/$1.md5"
  ffmpeg -nostdin -hide_banner -loglevel verbose -err_detect crccheck -i "$out/$1.h265" -f null - 2>&1 | grep -qi "mismatching checksum" && echo "note: FFmpeg fails the hashes of $1" >&2
  return 0
}
make() {   # name, x265 params, [frames]
  local name=$1 params=$2 frames=${3:-$n}
  ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "$src" -frames:v "$frames" -c:v libx265 -pix_fmt yuv420p10le -x265-params "log-level=error:repeat-headers=1:info=0:hash=1:$params" -f hevc "$out/hevc10_$name.h265"
  reference "hevc10_$name"
}
make intra      "keyint=1:ctu=32:min-cu-size=8:strong-intra-smoothing=1"
make intra_raw  "keyint=1:no-deblock=1:no-sao=1:ctu=16:min-cu-size=8:rect=0:amp=0:rdoq-level=0:no-strong-intra-smoothing=1"
make ipp        "keyint=30:bframes=0:ref=3:ctu=32:min-cu-size=8"
make bframes    "keyint=30:bframes=4:ref=4:ctu=32:b-pyramid=1:weightp=0:weightb=0"
make weighted   "keyint=30:bframes=2:ref=3:weightp=1:weightb=1:ctu=32"
make tools      "keyint=30:bframes=3:ctu=32:tskip=1:tskip-fast=0:sign-hide=1:amp=1:rect=1:tmvp=1:strong-intra-smoothing=1:rdoq-level=2:psy-rd=2.0:b-intra=1"
make scaling    "keyint=30:bframes=2:ctu=32:scaling-list=default"
make lossless   "keyint=30:bframes=2:ctu=32:lossless=1"
make culossless "keyint=30:bframes=2:ctu=32:cu-lossless=1:crf=30"
make sao        "keyint=30:bframes=2:ctu=32:crf=33:limit-sao=0:sao-non-deblock=1"
make lowrate    "keyint=30:bframes=3:ctu=64:crf=45:ref=2"
make highrate   "keyint=30:bframes=2:ctu=32:crf=0:ref=3:qpmin=0"
make chromaqp   "keyint=30:bframes=2:ctu=32:cbqpoffs=-12:crqpoffs=12:crf=20"
make slices     "keyint=30:bframes=2:slices=3:ctu=16:ref=2"
make wpp        "keyint=30:bframes=2:ctu=32:wpp=1"
make cip        "keyint=30:bframes=2:ctu=16:constrained-intra=1:b-intra=1"
make cra        "keyint=5:bframes=3:ctu=32:open-gop=1:ref=3"
make still      "keyint=1" 1
src="testsrc2=size=200x120:rate=30,noise=alls=12:allf=t+u,format=yuv420p10le"
make oddsize    "keyint=30:bframes=2:ctu=64:ref=2"
# Main Still Picture (8-bit): the profile x265 calls mainstillpicture, one picture
ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=160x96:rate=30,noise=alls=12:allf=t+u,format=yuv420p" -frames:v 1 -c:v libx265 -pix_fmt yuv420p -profile:v mainstillpicture \
  -x265-params "log-level=error:repeat-headers=1:info=0:hash=1:keyint=1:ctu=32" -f hevc "$out/hevc_still.h265"
python3 -I scripts/gen_hevc10_pcm_stream.py "$out/hevc10_pcm.h265"
tmp=$(mktemp -d)
dec265 -q -o "$tmp/pcm.yuv" "$out/hevc10_pcm.h265"
python3 -I -c 'import hashlib, sys; d = open(sys.argv[1], "rb").read(); n = 128 * 64 * 3; print("\n".join(hashlib.md5(d[i:i + n]).hexdigest() for i in range(0, len(d), n)))' "$tmp/pcm.yuv" > "$out/hevc10_pcm.md5"
rm -rf "$tmp"
ls -l $out/hevc10_*.h265 $out/hevc_still.h265 | awk '{print $5, $9}'
