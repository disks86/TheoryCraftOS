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
# writes an IDR P B B P B B stream (three slices a picture, constrained intra prediction, the program's own parameter sets) with its reconstruction and its source;
# ffmpeg's decode must be the reconstruction byte for byte, and the PSNR against the source is printed.
# Usage: tests/h264_encode_ffmpeg.sh      (needs ffmpeg; uses $TMPDIR)
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-h264-encode
mkdir -p "$work"
TMPDIR="$work" tests/run_host_tests.sh VideoEncodeTest >/dev/null
"$work/tcos-host-tests/VideoEncodeTest" "$work" | tail -1
ffmpeg -v error -threads 1 -i "$work/stream.h264" -f rawvideo -pix_fmt yuv420p -y "$work/ffmpeg.yuv"
frames=$(( $(stat -c %s "$work/ffmpeg.yuv") / (96 * 64 * 3 / 2) ))
same=0
if cmp -s "$work/ffmpeg.yuv" "$work/recon.yuv"; then
  echo "ffmpeg decodes $frames pictures to the encoder's reconstruction, byte for byte"
  same=1
else
  echo "ffmpeg's decode differs from the reconstruction"
fi
ffmpeg -hide_banner -f rawvideo -pix_fmt yuv420p -s 96x64 -i "$work/ffmpeg.yuv" -f rawvideo -pix_fmt yuv420p -s 96x64 -i "$work/source.yuv" -lavfi psnr -f null - 2>&1 |
  grep -o "PSNR y:[^ ]* u:[^ ]* v:[^ ]* average:[^ ]*" || true
[ "$same" = 1 ] && [ "$frames" = 7 ]
