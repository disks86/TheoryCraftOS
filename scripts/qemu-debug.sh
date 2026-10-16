#!/usr/bin/env bash
# Build, then run QEMU in the foreground with the gdbstub on :1234.
# OS output appears on stdout, so run this from an IDE run tab.
set -euo pipefail
cd "$(dirname "$0")/.."
# Build dir: $BUILD_DIR, else the first of build/ or CLion's cmake-build-debug/.
BUILD=${BUILD_DIR:-$(for d in build cmake-build-debug; do [ -f "$d/CMakeCache.txt" ] && echo "$d" && break; done)}

cmake --build "$BUILD" --target esp vars
rm -f "$BUILD/debugcon.log"
exec qemu-system-x86_64 -machine q35 -m 256M -net none -display ${QEMU_DISPLAY:-gtk} -s \
  -drive if=pflash,format=raw,readonly=on,file="$(sed -n 's/^OVMF_CODE:FILEPATH=//p' "$BUILD/CMakeCache.txt")" \
  -drive if=pflash,format=raw,file="$BUILD/OVMF_VARS.fd" \
  -drive format=raw,file=fat:rw:"$BUILD/esp" \
  -chardev stdio,id=dbg,logfile="$BUILD/debugcon.log",signal=off \
  -device isa-debugcon,iobase=0x402,chardev=dbg -serial null
