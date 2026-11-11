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
# Runs tests/linux/vulkantest.c - a Linux program over libvulkan.a, the Vulkan driver vulkan-1.dll shares (src/Vulkan) - on the kernel's CPU emulator with the kernel's
# Direct3D behind /dev/dxg: win32k's D3DKMT services over a dxgk::Kernel whose one adapter is the software renderer (tests/EmulationRun.cpp with TCOS_EMU_DXG, as
# tests/emulated_dx.sh does for Windows programs). One run per CPU: the same driver source on x86-64, AArch64 and RISC-V guests.
# Usage: tests/linux_vulkan.sh [x86_64|aarch64|riscv64 ...]      (default: all three)
# Needs clang with the Linux targets and ld.lld. Host objects are cached in $TMPDIR/tcos-linuxvk/obj (delete the folder after changing a header).
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-linuxvk
mkdir -p "$work/obj"

sources=(tests/EmulationRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Crypto/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/Kernel/Sync.cpp src/Kernel/Clipboard.cpp
         tests/KernelStubs.cpp src/Hardware/Keyboard/KeyCodes.cpp src/Process/Win32k/*.cpp src/Process/Win32k/Dxg/*.cpp src/Kernel/Dxgkrnl/*.cpp src/Math/FloatMath.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp
         src/Graphics/Graphics3D/Software/*.cpp src/Graphics/Graphics2D/Software/*.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp)
flags="-std=c++20 -O1 -pthread -I src -w -DTCOS_EMU_DXG"
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
fail=0
for cpu in "${cpus[@]}"; do
  case $cpu in
    aarch64) extra=(-mno-outline-atomics) ;;
    riscv64) extra=(-march=rv64gc -mno-relax) ;;
    *) extra=() ;;
  esac
  clang --target="$cpu-linux-gnu" "${extra[@]}" -O1 -static -nostdlib -ffreestanding -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables -fuse-ld=lld \
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -o "$work/vulkantest-$cpu" tests/linux/vulkantest.c "$work/lib/$cpu/libvulkan.a"
  echo "== vulkantest ($cpu)"
  "$work/emurun" "$work/vulkantest-$cpu" | tee "$work/vulkantest-$cpu.out"
  grep -q "checks, 0 failed" "$work/vulkantest-$cpu.out" || fail=1
  grep -q "faulted=0" "$work/vulkantest-$cpu.out" || fail=1
done
exit $fail
