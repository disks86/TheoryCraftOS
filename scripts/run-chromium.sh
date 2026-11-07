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
# Boots the OS in QEMU with the Windows build of Chromium on the primary disk and starts it once the desktop is up (TCOS_AUTORUN).
#   scripts/run-chromium.sh <chrome-win folder> [chrome arguments ...]
# The folder is the unpacked chrome-win.zip of a Chromium snapshot (commondatastorage.googleapis.com/chromium-browser-snapshots/Win_x64/<rev>/chrome-win.zip). It goes
# to C:\Program Files\Chromium on a primary disk of its own (build/chromium.img, 2 GiB, made once and refreshed with the system DLLs on every run). The VM runs headless
# with the standard VGA card for CHROMIUM_SECONDS (default 300) seconds or until the program ends; the debug console goes to build/chromium.log and a screenshot of the
# end to build/chromium.ppm. Environment: CHROMIUM_SMP (cores, 4), CHROMIUM_MEMORY (8G), CHROMIUM_QMP (socket path, short: QEMU limits it to 107 bytes), CHROMIUM_EXTRA
# (more files for System32, space-separated).
set -euo pipefail
cd "$(dirname "$0")/.."
src=$(cd "$1" && pwd); shift
build=${CHROMIUM_BUILD:-build}
image=$build/chromium.img
log=$build/chromium.log
qmp=${CHROMIUM_QMP:-/tmp/tcos-chromium-$$.qmp}
args=$(IFS=';'; echo "$*")
cmake -S . -B "$build" -DTCOS_USERMODE_DEMO=OFF -DTCOS_WINE_TEST_PROGRAM= -DTCOS_AUTORUN="/Windows/Program Files/Chromium/chrome.exe" "-DTCOS_AUTORUN_ARGUMENTS=$args" >/dev/null
ninja -C "$build" -j8 esp primary vars >"$build/chromium-build.log" 2>&1 || { tail -30 "$build/chromium-build.log"; exit 1; }
# The disk: the system DLLs as the primary image has them, then Chromium's files (only those it needs to run: the test programs are left out).
if [ ! -f "$image" ]; then
  truncate -s 2G "$image"
  mkfs.fat -F 32 -n TCOS "$image" >/dev/null
fi
files=()
for dll in "$build"/windows/*.dll ${CHROMIUM_EXTRA:-}; do files+=("file:/Windows/Windows/System32/$(basename "$dll")=$dll"); done
stamp="$build/chromium.stamp"
if [ ! -f "$stamp" ] || [ "$src" != "$(cat "$stamp")" ]; then
  while IFS= read -r -d '' f; do
    rel=${f#"$src"/}
    case "$rel" in interactive_ui_tests.exe|*.zip) continue;; esac
    files+=("file:/Windows/Program Files/Chromium/$rel=$f")
  done < <(find "$src" -type f -print0)
fi
"$build/tools/primary_image" "$image" "${files[@]}"
echo "$src" > "$stamp"
: > "$log"
rm -f "$qmp"
qemu-system-x86_64 -accel kvm -cpu host -machine q35 -smp "${CHROMIUM_SMP:-4}" -m "${CHROMIUM_MEMORY:-8G}" \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 -device virtio-tablet-pci -device virtio-rng-pci \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd -drive if=pflash,format=raw,file="$build/OVMF_VARS.fd" \
  -drive format=raw,file=fat:rw:"$build/esp" -drive if=none,id=data,format=raw,file="$image" -device virtio-blk-pci,drive=data \
  -chardev file,id=dbg,path="$log" -device isa-debugcon,iobase=0x402,chardev=dbg -serial null \
  -vga none -device VGA,vgamem_mb=64 -display none -qmp unix:"$qmp",server=on,wait=off &
pid=$!
for _ in $(seq 1 "${CHROMIUM_SECONDS:-300}"); do
  kill -0 $pid 2>/dev/null || break
  grep -q "autorun: ended" "$log" 2>/dev/null && break
  sleep 1
done
if kill -0 $pid 2>/dev/null; then
  printf '{"execute":"qmp_capabilities"}\n{"execute":"screendump","arguments":{"filename":"%s"}}\n' "$PWD/$build/chromium.ppm" | socat - UNIX-CONNECT:"$qmp" >/dev/null 2>&1 || true
  sleep 1
  kill $pid 2>/dev/null || true
fi
wait $pid 2>/dev/null || true
rm -f "$qmp"
grep -a "autorun\|user program\|exception\|Fault\|fault" "$log" | tail -40
