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
#
# VK_KHR_xcb_surface and VK_KHR_xlib_surface: tests/linux/vkx11.c, a glibc program with libxcb, libX11 and libX11-xcb, renders through Vulkan swapchains into
# two X windows (one made with xcb, one with Xlib). Vulkan is libvulkan.so.1 of src/Linux/libvulkan (installed where the Khronos loader would be), the X server
# is the kernel's. It runs on the kernel's CPU emulator (tests/EmulationRun.cpp with -DTCOS_EMU_DXG -DTCOS_EMU_X11: the Direct3D layer over the software
# renderer, the X server and a stand-in desktop that draws the windows). The program must pass its checks and both windows reach the desktop with their last
# frame's colour. Then the same through the development machine's Khronos loader with libvulkan_tcos.so as its driver.
# Usage: tests/linux_vulkan_x11_emulated.sh   (x86-64 only). Needs clang with ld.lld, gcc, libxcb, libX11 and the Vulkan headers.
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-vkx11
mkdir -p "$work/obj"
sources=(tests/EmulationRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Crypto/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/Kernel/Sync.cpp
         tests/KernelStubs.cpp src/Hardware/Keyboard/KeyCodes.cpp src/Process/Win32k/*.cpp src/Process/Win32k/Dxg/*.cpp src/Kernel/Dxgkrnl/*.cpp src/Math/FloatMath.cpp
         src/Graphics/Graphics3D/BuiltinShaders.cpp src/Graphics/Graphics3D/Software/*.cpp src/Graphics/Graphics2D/Software/*.cpp src/Graphics/Graphics2D/Device/*.cpp
         src/Hardware/GraphicsProcessingUnit/Font8x16.cpp src/X11/*.cpp src/Kernel/Clipboard.cpp)
flags="-std=c++20 -O1 -pthread -I src -w -DTCOS_EMU_DXG -DTCOS_EMU_X11"
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

src/Linux/libvulkan/build.sh "$work/lib" x86_64
lib=$work/lib/x86_64/libvulkan.so.1
gcc -O1 -g -Wall -o "$work/vkx11" tests/linux/vkx11.c "$lib" -lxcb -lX11 -lX11-xcb

check() {      # <output file>
  grep -q "vk-x11: [0-9]* checks, 0 failed" "$1" || fail=1
  grep -q "faulted=0" "$1" || fail=1
  # the windows, framed, with their last frames (within a step of rounding): purple 0xFF8040C0 (xcb), teal 0xFF2080A0 (Xlib)
  grep -Eq 'framed window "Linux Vulkan xcb" pixel ff(7f|80)(3f|40)(bf|c0)' "$1" || { echo "the xcb window did not reach the desktop with its last frame"; fail=1; }
  grep -Eq 'framed window "Linux Vulkan xlib" pixel ff(1f|20)(7f|80)(9f|a0)' "$1" || { echo "the Xlib window did not reach the desktop with its last frame"; fail=1; }
}

# the program's interpreter and libraries at their own paths, this libvulkan.so.1 at the system's
files=$( { LD_LIBRARY_PATH=$work/lib/x86_64 ldd "$work/vkx11" | awk '/=>/ { if ($3 ~ /^\// && $1 != "libvulkan.so.1") print $3 } /^\t\// { print $1 }'
           readelf -l "$work/vkx11" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p'; } | sort -u | paste -sd:)
fail=0
echo "== vkx11 (glibc, libxcb, libX11, libvulkan.so.1)"
out=$work/vkx11.out
TCOS_EMU_ENV=TCOS_VK_X11_HOLD=1 TCOS_EMU_LINUX_FILES="$files:$lib=/usr/lib/libvulkan.so.1" "$work/emurun" "$work/vkx11" | tee "$out"
check "$out"

# the same through the Khronos loader (the development machine's libvulkan.so.1) with libvulkan_tcos.so as its installable client driver
if [ -e /usr/lib/libvulkan.so.1 ]; then
  loader=$(readlink -f /usr/lib/libvulkan.so.1)
  lfiles=$( { ldd "$work/vkx11" | awk '/=>/ { if ($3 ~ /^\// && $1 != "libvulkan.so.1") print $3 } /^\t\// { print $1 }'
              ldd "$loader" | awk '/=>/ { if ($3 ~ /^\//) print $3 }'
              readelf -l "$work/vkx11" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p'; } | sort -u | paste -sd:)
  lfiles="$lfiles:$loader=/usr/lib/libvulkan.so.1:$work/lib/x86_64/libvulkan_tcos.so=/usr/lib/libvulkan_tcos.so"
  lfiles="$lfiles:$work/lib/x86_64/tcos_icd.x86_64.json=/usr/share/vulkan/icd.d/tcos_icd.x86_64.json"
  echo "== vkx11 through the Khronos loader ($(basename "$loader")) and libvulkan_tcos.so"
  out=$work/vkx11-loader.out
  TCOS_EMU_ENV="TCOS_VK_LOADER=1 TCOS_VK_X11_HOLD=1" TCOS_EMU_LINUX_FILES=$lfiles "$work/emurun" "$work/vkx11" | tee "$out"
  check "$out"
else
  echo "(no Khronos loader on this machine: the installable client driver was not tried)"
fi
[ $fail -eq 0 ] && echo "linux vulkan x11 emulated: all passed" || echo "linux vulkan x11 emulated: FAILED"
exit $fail
