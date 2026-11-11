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
# Builds libvulkan.a for Linux programs (src/Linux/libvulkan/libvulkan.c: the shared driver of src/Vulkan over /dev/dxg), freestanding, with clang.
# Usage: src/Linux/libvulkan/build.sh <out-dir> [x86_64|aarch64|riscv64|loongarch64 ...]   (default: x86_64)  ->  <out-dir>/<cpu>/libvulkan.a
# A program links it statically (there is no dynamic loader yet): clang --target=<cpu>-linux-gnu -static ... -L <out-dir>/<cpu> -lvulkan, or the archive by name.
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
done
