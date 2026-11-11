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
# GPU buffers through the kernel's Wayland compositor, end to end through the OS's own system calls: tests/linux/waylandvktest.c (no C library; x86-64,
# AArch64 and RISC-V) renders with Vulkan (libvulkan.a over /dev/dxg), exports its image as a dmabuf (vkGetMemoryFdKHR: /dev/dxg's export), passes it with
# SCM_RIGHTS in zwp_linux_buffer_params_v1.add and shows it in a decorated toplevel, frame after frame rendered on the GPU into the same image. It runs on the
# kernel's CPU emulator (tests/EmulationRun.cpp with -DTCOS_EMU_DXG -DTCOS_EMU_WAYLAND: the Direct3D layer over the software renderer, the compositor, its
# transport, and a stand-in desktop drawing every other time with a 2D renderer on the adapter's device - the image sampled in place - and else on the CPU).
# Each must pass its checks, its window reach the desktop with the last frame's colour, and the desktop have drawn it both ways.
# Usage: tests/wayland_dmabuf_emulated.sh [x86_64|aarch64|riscv64 ...]   (default: all three). Needs clang with the Linux targets and ld.lld.
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-wldmabuf
mkdir -p "$work/obj"
sources=(tests/EmulationRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Crypto/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/Kernel/Sync.cpp src/Kernel/Clipboard.cpp
         tests/KernelStubs.cpp src/Hardware/Keyboard/KeyCodes.cpp src/Process/Win32k/*.cpp src/Process/Win32k/Dxg/*.cpp src/Kernel/Dxgkrnl/*.cpp src/Math/FloatMath.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp
         src/Graphics/Graphics3D/Software/*.cpp src/Graphics/Graphics2D/Software/*.cpp src/Graphics/Graphics2D/Device/*.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp src/Wayland/*.cpp src/Wayland/Dxg/*.cpp)
flags="-std=c++20 -O1 -pthread -I src -w -DTCOS_EMU_DXG -DTCOS_EMU_WAYLAND"
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

cpus=("$@")
[ ${#cpus[@]} -gt 0 ] || cpus=(x86_64 aarch64 riscv64)
src/Linux/libvulkan/build.sh "$work/lib" "${cpus[@]}"
lld=$(ls "$HOME"/.rustup/toolchains/*/lib/rustlib/*/bin/gcc-ld/ld.lld 2>/dev/null | head -1 || true)
linker=(-fuse-ld=lld)
[ -n "$lld" ] && linker=("--ld-path=$lld")
fail=0
for cpu in "${cpus[@]}"; do
  case $cpu in
    aarch64) extra=(-mno-outline-atomics) ;;
    riscv64) extra=(-march=rv64gc -mno-relax) ;;
    *) extra=() ;;
  esac
  clang --target="$cpu-linux-gnu" "${extra[@]}" -O1 -static -nostdlib -ffreestanding -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables -fno-builtin "${linker[@]}" \
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -o "$work/waylandvktest-$cpu" tests/linux/waylandvktest.c "$work/lib/$cpu/libvulkan.a"
  echo "== wayland vulkan client ($cpu)"
  out="$work/$cpu.out"
  "$work/emurun" "$work/waylandvktest-$cpu" | tee "$out"
  grep -q "wayland-vulkan: [0-9]* checks, 0 failed" "$out" || fail=1
  grep -q "faulted=0" "$out" || fail=1
  # the window, framed, showing the last frame (teal, 0xFF00BF80, within a step of rounding)
  grep -Eq 'framed window "Linux Vulkan dmabuf" pixel ff00(be|bf|c0)(7f|80|81)' "$out" || { echo "the window did not reach the desktop with its last frame"; fail=1; }
  grep -Eq "wayland-desktop: [1-9][0-9]* draws on the device, [1-9][0-9]* on the CPU" "$out" || { echo "the desktop did not draw the GPU buffer both ways"; fail=1; }
done
[ $fail -eq 0 ] && echo "wayland dmabuf emulated: all passed" || echo "wayland dmabuf emulated: FAILED"
exit $fail
