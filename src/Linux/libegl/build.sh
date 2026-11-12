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
# Builds libEGL for Linux programs (src/Linux/libegl/libegl.c: EGL, OpenGL ES and OpenGL over the kernel's OpenGL through /dev/dxg), against the development
# machine's glibc and libwayland-client (x86-64: it has no other C library):
#   <out-dir>/libEGL.so.1     all of it: EGL and every GL command (installed in the Linux tree as /usr/lib/libEGL.so.1)
#   <out-dir>/libGLESv2.so.2, libGL.so.1, libOpenGL.so.0
#                             empty libraries that need libEGL.so.1, where Mesa's (glvnd's) GL libraries would be: a program linked against them, or one that
#                             opens them with dlopen and looks commands up with dlsym, reaches the one copy of the GL state in libEGL.so.1. (No GLX: libGL.so.1
#                             is there for EGL programs that link it for desktop GL.)
#   <out-dir>/libgbm.so.1     (src/Linux/libgbm/build.sh) which libEGL.so.1 needs: GBM displays, and the GPU buffers of Wayland window surfaces
# Usage: src/Linux/libegl/build.sh <out-dir>
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: build.sh <out-dir>}
mkdir -p "$out"
CC=${CC:-gcc}
"$here/../libgbm/build.sh" "$out"
"$CC" -std=gnu11 -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare \
  -shared -Wl,-soname,libEGL.so.1 -Wl,-z,now -Wl,-z,defs -o "$out/libEGL.so.1" "$here/libegl.c" -lwayland-client "$out/libgbm.so.1"
for lib in libGLESv2.so.2 libGL.so.1 libOpenGL.so.0; do
  echo "" | "$CC" -x c -shared -fPIC -Wl,-soname,$lib -Wl,--no-as-needed -o "$out/$lib" - -x none "$out/libEGL.so.1"
done
