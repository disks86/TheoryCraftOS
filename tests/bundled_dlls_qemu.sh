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
# A Windows program that brings its own Direct3D libraries, on the real kernel under QEMU (docs/WindowsUserMode.md, "Libraries of the same name"):
# tests/windows/bundletest.c in C:\BundleA with "foreign" d3d9.dll, d3d12.dll, d3d11.dll and d3d10.dll beside it (tests/windows/bundle_foreign.c), and the
# same program in C:\BundleB with ReShade-style proxies of d3d9.dll and dxgi.dll (tests/windows/bundle_proxy.c), which the first starts. This OS's ddraw.dll,
# d3d8.dll, d3d10.dll, d3d10_1.dll and d3d11.dll have to draw on their own System32 layers, and the program's LoadLibrary and GetModuleHandle of the names have
# to be its own files. Runs on the standard VGA card (the kernel's CPU renderer), once for each width.
# Passes if the program says all its checks passed and the autorun sees exit code 42.
# Usage: tests/bundled_dlls_qemu.sh [64] [32]      (no argument: both; BUILD=<folder> for the build, default build-bundled-qemu)
set -euo pipefail
cd "$(dirname "$0")/.."
build=${BUILD:-build-bundled-qemu}
work=${TMPDIR:-/tmp}/tcos-bundled-qemu
widths=("$@")
[ ${#widths[@]} -eq 0 ] && widths=(64 32)
rm -rf "$work"; mkdir -p "$work"
# (the two shaders the program draws with, as C arrays: the kernel's own fixtures)
python3 - "$work/dxfixtures.h" <<'PY'
import sys
out = open(sys.argv[1], "w")
for name in ("vs", "ps_color"):
    data = open("tests/data/dxbc/%s.dxbc" % name, "rb").read()
    out.write("static const unsigned char dx_%s_dxbc[] = {%s};\n" % (name, ",".join(str(b) for b in data)))
PY
cmake -S . -B "$build" -G Ninja "-DTCOS_AUTORUN=/Windows/BundleA/bundletest.exe" "-DTCOS_AUTORUN_ARGUMENTS=" -DTCOS_AUTORUN_ELEVATED=OFF -DTCOS_TRACE_SERVICE_FAILURES=${TRACE:-OFF} >/dev/null
cmake --build "$build" -j "${JOBS:-8}" >"$work/build.log" 2>&1 || { tail -30 "$work/build.log"; exit 1; }
cmake --build "$build" --target vars >/dev/null
status=0
for width in "${widths[@]}"; do
  if [ "$width" = 32 ]; then cc=i686-w64-mingw32-gcc; else cc=x86_64-w64-mingw32-gcc; fi
  out="$work/$width"
  mkdir -p "$out/a" "$out/b"
  # (the DirectDraw and Direct3D 8 draws: their headers cannot be in one file with d3d9.h)
  $cc -O1 -Wall -Wextra -DBUNDLE_DDRAW -c -o "$out/ddraw.o" tests/windows/bundletest_legacy.c
  $cc -O1 -Wall -Wextra -DBUNDLE_D3D8 -c -o "$out/d3d8.o" tests/windows/bundletest_legacy.c
  $cc -O1 -Wall -Wextra -Wno-unused-parameter -I "$work" -o "$out/bundletest.exe" tests/windows/bundletest.c "$out/ddraw.o" "$out/d3d8.o" -ldxguid -luuid -luser32
  for library in d3d9 d3d12 d3d11 d3d10; do
    $cc -O1 -shared -Wl,--kill-at "-DFOREIGN_$(tr a-z A-Z <<< "$library")" -o "$out/a/$library.dll" tests/windows/bundle_foreign.c
  done
  $cc -O1 -shared -Wl,--kill-at -DPROXY_D3D9 -o "$out/b/d3d9.dll" tests/windows/bundle_proxy.c -luuid
  $cc -O1 -shared -Wl,--kill-at -DPROXY_DXGI -o "$out/b/dxgi.dll" tests/windows/bundle_proxy.c -luuid
  "$build/tools/primary_image" "$build/primary.img" "file:/Windows/BundleA/bundletest.exe=$out/bundletest.exe" "file:/Windows/BundleB/bundletest.exe=$out/bundletest.exe" \
    "file:/Windows/BundleA/d3d9.dll=$out/a/d3d9.dll" "file:/Windows/BundleA/d3d12.dll=$out/a/d3d12.dll" "file:/Windows/BundleA/d3d11.dll=$out/a/d3d11.dll" \
    "file:/Windows/BundleA/d3d10.dll=$out/a/d3d10.dll" "file:/Windows/BundleB/d3d9.dll=$out/b/d3d9.dll" "file:/Windows/BundleB/dxgi.dll=$out/b/dxgi.dll" >/dev/null
  # (the run target's command, without its QMP socket - the path is too long for QEMU in a deep checkout - and with no window)
  read -r -a words <<< "$(ninja -C "$build" -t commands run-std | tail -1 | sed -E 's/^cd [^&]* && //; s/-audiodev [a-z]+,/-audiodev none,/')"
  args=(); skip=0
  for a in "${words[@]}"; do
    if [ $skip = 1 ]; then skip=0; elif [ "$a" = -qmp ]; then skip=1; else args+=("$a"); fi
  done
  args+=(-display none)
  rm -f "$build/debugcon.log"
  (cd "$build" && timeout "${QEMU_SECONDS:-180}" "${args[@]}" </dev/null >/dev/null 2>"$out/qemu-err.txt") || true
  cp "$build/debugcon.log" "$out/debugcon.log" 2>/dev/null || true
  grep -a -E "^bundletest|autorun:|PANIC|user program:|not loaded|missing" "$build/debugcon.log" || echo "no output from bundletest ($width-bit)"
  ok=1
  [ "$(grep -a -c -E "^bundletest: all [0-9]* checks passed" "$build/debugcon.log")" -ge 1 ] || ok=0
  grep -a -q -E "autorun: ended, result 0, exit code 0x0*2a " "$build/debugcon.log" || ok=0
  if [ $ok = 1 ]; then echo "bundled dlls qemu ($width-bit): passed"; else echo "bundled dlls qemu ($width-bit): FAILED"; status=1; fi
done
exit $status
