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
# The HEVC picture encoder (src/Hardware/VideoCodec/Software/H265PictureEncoder, what Direct3D 12's HEVC video encoder runs on) checked against ffmpeg:
# tests/VideoEncodeHevcTest writes an IDR P B B P B B P B B stream (200x120, CTB 32, SAO, two slices a picture, the program's own VPS / SPS / PPS), a long-term /
# list-modification / POC-wrap stream (64x48, CTB 16) and a Main 10 stream (128x96) with their reconstructions and sources; ffmpeg's decodes must be the
# reconstructions byte for byte, and the PSNR against the sources is printed.
# Usage: tests/hevc_encode_ffmpeg.sh      (needs ffmpeg; uses $TMPDIR)
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-hevc-encode
mkdir -p "$work"
TMPDIR="$work" tests/run_host_tests.sh VideoEncodeHevcTest >/dev/null
"$work/tcos-host-tests/VideoEncodeHevcTest" "$work" | tail -1
ok=1
check() {   # stream recon source width height pix_fmt pictures
  local stream=$1 recon=$2 source=$3 w=$4 h=$5 fmt=$6 want=$7
  ffmpeg -v error -threads 1 -i "$work/$stream" -f rawvideo -pix_fmt "$fmt" -y "$work/ffmpeg.yuv"
  local bytes=$(( w * h * 3 / 2 ))
  [ "$fmt" = yuv420p10le ] && bytes=$(( bytes * 2 ))
  local frames=$(( $(stat -c %s "$work/ffmpeg.yuv") / bytes ))
  if cmp -s "$work/ffmpeg.yuv" "$work/$recon" && [ "$frames" = "$want" ]; then
    echo "$stream: ffmpeg decodes $frames pictures to the encoder's reconstruction, byte for byte"
  else
    echo "$stream: ffmpeg's decode ($frames pictures) differs from the reconstruction"
    ok=0
  fi
  if [ -n "$source" ]; then
    ffmpeg -hide_banner -f rawvideo -pix_fmt "$fmt" -s "${w}x$h" -i "$work/ffmpeg.yuv" -f rawvideo -pix_fmt "$fmt" -s "${w}x$h" -i "$work/$source" -lavfi psnr -f null - 2>&1 |
      grep -o "PSNR y:[^ ]* u:[^ ]* v:[^ ]* average:[^ ]*" || true
  fi
}
check stream.hevc recon.yuv source.yuv 200 120 yuv420p 10
check stream10.hevc recon10.yuv source10.yuv 128 96 yuv420p10le 7
check stream_lt.hevc recon_lt.yuv "" 64 48 yuv420p 23
[ "$ok" = 1 ]
