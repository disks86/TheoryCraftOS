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

# Build, then run QEMU in the foreground. QEMU_GDB=1 opens the gdbstub on :2159 (off by default: QEMU pauses the VM whenever any client connects to it).
# OS output appears on stdout, so run this from an IDE run tab.
set -euo pipefail
cd "$(dirname "$0")/.."
# Build dir: $BUILD_DIR, else the first of build/ or CLion's cmake-build-debug/.
BUILD=${BUILD_DIR:-$(for d in build cmake-build-debug; do [ -f "$d/CMakeCache.txt" ] && echo "$d" && break; done)}

# cmake may not be on PATH (e.g. only bundled with the IDE); the cache knows it.
CMAKE=$(sed -n 's/^CMAKE_COMMAND:INTERNAL=//p' "$BUILD/CMakeCache.txt")
[ -n "${NO_BUILD:-}" ] || "$CMAKE" --build "$BUILD" --target esp vars
[ -n "${NO_BUILD:-}" ] || rm -f "$BUILD/debugcon.log"
# A persistent, writable data disk (FAT32, volume "DATA"), created on first use. It shows up in the file explorer next to the boot disk,
# whose contents QEMU only fakes (fat:rw: is a host folder presented as a disk, not something to keep files on).
DATA_IMG=${DATA_IMG:-$BUILD/data.img}
if [ ! -f "$DATA_IMG" ]; then
  qemu-img create -f raw "$DATA_IMG" "${DATA_SIZE:-256M}" >/dev/null
  mkfs.fat -F 32 -n DATA "$DATA_IMG" >/dev/null
fi
# The window shows the guest's picture pixel for pixel (zoom-to-fit=off): a tiling window manager or a resized window would otherwise stretch it by a
# fraction and blur and wobble the text.
# QEMU_GPU: auto (default: gl when this QEMU has virtio-gpu-gl, else std), std (Bochs VGA with 64 MiB of video memory so the larger resolutions fit; QEMU_VGAMEM changes it), virtio (virtio-gpu, 2D scanout) or gl (virtio-gpu with virgl 3D).
GPU_CHOICE=${QEMU_GPU:-auto}
if [ "$GPU_CHOICE" = auto ]; then
  if qemu-system-x86_64 -device help 2>/dev/null | grep -q 'virtio-gpu-gl-pci'; then GPU_CHOICE=gl; else GPU_CHOICE=std; fi
fi
case $GPU_CHOICE in
  gl) GPU_ARGS="-vga none -device virtio-gpu-gl-pci"; DISPLAY_ARGS="gtk,gl=on,zoom-to-fit=off,show-cursor=off,show-menubar=off" ;;
  virtio) GPU_ARGS="-vga none -device virtio-gpu-pci"; DISPLAY_ARGS="gtk,zoom-to-fit=off,show-cursor=off,show-menubar=off" ;;
  std) GPU_ARGS="-vga none -device VGA,vgamem_mb=${QEMU_VGAMEM:-64}"; DISPLAY_ARGS="gtk,zoom-to-fit=off,show-cursor=off,show-menubar=off" ;;
  *) echo "unknown QEMU_GPU=$QEMU_GPU (auto, std, virtio, gl)" >&2; exit 2 ;;
esac
# QEMU_AUDIO: the host sound backend for the guest's HD Audio codec: auto (default: PipeWire, else PulseAudio, ALSA, SDL, whichever this QEMU has first),
# any QEMU -audiodev driver name (pipewire, pa, alsa, sdl, ...), wav (record to $BUILD/audio.wav, for checking without speakers) or none (discard).
AUDIO_CHOICE=${QEMU_AUDIO:-auto}
if [ "$AUDIO_CHOICE" = auto ]; then
  AUDIO_CHOICE=none
  DRIVERS=$(qemu-system-x86_64 -audiodev help 2>/dev/null || true)
  for d in pipewire pa alsa sdl; do
    if printf '%s\n' "$DRIVERS" | grep -qx "$d"; then AUDIO_CHOICE=$d; break; fi
  done
fi
AUDIO_OPTS=
[ "$AUDIO_CHOICE" != wav ] || AUDIO_OPTS=,path="$BUILD/audio.wav"
echo "QEMU audio backend: $AUDIO_CHOICE" >&2
AUDIO_ARGS="-audiodev $AUDIO_CHOICE,id=snd0$AUDIO_OPTS -device ich9-intel-hda -device hda-duplex,audiodev=snd0"

exec qemu-system-x86_64 -machine q35 -smp ${QEMU_SMP:-4} -m 8G -netdev user,id=net0 -device virtio-net-pci,netdev=net0 -rtc base=localtime $GPU_ARGS -device virtio-tablet-pci $AUDIO_ARGS -display ${QEMU_DISPLAY:-$DISPLAY_ARGS} ${QEMU_GDB:+-gdb tcp:127.0.0.1:2159} \
  -qmp unix:"$BUILD/qmp.sock",server=on,wait=off -d guest_errors,unimp -D "$BUILD/qemu-guest.log" \
  -drive if=pflash,format=raw,readonly=on,file="$(sed -n 's/^OVMF_CODE:FILEPATH=//p' "$BUILD/CMakeCache.txt")" \
  -drive if=pflash,format=raw,file="$BUILD/OVMF_VARS.fd" \
  -drive format=raw,file=fat:rw:"$BUILD/esp" \
  -drive if=none,id=data,format=raw,file="$DATA_IMG" -device virtio-blk-pci,drive=data \
  -chardev stdio,id=dbg,logfile="$BUILD/debugcon.log",signal=off \
  -device isa-debugcon,iobase=0x402,chardev=dbg -serial null
