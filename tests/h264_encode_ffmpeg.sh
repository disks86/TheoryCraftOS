#!/usr/bin/env bash
# TheoryCraftOS - a minimal UEFI x86_64 operating system.
# Copyright (C) 2026 Christopher Schaefer
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# The H.264 picture encoder (src/Hardware/VideoCodec/Software/H264PictureEncoder, what Direct3D 12's video encoder runs on) checked against ffmpeg: tests/VideoEncodeTest
# writes its sequences (the program's own parameter sets; CAVLC and CABAC, the 8x8 transform and Intra8x8, B_Skip / B_Direct_16x16 with spatial and temporal direct
# prediction, slices by rows, macroblocks, count and bytes, intra refresh, cropped sizes, unchanged macroblocks, motion hints) with their reconstructions and sources;
# ffmpeg's decode of each must be the reconstruction byte for byte, and the PSNR against the source is printed.
# Usage: tests/h264_encode_ffmpeg.sh      (needs ffmpeg; uses $TMPDIR)
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-h264-encode
rm -rf "$work"
mkdir -p "$work"
TMPDIR="$work" tests/run_host_tests.sh VideoEncodeTest >/dev/null
"$work/tcos-host-tests/VideoEncodeTest" "$work" | tail -1
failed=0 count=0
for info in "$work"/*.size; do
  name=$(basename "$info" .size)
  read -r size frames < "$info"
  ffmpeg -v error -threads 1 -i "$work/$name.h264" -f rawvideo -pix_fmt yuv420p -y "$work/$name.ffmpeg.yuv"
  w=${size%x*} h=${size#*x}
  got=$(( $(stat -c %s "$work/$name.ffmpeg.yuv") / (w * h * 3 / 2) ))
  psnr=$(ffmpeg -hide_banner -f rawvideo -pix_fmt yuv420p -s "$size" -i "$work/$name.ffmpeg.yuv" -f rawvideo -pix_fmt yuv420p -s "$size" -i "$work/$name.source.yuv" \
           -lavfi psnr -f null - 2>&1 | grep -o "average:[^ ]*" || true)
  count=$((count + 1))
  if cmp -s "$work/$name.ffmpeg.yuv" "$work/$name.recon.yuv" && [ "$got" = "$frames" ]; then
    echo "$name ($size): ffmpeg decodes $got pictures to the encoder's reconstruction, byte for byte ($psnr)"
  else
    echo "$name ($size): ffmpeg's decode ($got of $frames pictures) differs from the reconstruction"
    failed=$((failed + 1))
  fi
done
echo "$((count - failed)) of $count streams match"
[ "$failed" = 0 ] && [ "$count" -gt 0 ]
