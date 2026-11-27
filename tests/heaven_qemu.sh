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
# Unigine Heaven 4.0 (the Windows build, 32-bit) under QEMU on the 3D device: the kernel's autorun starts the engine through cmd.exe with the
# graphics API given (opengl, direct3d9 or direct3d11) in benchmark mode (PHORONIX: the engine runs the benchmark once and quits, writing the
# result to its log), with no window on the host (egl-headless) and the screen read over a VNC socket every SHOT seconds into $work/shot-*.png.
# The disk must already have Heaven installed: SOURCE (default build-winget-qemu/primary.img, which `KEEP=ON tests/winget_qemu.sh
# Unigine.HeavenBenchmark` leaves) is copied into the build folder the first time (FRESH=1: again).
# Prints the engine's log (C:\Users\Default\Heaven\log.html) and the kernel's GL/Direct3D statistics; passes if the benchmark wrote its score.
# Usage: tests/heaven_qemu.sh [opengl|direct3d9|direct3d11]
#   MINUTES (default 25) QEMU's time limit; WIDTH x HEIGHT (default 1280x720); TESSELLATION (DISABLED, MODERATE, NORMAL, EXTREME; default NORMAL,
#   which Direct3D 9 does not have); QUALITY (LOW, MEDIUM, HIGH, ULTRA; default HIGH); GPU_BACKEND (AUTO, VENUS, VIRGL; default AUTO) with
#   STD=1 for the standard VGA card (no 3D device: the CPU renderer draws); VENUS=on to give the device Venus; BENCHMARK=0 to run the demo without the benchmark (it does not quit; MINUTES ends it); PROFILE=<n>: after the
#   n-th screenshot a 60-second sampling profile (scripts/qemu-profile.py) into $work/profile.txt.
# Uses its own build folder (build-heaven-qemu, BUILD= to change); screenshots and logs go in WORK (default $TMPDIR/tcos-heaven-<api>).
set -euo pipefail
cd "$(dirname "$0")/.."
api=${1:-opengl}
build=${BUILD:-build-heaven-qemu}
work=${WORK:-${TMPDIR:-/tmp}/tcos-heaven-$api}
source_image=${SOURCE:-build-winget-qemu/primary.img}
minutes=${MINUTES:-25}
shot=${SHOT:-60}
width=${WIDTH:-1280} height=${HEIGHT:-720}
quality=${QUALITY:-HIGH}
tessellation=${TESSELLATION:-NORMAL}
[ "$api" = direct3d9 ] && tessellation=DISABLED
defines="RELEASE,LANGUAGE_EN,QUALITY_$quality,TESSELLATION_$tessellation"
[ "${BENCHMARK:-1}" = 1 ] && defines="PHORONIX,$defines"
case "$api" in opengl|direct3d9|direct3d11) ;; *) echo "unknown API $api"; exit 2 ;; esac
mkdir -p "$work"
rm -f "$work"/shot-*.png
# (forward slashes: a backslash in the define becomes a C escape)
heaven_bin="C:/Program Files/Unigine/Heaven Benchmark 4.0/bin"
arguments="/c;start;/wait;/d;$heaven_bin;heaven.exe;-project_name;Heaven;-data_path;../;-engine_config;../data/heaven_4.0.cfg;-system_script;heaven/unigine.cpp"
arguments+=";-sound_app;null;-video_app;$api;-video_multisample;0;-video_fullscreen;0;-video_mode;-1;-video_width;$width;-video_height;$height"
arguments+=";-extern_define;$defines;-extern_plugin;GPUMonitor"
cmake -S . -B "$build" -G Ninja -DTCOS_AUTORUN=/Windows/Windows/System32/cmd.exe "-DTCOS_AUTORUN_ARGUMENTS=$arguments" -DTCOS_AUTORUN_ELEVATED=OFF \
  "-DTCOS_GPU_BACKEND=${GPU_BACKEND:-AUTO}" >/dev/null
if [ ! -f "$build/primary.img" ] || [ "${FRESH:-0}" = 1 ]; then
  [ -f "$source_image" ] || { echo "no image with Heaven installed at $source_image (KEEP=ON tests/winget_qemu.sh Unigine.HeavenBenchmark)"; exit 1; }
  cp --sparse=always "$source_image" "$build/primary.img"
