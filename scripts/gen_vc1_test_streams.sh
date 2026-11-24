#!/bin/bash
# Makes the VC-1 test streams in tests/data (vc1_*.rcv: Simple and Main profile in the RCV container; vc1_*.vc1: Advanced profile elementary streams) and the pictures a
# reference decoder makes of them (vc1_*.yuv, I420, all frames in display order). The streams come from scripts/gen_vc1_synthetic.py, which writes random but valid syntax
# (every syntax element, all picture types, 4MV, intensity compensation, B and BI pictures, bit plane modes, the quantizer variations, skipped frames, range reduction and
# range mapping, interlaced field and frame pictures, slices); ffmpeg's C code (-cpuflags 0: its SIMD transforms differ on out of range coefficients) is the reference, and
# the decoder must reproduce it exactly. FFmpeg does not apply the Advanced profile range mapping (RANGE_MAPY / RANGE_MAPUV), so the pictures of vc1_adv_rmap_* are
# FFmpeg's mapped with the formula of SMPTE 421M afterwards (gen_vc1_synthetic.py --rangemap). FFmpeg's Main profile range reduction (RANGEREDFRM) does not follow
# SMPTE 421M either (it doubles intra blocks, keeps a range reduced P or B picture half in reduced range, and scales references down whatever their own RANGEREDFRM;
# see the patch for the details), so the pictures of vc1_main_rr_* come from an FFmpeg 7.0 built with scripts/ffmpeg-vc1-range-reduction.patch, which makes it follow
# the standard (and leaves every other stream as it was): its ffmpeg program is $FFMPEG_RANGERED. Without it those two .yuv files are left as they are.
# Usage: [FFMPEG_RANGERED=/path/to/patched/ffmpeg] scripts/gen_vc1_test_streams.sh   (needs ffmpeg with vc1 and wmv3)
set -e
out=tests/data
gen="$(dirname "$0")/gen_vc1_synthetic.py"
python3 -I "$gen" $out
for f in $out/vc1_*.rcv $out/vc1_*.vc1; do
  fmt=vc1test; case $f in *.vc1) fmt=vc1;; esac
  ff=ffmpeg
  case $f in */vc1_main_rr_*) if [ -z "$FFMPEG_RANGERED" ]; then echo "FFMPEG_RANGERED is not set: ${f%.*}.yuv not made" >&2; continue; fi; ff=$FFMPEG_RANGERED;; esac
  $ff -nostdin -hide_banner -loglevel fatal -y -flags +bitexact -cpuflags 0 -threads 1 -f $fmt -i "$f" -fps_mode passthrough -f rawvideo -pix_fmt yuv420p "${f%.*}.yuv"
  case $f in */vc1_adv_rmap_*) name=$(basename "${f%.*}"); python3 -I "$gen" --rangemap "${name#vc1_}" "${f%.*}.yuv";; esac
done
ls -l $out/vc1_*.rcv $out/vc1_*.vc1 | awk '{print $5, $9}'
