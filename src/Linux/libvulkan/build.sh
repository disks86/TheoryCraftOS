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
# Builds libvulkan for Linux programs (src/Linux/libvulkan/libvulkan.c: the shared driver of src/Vulkan over /dev/dxg), freestanding, with clang:
#   <out-dir>/<cpu>/libvulkan.a     for programs linked statically: clang --target=<cpu>-linux-gnu -static ... <out-dir>/<cpu>/libvulkan.a
#   <out-dir>/<cpu>/libvulkan.so.1  for programs of a C library (glibc's ld.so loads it): it takes the Khronos loader's place, so a program built against the
#                                   loader (-lvulkan) runs on it unchanged; it is installed in the Linux tree as /usr/lib/libvulkan.so.1
# Usage: src/Linux/libvulkan/build.sh <out-dir> [x86_64|aarch64|riscv64|loongarch64 ...]   (default: x86_64)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build.sh <out-dir> [cpu ...]}
shift
cpus=("$@")
[ ${#cpus[@]} -gt 0 ] || cpus=(x86_64)
CC=${CC:-clang}
AR=${AR:-llvm-ar}
for cpu in "${cpus[@]}"; do
  case $cpu in
    x86_64) extra=() ;;
    aarch64) extra=(-mno-outline-atomics) ;;      # (atomics inline: there is no libgcc to call)
    riscv64) extra=(-march=rv64gc) ;;
    loongarch64) extra=() ;;
    *) echo "build.sh: unknown cpu $cpu" >&2; exit 2 ;;
  esac
  mkdir -p "$out/$cpu"
  "$CC" --target="$cpu-linux-gnu" "${extra[@]}" -std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables \
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers -Wno-sign-compare \
    -c "$here/libvulkan.c" -o "$out/$cpu/libvulkan.o"
  rm -f "$out/$cpu/libvulkan.a"
  "$AR" rcs "$out/$cpu/libvulkan.a" "$out/$cpu/libvulkan.o"
  # the shared library: position independent, only the vk* functions exported (the C library's memcpy family, dlopen and environ come from the program)
  "$CC" --target="$cpu-linux-gnu" "${extra[@]}" -std=gnu11 -O2 -ffreestanding -fno-stack-protector -fPIC -fvisibility=hidden -fno-asynchronous-unwind-tables -DVK_SHARED \
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-missing-field-initializers -Wno-sign-compare \
    -shared -nostdlib -fuse-ld=lld -Wl,-soname,libvulkan.so.1 -Wl,--hash-style=both -Wl,-z,now -o "$out/$cpu/libvulkan.so.1" "$here/libvulkan.c"
done
