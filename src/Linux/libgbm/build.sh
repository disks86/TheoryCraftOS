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
# Builds libgbm for Linux programs (src/Linux/libgbm/libgbm.c: the Generic Buffer Manager over the kernel's Direct3D layer through /dev/dxg) against the
# development machine's glibc and Mesa's gbm.h (the same ABI; x86-64: it has no other C library):
#   <out-dir>/libgbm.so.1     installed in the Linux tree as /usr/lib/libgbm.so.1
# Usage: src/Linux/libgbm/build.sh <out-dir>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build.sh <out-dir>}
mkdir -p "$out"
CC=${CC:-gcc}
"$CC" -std=gnu11 -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare \
  -shared -Wl,-soname,libgbm.so.1 -Wl,-z,now -Wl,-z,defs -o "$out/libgbm.so.1" "$here/libgbm.c" -lpthread
