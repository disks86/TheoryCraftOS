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

# Build, then run QEMU in the foreground with the gdbstub on :1234.
# OS output appears on stdout, so run this from an IDE run tab.
set -euo pipefail
cd "$(dirname "$0")/.."
# Build dir: $BUILD_DIR, else the first of build/ or CLion's cmake-build-debug/.
BUILD=${BUILD_DIR:-$(for d in build cmake-build-debug; do [ -f "$d/CMakeCache.txt" ] && echo "$d" && break; done)}

# cmake may not be on PATH (e.g. only bundled with the IDE); the cache knows it.
CMAKE=$(sed -n 's/^CMAKE_COMMAND:INTERNAL=//p' "$BUILD/CMakeCache.txt")
[ -n "${NO_BUILD:-}" ] || "$CMAKE" --build "$BUILD" --target esp vars
[ -n "${NO_BUILD:-}" ] || rm -f "$BUILD/debugcon.log"
# QEMU_GPU: std (default, Bochs VGA), virtio (virtio-gpu, 2D scanout) or gl (virtio-gpu with virgl 3D).
case ${QEMU_GPU:-std} in
  gl) GPU_ARGS="-vga none -device virtio-gpu-gl-pci"; DISPLAY_ARGS="gtk,gl=on,show-cursor=off" ;;
  virtio) GPU_ARGS="-vga none -device virtio-gpu-pci"; DISPLAY_ARGS="gtk,show-cursor=off" ;;
  std) GPU_ARGS=""; DISPLAY_ARGS="gtk,show-cursor=off" ;;
  *) echo "unknown QEMU_GPU=$QEMU_GPU (std, virtio, gl)" >&2; exit 2 ;;
esac
exec qemu-system-x86_64 -machine q35 -smp ${QEMU_SMP:-4} -m 8G -netdev user,id=net0 -device virtio-net-pci,netdev=net0 -rtc base=localtime $GPU_ARGS -device virtio-tablet-pci -display ${QEMU_DISPLAY:-$DISPLAY_ARGS} -s \
  -drive if=pflash,format=raw,readonly=on,file="$(sed -n 's/^OVMF_CODE:FILEPATH=//p' "$BUILD/CMakeCache.txt")" \
  -drive if=pflash,format=raw,file="$BUILD/OVMF_VARS.fd" \
  -drive format=raw,file=fat:rw:"$BUILD/esp" \
  -chardev stdio,id=dbg,logfile="$BUILD/debugcon.log",signal=off \
  -device isa-debugcon,iobase=0x402,chardev=dbg -serial null
