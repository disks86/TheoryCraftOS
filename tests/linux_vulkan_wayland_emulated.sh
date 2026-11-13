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
# VK_KHR_wayland_surface: tests/linux/vkswapchain.c, a glibc program with libwayland-client and an xdg-shell toplevel, renders through Vulkan swapchains
# into its window. Vulkan is libvulkan.so.1 of src/Linux/libvulkan (installed at /usr/lib/libvulkan.so.1 of the Linux tree, where the Khronos loader would
# be), the compositor is the kernel's. It runs on the kernel's CPU emulator (the -DTCOS_EMU_DXG -DTCOS_EMU_WAYLAND build of tests/wayland_dmabuf_emulated.sh,
# whose own test runs first): the Direct3D layer over the software renderer, the compositor, and a stand-in desktop that draws the window every other time on
# the adapter's device (the swapchain image sampled in place) and else on the CPU. The program must pass its checks and its window reach the desktop with the
# last frame's colour, drawn both ways.
# Usage: tests/linux_vulkan_wayland_emulated.sh   (x86-64 only: the development machine has no AArch64 or RISC-V C library). Needs clang with ld.lld,
# libwayland-client and wayland-scanner, and wayland-protocols.
set -euo pipefail
cd "$(dirname "$0")/.."
emu=${TMPDIR:-/tmp}/tcos-wldmabuf
TMPDIR=${TMPDIR:-/tmp} tests/wayland_dmabuf_emulated.sh x86_64 > "$emu.build.log" 2>&1 || { tail -20 "$emu.build.log"; echo "the emulator build (or its own test) failed"; exit 1; }
work=${TMPDIR:-/tmp}/tcos-vkwayland
mkdir -p "$work"
src/Linux/libvulkan/build.sh "$work/lib" x86_64
lib=$work/lib/x86_64/libvulkan.so.1
for p in stable/xdg-shell/xdg-shell unstable/xdg-decoration/xdg-decoration-unstable-v1; do
  wayland-scanner client-header "/usr/share/wayland-protocols/$p.xml" "$work/$(basename $p)-client-protocol.h"
  wayland-scanner private-code "/usr/share/wayland-protocols/$p.xml" "$work/$(basename $p)-protocol.c"
done
gcc -O1 -g -Wall -I "$work" -o "$work/vkswapchain" tests/linux/vkswapchain.c "$work/xdg-shell-protocol.c" "$work/xdg-decoration-unstable-v1-protocol.c" "$lib" -lwayland-client

# the program's interpreter and libraries at their own paths, this libvulkan.so.1 at the system's
files=$( { LD_LIBRARY_PATH=$work/lib/x86_64 ldd "$work/vkswapchain" | awk '/=>/ { if ($3 ~ /^\// && $1 != "libvulkan.so.1") print $3 } /^\t\// { print $1 }'
           readelf -l "$work/vkswapchain" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p'; } | sort -u | paste -sd:)
files="$files:$lib=/usr/lib/libvulkan.so.1"
fail=0
echo "== vkswapchain (glibc, libwayland-client, libvulkan.so.1)"
out=$work/vkswapchain.out
TCOS_EMU_LINUX_FILES=$files "$emu/emurun" "$work/vkswapchain" | tee "$out"
grep -q "vk-swapchain: [0-9]* checks, 0 failed" "$out" || fail=1
grep -q "faulted=0" "$out" || fail=1
# the window, framed, showing the last frame (purple, 0xFF8040C0, within a step of rounding)
grep -Eq 'framed window "Linux Vulkan swapchain" pixel ff(7f|80)(3f|40)(bf|c0)' "$out" || { echo "the window did not reach the desktop with its last frame"; fail=1; }
grep -Eq "wayland-desktop: [1-9][0-9]* draws on the device, [1-9][0-9]* on the CPU" "$out" || { echo "the desktop did not draw the swapchain's images both ways"; fail=1; }

# the same program through the Khronos loader (the development machine's libvulkan.so.1) with this driver as its installable client driver: libvulkan_tcos.so
# and its manifest at the system's paths, which the loader finds by itself
if [ -e /usr/lib/libvulkan.so.1 ]; then
  loader=$(readlink -f /usr/lib/libvulkan.so.1)
  lfiles=$( { ldd "$work/vkswapchain" | awk '/=>/ { if ($3 ~ /^\// && $1 != "libvulkan.so.1") print $3 } /^\t\// { print $1 }'
              ldd "$loader" | awk '/=>/ { if ($3 ~ /^\//) print $3 }'
              readelf -l "$work/vkswapchain" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p'; } | sort -u | paste -sd:)
  lfiles="$lfiles:$loader=/usr/lib/libvulkan.so.1:$work/lib/x86_64/libvulkan_tcos.so=/usr/lib/libvulkan_tcos.so"
  lfiles="$lfiles:$work/lib/x86_64/tcos_icd.x86_64.json=/usr/share/vulkan/icd.d/tcos_icd.x86_64.json"
  echo "== vkswapchain through the Khronos loader ($(basename "$loader")) and libvulkan_tcos.so"
  out=$work/vkswapchain-loader.out
  TCOS_EMU_ENV=TCOS_VK_LOADER=1 TCOS_EMU_LINUX_FILES=$lfiles "$emu/emurun" "$work/vkswapchain" | tee "$out"
  grep -q "vk-swapchain: [0-9]* checks, 0 failed" "$out" || fail=1
  grep -q "faulted=0" "$out" || fail=1
  grep -Eq 'framed window "Linux Vulkan swapchain" pixel ff(7f|80)(3f|40)(bf|c0)' "$out" || { echo "the window did not reach the desktop through the loader"; fail=1; }
  grep -Eq "wayland-desktop: [1-9][0-9]* draws on the device, [1-9][0-9]* on the CPU" "$out" || { echo "the desktop did not draw the swapchain's images (loader)"; fail=1; }
else
  echo "(no Khronos loader on this machine: the installable client driver was not tried)"
fi
[ $fail -eq 0 ] && echo "linux vulkan wayland emulated: all passed" || echo "linux vulkan wayland emulated: FAILED"
exit $fail
