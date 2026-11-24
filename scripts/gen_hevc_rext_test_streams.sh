#!/bin/bash
# Makes the H.265 range extension test streams in tests/data with ffmpeg + libx265: every chroma format (4:0:0, 4:2:0, 4:2:2, 4:4:4) at 8, 10 and 12 bits, with
# the coding tools of the earlier streams (B pictures, weighted prediction, transform skip, scaling lists, lossless coding units, SAO, slices, wavefront rows, chroma QP
# offsets, 64x64 trees with transform depth 4). x265 writes no SPS / PPS range extension: the range extension's own tools are in the streams of
# scripts/gen_hevc_rext_tool_streams.py.
#
#   tests/data/hevcrext_<name>.h265  128x80 (oddsize: 200x120), each picture carrying the encoder's MD5 of it (hash=1), which the decoder checks over its planes
#   tests/data/hevcrext_<name>.md5   FFmpeg's decode: one MD5 per picture in display order of the picture in its own format, planar (gray, gray12le, yuv420p12le,
#                                    yuv422p, yuv422p10le, ...: bytes for 8 bits, 16-bit little-endian words above), the cropped size
# The decode is checked against libde265's (dec265) as well; the script stops when they differ.
# Usage: scripts/gen_hevc_rext_test_streams.sh   (needs ffmpeg with libx265 of 8, 10 and 12 bits, and dec265)
set -e
out=tests/data
n=8
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
make() {   # name, pixel format, x265 params, [frames], [size]
  local name=$1 pf=$2 params=$3 frames=${4:-$n} size=${5:-128x80}
  ffmpeg -nostdin -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=$size:rate=30,noise=alls=12:allf=t+u,format=$pf" -frames:v "$frames" -c:v libx265 -pix_fmt "$pf" \
    -x265-params "log-level=error:repeat-headers=1:info=0:hash=1:$params" -f hevc "$out/hevcrext_$name.h265"
  ffmpeg -nostdin -hide_banner -loglevel error -y -i "$out/hevcrext_$name.h265" -f framemd5 -pix_fmt "$pf" - | grep -v '^#' | awk -F', *' '{print $NF}' > "$out/hevcrext_$name.md5"
  if ffmpeg -nostdin -hide_banner -loglevel verbose -err_detect crccheck -i "$out/hevcrext_$name.h265" -f null - 2>&1 | grep -qi "mismatching checksum"; then
    echo "FFmpeg fails the hashes of $name" >&2
    exit 1
  fi
  # libde265 writes planar YUV (4:0:0 as luma alone): its pictures must be FFmpeg's
  dec265 -q -o "$tmp/$name.yuv" "$out/hevcrext_$name.h265"
  ffmpeg -nostdin -hide_banner -loglevel error -y -i "$out/hevcrext_$name.h265" -f rawvideo -pix_fmt "$pf" "$tmp/$name.ffmpeg.yuv"
  cmp -s "$tmp/$name.yuv" "$tmp/$name.ffmpeg.yuv" || { echo "libde265 and FFmpeg disagree on $name" >&2; exit 1; }
}
make 400_8          gray        "keyint=8:bframes=2:ref=2:ctu=32:sao=1"
make 400_12         gray12le    "keyint=8:bframes=2:ref=2:ctu=32:weightp=1:weightb=1:tskip=1"
make 420_12         yuv420p12le "keyint=8:bframes=3:ref=3:ctu=32:tskip=1:tskip-fast=0:sign-hide=1:amp=1:rect=1:scaling-list=default"
make 420_12_lossless yuv420p12le "keyint=8:bframes=2:ctu=32:cu-lossless=1:crf=30"
make 422_8          yuv422p     "keyint=8:bframes=2:ref=2:ctu=32:wpp=1:sao=1:crf=32"
make 422_10         yuv422p10le "keyint=8:bframes=3:ref=3:ctu=32:weightp=1:weightb=1:tskip=1:tskip-fast=0:scaling-list=default:amp=1:rect=1"
make 422_10_intra   yuv422p10le "keyint=1:ctu=16:min-cu-size=8:strong-intra-smoothing=1:tskip=1:rdoq-level=0" 4
make 422_12         yuv422p12le "keyint=8:bframes=2:ref=2:ctu=32:slices=2:cbqpoffs=-5:crqpoffs=6:crf=24"
make 444_8          yuv444p     "keyint=8:bframes=2:ref=2:ctu=32:cu-lossless=1:tskip=1:crf=28"
make 444_8_intra    yuv444p     "keyint=1:ctu=16:min-cu-size=8:tskip=1:tskip-fast=0:rdoq-level=0:sign-hide=1" 4
make 444_10         yuv444p10le "keyint=8:bframes=2:ref=2:ctu=32:weightp=1:weightb=1:sao=1:wpp=1:crf=30"
make 444_12         yuv444p12le "keyint=8:bframes=3:ref=3:ctu=32:scaling-list=default:cbqpoffs=4:crqpoffs=-4:tskip=1"
make 444_12_ctu64   yuv444p12le "keyint=8:bframes=2:ctu=64:tu-intra-depth=4:tu-inter-depth=4:crf=20" 6
make 422_10_oddsize yuv422p10le "keyint=8:bframes=2:ctu=64:ref=2" 6 200x120
# 4:4:4 with scaling lists of its own (every list, the 32x32 chroma ones made from the 16x16 ones, 7.4.5) and 32x32 transforms
python3 -I -c '
import random, sys
r = random.Random(5)
out = []
for size, n in ((4, 16), (8, 64), (16, 64), (32, 64)):
    for kind in ("INTRA", "INTER"):
        for comp in ("LUMA", "CHROMAU", "CHROMAV"):
            out.append(f"{kind}{size}X{size}_{comp} =")
            out.append(",".join(str(r.randint(8, 90)) for _ in range(n)))
            if size >= 16:
                out.append(f"{kind}{size}X{size}_{comp}_DC =")
                out.append(str(r.randint(8, 60)))
open(sys.argv[1], "w").write("\n".join(out) + "\n")' "$tmp/scaling.txt"
make 444_10_scaling yuv444p10le "keyint=8:bframes=1:ctu=64:tu-intra-depth=3:tu-inter-depth=3:scaling-list=$tmp/scaling.txt:crf=18" 4
ls -l $out/hevcrext_*.h265 | awk '{print $5, $9}'
