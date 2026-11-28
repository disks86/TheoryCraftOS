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
# Vulkan on the host's own driver (docs/Venus.md): a Windows program over vulkan-1.dll is started by the kernel's autorun on a machine whose graphics card is
# QEMU's virtio-gpu-gl with venus=on, so the driver's Venus backend sends every call to the host's Vulkan driver through virglrenderer (no window:
# egl-headless). The program is tests/windows/<program>.c (default venustest; vulkantest runs the Direct3D backend's 386 checks on the host's devices).
# Passes if the program's checks all pass, it ran on the host's driver (the Venus driver id), and the autorun sees it end with exit code 0.
# Uses its own build folder (build-venus-qemu, BUILD= to change) so the normal build keeps no autorun; a deep checkout path is too long for QEMU's sockets:
# run it through a short symlink to the checkout if QEMU complains.
# LINUX=1: tests/linux/<program>.c (default vulkantest) instead, a static Linux program over libvulkan.a, through /dev/dri/renderD128 (prints
# "vulkan-linux: N checks").
# Usage: tests/venus_qemu.sh [program]        HOSTMEM=<size> (default 1G): the device's host memory window. VENUS=off: the same machine without Venus (the
# Direct3D backend over virgl: what the program does when there is no host Vulkan). GPU_BACKEND=AUTO|VENUS|VIRGL: the kernel's own 3D device (TCOS_GPU_BACKEND).
set -euo pipefail
cd "$(dirname "$0")/.."
build=${BUILD:-build-venus-qemu}
work=${TMPDIR:-/tmp}/tcos-venus-qemu
linux=${LINUX:-0}
if [ "$linux" = 1 ]; then program=${1:-vulkantest}; else program=${1:-venustest}; fi
hostmem=${HOSTMEM:-1G}
venus=${VENUS:-on}
mkdir -p "$work"
if [ "$linux" = 1 ]; then
  src/Linux/libvulkan/build.sh "$work/lib" x86_64 >/dev/null 2>&1 || [ -f "$work/lib/x86_64/libvulkan.a" ] || { echo "libvulkan.a did not build"; exit 1; }
  # (linked with the system's linker: the program and the archive need no C library)
  clang -O1 -static -nostdlib -ffreestanding -fno-stack-protector -fno-pic -no-pie -fno-asynchronous-unwind-tables -Wall -Wextra -Wno-unused-parameter \
    -Wno-missing-field-initializers -o "$work/$program" "tests/linux/$program.c" "$work/lib/x86_64/libvulkan.a"
  autorun=/Linux/usr/bin/$program image="file:/Linux/usr/bin/$program=$work/$program" banner=vulkan-linux
else
  x86_64-w64-mingw32-gcc -O1 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type -o "$work/$program.exe" "tests/windows/$program.c"
  autorun=/Windows/$program.exe image="file:/Windows/$program.exe=$work/$program.exe" banner=$program
fi
cmake -S . -B "$build" -G Ninja "-DTCOS_AUTORUN=$autorun" -DTCOS_AUTORUN_ARGUMENTS= -DTCOS_AUTORUN_ELEVATED=OFF "-DTCOS_GPU_BACKEND=${GPU_BACKEND:-AUTO}" >/dev/null
cmake --build "$build" -j "${JOBS:-8}" >"$work/build.log" 2>&1 || { tail -30 "$work/build.log"; exit 1; }
cmake --build "$build" --target vars >/dev/null
"$build/tools/primary_image" "$build/primary.img" "$image" >/dev/null
# (the run-gl target's command, without its QMP socket, with no window, and the virtio-gpu with Venus, blobs and a host memory window)
read -r -a words <<< "$(ninja -C "$build" -t commands run-gl | tail -1 | sed -E 's/^cd [^&]* && //; s/-audiodev [a-z]+,/-audiodev none,/')"
args=(); skip=0
for a in "${words[@]}"; do
  if [ $skip = 1 ]; then skip=0; elif [ "$a" = -qmp ]; then skip=1; else
    case "$a" in
      gtk,gl=on*) a=egl-headless ;;
      virtio-gpu-gl-pci*) if [ "$venus" = off ]; then a=virtio-gpu-gl-pci; else a="virtio-gpu-gl-pci,venus=on,blob=on,hostmem=$hostmem"; fi ;;
    esac
    args+=("$a")
  fi
done
rm -f "$build/debugcon.log"
(cd "$build" && timeout "${QEMU_SECONDS:-180}" "${args[@]}" </dev/null >/dev/null 2>"$work/qemu-err.txt") || true
grep -a -E "^$banner|^FAIL|^venus|virtio-gpu capsets|Venus|autorun:|PANIC|user program:" "$build/debugcon.log" || echo "no output from $program"
status=0
grep -a -q -E "^$banner: [0-9]* checks, 0 failed" "$build/debugcon.log" || status=1
grep -a -q -E "autorun: ended, result 0, exit code 0x0* " "$build/debugcon.log" || status=1
[ $status -eq 0 ] && echo "venus qemu: passed" || echo "venus qemu: FAILED"
exit $status
