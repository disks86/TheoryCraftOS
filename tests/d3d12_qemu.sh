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
# Direct3D 12 on the real kernel under QEMU, on the 3D hardware: tests/windows/d3d12test.c (dxgi.dll and d3d12.dll) is started by the kernel's autorun on a machine whose
# graphics card is QEMU's virtio-gpu-gl, so the kernel's Direct3D draws through virglrenderer on the host's GPU (no window: egl-headless). tests/emulated_dx.sh runs the same
# program on the CPU renderer; this is the run that shows the hardware path gives the same answers.
# GPU=std uses the standard VGA card instead (the kernel's CPU renderer), to tell a hardware difference from a kernel one.
# Passes if the program's checks all pass, it says the device it drew on was not the CPU renderer (GPU=virgl), and the autorun sees it end with exit code 0.
# Uses its own build folder (build-d3d12-qemu) so the normal build keeps no autorun.
# Usage: [ARGS="..."] tests/d3d12_qemu.sh [program]      (default d3d12test; the source is tests/windows/<program>.c; d3d11test and d3d9test run Direct3D 11 and 9 there
# too; ARGS="processes" d3dsharetest shares resources and fences with a second process it starts)
set -euo pipefail
cd "$(dirname "$0")/.."
build=${BUILD:-build-d3d12-qemu}
work=${TMPDIR:-/tmp}/tcos-d3d12-qemu
gpu=${GPU:-virgl}
program=${1:-d3d12test}
mkdir -p "$work"
# (the shaders the program draws with, as C arrays: the kernel's own fixtures)
python3 - "$work/dxfixtures.h" <<'PY'
import glob, os, sys
out = open(sys.argv[1], "w")
for path in sorted(glob.glob("tests/data/dxbc/*.dxbc") + glob.glob("tests/data/dxbc/*.dxil")):
    n = os.path.basename(path).replace(".", "_")
    data = open(path, "rb").read()
    out.write("static const unsigned char dx_%s[] = {%s};\n" % (n, ",".join(str(b) for b in data)))
PY
x86_64-w64-mingw32-gcc -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-const-variable -I "$work" -o "$work/$program.exe" "tests/windows/$program.c" -ld3d11 -ld3d9 -ldxgi -ld3d12 -luuid -ldxguid -luser32
cmake -S . -B "$build" -G Ninja "-DTCOS_AUTORUN=/Windows/$program.exe" "-DTCOS_AUTORUN_ARGUMENTS=${ARGS:-}" -DTCOS_AUTORUN_ELEVATED=OFF "-DTCOS_TRACE_SERVICE_FAILURES=${TRACE:-OFF}" >/dev/null
cmake --build "$build" -j "${JOBS:-8}" >"$work/build.log" 2>&1 || { tail -30 "$work/build.log"; exit 1; }
cmake --build "$build" --target vars >/dev/null
"$build/tools/primary_image" "$build/primary.img" "file:/Windows/$program.exe=$work/$program.exe" >/dev/null
if [ "$gpu" = virgl ]; then target=run-gl; else target=run-std; fi
# (the run target's command, without its QMP socket - the path is too long for QEMU in a deep checkout - and with no window)
read -r -a words <<< "$(ninja -C "$build" -t commands $target | tail -1 | sed -E 's/^cd [^&]* && //; s/-audiodev [a-z]+,/-audiodev none,/')"
args=(); skip=0
for a in "${words[@]}"; do
  if [ $skip = 1 ]; then skip=0; elif [ "$a" = -qmp ]; then skip=1; else case "$a" in gtk,gl=on*) a=egl-headless;; esac; args+=("$a"); fi
done
[ "$gpu" = virgl ] || args+=(-display none)
rm -f "$build/debugcon.log"
(cd "$build" && timeout "${QEMU_SECONDS:-180}" "${args[@]}" </dev/null >/dev/null 2>"$work/qemu-err.txt") || true
grep -a -E "^$program|^FAIL|^adapter|^device|autorun:|PANIC|user program:|direct3d stream refused" "$build/debugcon.log" || echo "no output from $program"
status=0
# (d3d11test says "all N checks passed" and ends with 42, and does not name its device)
grep -a -q -E "^$program: ([0-9]* checks, 0 failed|all [0-9]* checks passed)" "$build/debugcon.log" || status=1
grep -a -q -E "autorun: ended, result 0, exit code 0x0*(2a)? " "$build/debugcon.log" || status=1
if [ "$gpu" = virgl ] && [ "$program" != d3d11test ] && [ "$program" != d3d9test ] && [ "$program" != d3dsharetest ]; then grep -a -q "^device: hardware" "$build/debugcon.log" || { echo "the program did not draw on the 3D hardware"; status=1; }; fi
[ $status -eq 0 ] && echo "d3d12 qemu ($gpu): passed" || echo "d3d12 qemu ($gpu): FAILED"
exit $status
