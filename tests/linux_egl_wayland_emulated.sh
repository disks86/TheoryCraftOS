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
# EGL and OpenGL ES on Wayland: tests/linux/eglwayland.c, a glibc program with libwayland-client, libwayland-egl and an xdg-shell toplevel, draws with OpenGL
# ES 2.0 and 3.0 (and makes a desktop OpenGL 3.3 context) through libEGL.so.1 / libGLESv2.so.2 of src/Linux/libegl (installed at /usr/lib of the Linux tree, where
# Mesa's would be), over the kernel's OpenGL reached through /dev/dxg; its window surface's frames reach the kernel's compositor as wl_shm buffers. It runs on
# the kernel's CPU emulator (tests/EmulationRun.cpp with -DTCOS_EMU_DXG -DTCOS_EMU_WAYLAND -DTCOS_EMU_GL: the Direct3D layer, the compositor and a stand-in
# desktop that draws the windows, and the kernel's OpenGL). The program must pass its checks and its window reach the desktop with the last frame's colour.
# Usage: tests/linux_egl_wayland_emulated.sh   (x86-64 only: the development machine has no AArch64 or RISC-V C library). Needs gcc, libwayland-client,
# libwayland-egl and the EGL headers, wayland-scanner and wayland-protocols.
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-eglwayland
mkdir -p "$work/obj"
sources=(tests/EmulationRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Crypto/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/Kernel/Sync.cpp src/Kernel/Clipboard.cpp
         tests/KernelStubs.cpp src/Hardware/Keyboard/KeyCodes.cpp src/Process/Win32k/*.cpp src/Process/Win32k/Dxg/*.cpp src/Kernel/Dxgkrnl/*.cpp src/Math/FloatMath.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp
         src/Graphics/Graphics3D/Software/*.cpp src/Graphics/Graphics2D/Software/*.cpp src/Graphics/Graphics2D/Device/*.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp src/Wayland/*.cpp src/Wayland/Dxg/*.cpp
         src/OpenGl/*.cpp src/OpenGl/generated/GlApiVariants.cpp src/WebGl/*.cpp)
flags="-std=c++20 -O1 -pthread -I src -w -DTCOS_EMU_DXG -DTCOS_EMU_WAYLAND -DTCOS_EMU_GL"
objs=()
jobs=()
for f in "${sources[@]}"; do
  o="$work/obj/$(echo "$f" | tr '/' '_').o"
  objs+=("$o")
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$(find src -name '*.hpp' -newer "$o" -print -quit)" ] || [ src/Process/DxgDevice.h -nt "$o" ]; then jobs+=("$f"); fi
done
if [ ${#jobs[@]} -gt 0 ]; then
  printf '%s\n' "${jobs[@]}" | xargs -P "$(nproc)" -I{} sh -c 'o="'"$work"'/obj/$(echo "{}" | tr / _).o"; g++ '"$flags"' -c "{}" -o "$o" || { rm -f "$o"; exit 255; }'
fi
g++ -pthread "${objs[@]}" -o "$work/emurun"

src/Linux/libegl/build.sh "$work/lib"
for p in stable/xdg-shell/xdg-shell unstable/xdg-decoration/xdg-decoration-unstable-v1; do
  wayland-scanner client-header "/usr/share/wayland-protocols/$p.xml" "$work/$(basename $p)-client-protocol.h"
  wayland-scanner private-code "/usr/share/wayland-protocols/$p.xml" "$work/$(basename $p)-protocol.c"
done
gcc -O1 -g -Wall -I "$work" -o "$work/eglwayland" tests/linux/eglwayland.c "$work/xdg-shell-protocol.c" "$work/xdg-decoration-unstable-v1-protocol.c" \
  "$work/lib/libEGL.so.1" "$work/lib/libGLESv2.so.2" -lwayland-egl -lwayland-client -ldl -lpthread

# the program's interpreter and libraries at their own paths, these EGL and GL libraries at the system's
ours="libEGL.so.1 libGLESv2.so.2 libGL.so.1 libOpenGL.so.0"
files=$( { LD_LIBRARY_PATH=$work/lib ldd "$work/eglwayland" | awk -v ours="$ours" '/=>/ { if ($3 ~ /^\// && index(" " ours " ", " " $1 " ") == 0) print $3 } /^\t\// { print $1 }'
           readelf -l "$work/eglwayland" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p'; } | sort -u | paste -sd:)
for l in $ours; do files="$files:$work/lib/$l=/usr/lib/$l"; done
fail=0
echo "== eglwayland (glibc, libwayland-client, libwayland-egl, libEGL.so.1, libGLESv2.so.2)"
out=$work/eglwayland.out
TCOS_EMU_LINUX_FILES=$files "$work/emurun" "$work/eglwayland" | tee "$out"
grep -q "egl-wayland: [0-9]* checks, 0 failed" "$out" || fail=1
grep -q "faulted=0" "$out" || fail=1
# the window, framed, showing the last frame (purple, 0xFF8040C0, within a step of rounding)
grep -Eq 'framed window "Linux EGL window" pixel ff(7f|80)(3f|40)(bf|c0)' "$out" || { echo "the window did not reach the desktop with its last frame"; fail=1; }
[ $fail -eq 0 ] && echo "linux egl wayland emulated: all passed" || echo "linux egl wayland emulated: FAILED"
exit $fail
