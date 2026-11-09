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
# Fetches piglit's tests (the OpenGL test suite of the Mesa project, MIT/X11 licensed: not vendored) at a pinned commit into a cache and prints the directory.
#   tests/fetch_piglit.sh            -> $PIGLIT_CACHE (default ~/.cache/tcos/piglit-<commit>)
# tests/PiglitShaderTest.cpp runs the .shader_test files of it; the expected failures are tests/data/piglit/shader_test.xfail.
set -euo pipefail
commit=715bb8dd01bc7a3eb2cc76d8f12851bc787116df
dir=${PIGLIT_CACHE:-$HOME/.cache/tcos/piglit-$commit}
if [ ! -f "$dir/.complete" ]; then
  mkdir -p "$dir"
  tmp=$(mktemp -d)
  curl -sSfL -o "$tmp/piglit.tar.gz" "https://gitlab.freedesktop.org/mesa/piglit/-/archive/$commit/piglit-$commit.tar.gz?path=tests"
  tar -xzf "$tmp/piglit.tar.gz" -C "$dir" --strip-components=1
  rm -rf "$tmp"
  touch "$dir/.complete"
fi
echo "$dir"
