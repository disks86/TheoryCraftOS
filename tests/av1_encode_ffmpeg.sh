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
# The AV1 picture encoder (src/Hardware/VideoCodec/Software/Av1PictureEncoder, what Direct3D 12's AV1 video encoder runs on) checked against ffmpeg's AV1
# decoders: tests/VideoEncodeAv1Test writes an 8-bit 200x120 stream (key, inter frames of several references, intra-only, error resilient and switch frames,
# 2x2 tiles, TX_MODE_SELECT, switchable filters, CDEF, loop filter deltas) and a 10-bit 128x96 one (explicit tiles, QP map) with their reconstructions and
# sources; dav1d (and libaom, when ffmpeg has it) must decode each to the reconstruction byte for byte. The PSNR against the source is printed.
# Usage: tests/av1_encode_ffmpeg.sh      (needs ffmpeg with libdav1d; uses $TMPDIR)
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-av1-encode
mkdir -p "$work"
TMPDIR="$work" tests/run_host_tests.sh VideoEncodeAv1Test >/dev/null
"$work/tcos-host-tests/VideoEncodeAv1Test" "$work" | tail -1
status=0
decoders=$(ffmpeg -hide_banner -decoders 2>/dev/null || true)
check() {   # stem pix_fmt width height frames
  local stem=$1 fmt=$2 w=$3 h=$4 frames=$5
  for dec in libdav1d libaom-av1; do
    if ! grep -q " $dec " <<<"$decoders"; then
      echo "$stem: ffmpeg has no $dec"
      [ "$dec" = libdav1d ] && status=1
      continue
    fi
    rm -f "$work/$stem.$dec.yuv"
    if ! ffmpeg -v error -threads 1 -c:v "$dec" -i "$work/$stem.ivf" -f rawvideo -pix_fmt "$fmt" -y "$work/$stem.$dec.yuv"; then
      echo "$stem: $dec cannot decode the stream"
      status=1
      continue
    fi
    local n=$(( $(stat -c %s "$work/$stem.$dec.yuv") / $(stat -c %s "$work/$stem.yuv") * frames ))
    if cmp -s "$work/$stem.$dec.yuv" "$work/$stem.yuv"; then
      echo "$stem: $dec decodes $frames frames to the encoder's reconstruction, byte for byte"
    else
      echo "$stem: $dec's decode differs from the reconstruction ($n frames)"
      status=1
    fi
  done
  ffmpeg -hide_banner -f rawvideo -pix_fmt "$fmt" -s "${w}x${h}" -i "$work/$stem.yuv" -f rawvideo -pix_fmt "$fmt" -s "${w}x${h}" -i "$work/${stem}_src.yuv" \
    -lavfi psnr -f null - 2>&1 | grep -o "PSNR y:[^ ]* u:[^ ]* v:[^ ]* average:[^ ]*" | sed "s/^/$stem: /" || true
}
check av1_8 yuv420p 200 120 11
check av1_10 yuv420p10le 128 96 5
exit $status