fi
cmake --build "$build" -j "${JOBS:-8}" >"$work/build.log" 2>&1 || { grep -E "error|FAILED" "$work/build.log" | head -30; exit 1; }
cmake --build "$build" --target vars >/dev/null
# The engine's log of the last run would be read as this one's.
"$build/tools/primary_image" "$build/primary.img" "file:/Windows/Users/Default/Heaven/log.html=/dev/null" >/dev/null 2>&1 || true
# (the run-gl target's command without its QMP socket and window: egl-headless, a VNC socket to read the screen and a monitor socket)
# (STD=1: the standard VGA card - no 3D device, so Direct3D and OpenGL draw on the CPU renderer: a reference for what the GPU path should show)
target=run-gl
[ "${STD:-0}" = 1 ] && target=run-std
read -r -a words <<< "$(ninja -C "$build" -t commands $target | tail -1 | sed -E 's/^cd [^&]* && //; s/-audiodev [a-z]+,/-audiodev none,/')"
args=(); skip=0
for a in "${words[@]}"; do
  if [ $skip = 1 ]; then skip=0; elif [ "$a" = -qmp ]; then skip=1; else
    case "$a" in
      gtk,gl=on*) a=egl-headless ;;
      # (a screen large enough for the window: the device's preferred mode)
      virtio-gpu-gl-pci) if [ "${VENUS:-off}" = off ]; then a="virtio-gpu-gl-pci,xres=${SCREEN_WIDTH:-1920},yres=${SCREEN_HEIGHT:-1080}"; else a="virtio-gpu-gl-pci,venus=on,blob=on,hostmem=${HOSTMEM:-2G},xres=${SCREEN_WIDTH:-1920},yres=${SCREEN_HEIGHT:-1080}"; fi ;;
    esac
    args+=("$a")
  fi
done
# (UNIX socket paths are at most 107 bytes: the sockets go in a short folder of their own)
sockets=$(mktemp -d /tmp/tcos-heaven.XXXXXX)
vnc=$sockets/vnc monitor=$sockets/mon
rm -f "$build/debugcon.log"
"${args[@]}" -vnc "unix:$vnc" -monitor "unix:$monitor,server=on,wait=off" </dev/null >"$work/qemu.log" 2>&1 &
qemu_pid=$!
trap 'kill $qemu_pid 2>/dev/null || true; rm -rf "$sockets"' EXIT
start=$(date +%s) n=0
while kill -0 $qemu_pid 2>/dev/null; do
  now=$(date +%s)
  [ $((now - start)) -ge $((minutes * 60)) ] && break
  sleep "$shot" || true
  n=$((n + 1))
  python3 -I scripts/vnc-screenshot.py "$vnc" "$work/shot-$(printf %03d $n).png" 2>/dev/null || true
  # PROFILE=<minute>: a sampling profile of the CPUs for 60 seconds from that minute on, into $work/profile.txt (scripts/qemu-profile.py)
  if [ -n "${PROFILE:-}" ] && [ "$n" = "$PROFILE" ]; then
    python3 -I scripts/qemu-profile.py "$monitor" 60 "$build/BOOTX64.efi" "$build/debugcon.log" "$build/windows" "$build/windows32" > "$work/profile.txt" 2>&1 || true
  fi
  # (the autorun reports the program's end; the engine has quit once cmd.exe has)
  grep -a -q "autorun: ended" "$build/debugcon.log" 2>/dev/null && { sleep 5; break; }
done
kill $qemu_pid 2>/dev/null || true
wait $qemu_pid 2>/dev/null || true
trap - EXIT
rm -rf "$sockets"
echo "--- after $(( ($(date +%s) - start) / 60 )) min, screenshots in $work"
"$build/tools/primary_image" "$build/primary.img" "get:/Windows/Users/Default/Heaven/log.html=$work/log.html" >/dev/null 2>&1 || true
sed -E 's/<[^>]*>//g; s/&nbsp;/ /g; s/&lt;/</g; s/&gt;/>/g' "$work/log.html" 2>/dev/null | grep -v '^\s*$' > "$work/log.txt" || true
tail -40 "$work/log.txt" || true
echo "--- kernel"
grep -a -E "autorun|OpenGL|Direct3D|dxg|panic|Unhandled|fault" "$build/debugcon.log" | tail -40 || true
grep -a -q -E "Score:|FPS:" "$work/log.txt"
