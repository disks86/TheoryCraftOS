#!/usr/bin/env bash
# 32-bit Windows programs on the real kernel under QEMU (docs/Wow64.md). Two programs, each started by the kernel's autorun in a boot of its own:
#   hello32  tests/windows/hello32.c: PE32 for i386 with no libraries. It runs in the CPU's compatibility mode and calls the NT services through INT 2E.
#   crt32    tests/windows/crt32.c: built the ordinary way, with a C runtime; it runs on the 32-bit ntdll.dll, kernel32.dll and msvcrt.dll of
#            C:\Windows\SysWOW64 (the heap, files, the environment, thread-local storage, threads).
# Passes if all the checks of each pass and the autorun sees exit code 42. Uses its own build folder (build-wow64-qemu) so the normal build keeps no
# autorun. Needs i686-w64-mingw32-gcc.
# Usage: tests/wow64_qemu.sh [hello32] [crt32]      (no arguments: both)
set -euo pipefail
cd "$(dirname "$0")/.."
build=${BUILD:-build-wow64-qemu}
work=${TMPDIR:-/tmp}/tcos-wow64-qemu
rm -rf "$work"; mkdir -p "$work"
programs=("$@")
[ ${#programs[@]} -eq 0 ] && programs=(hello32 crt32)
i686-w64-mingw32-gcc -O1 -ffreestanding -nostdlib -Isrc/Windows -Wl,--entry=_start32 -Wl,--subsystem,console -o "$work/hello32.exe" tests/windows/hello32.c
i686-w64-mingw32-gcc -O1 -o "$work/crt32.exe" tests/windows/crt32.c
status=0
for program in "${programs[@]}"; do
  cmake -S . -B "$build" -G Ninja "-DTCOS_AUTORUN=/Windows/$program.exe" >/dev/null
  cmake --build "$build" >/dev/null
  cmake --build "$build" --target vars >/dev/null
  "$build/tools/primary_image" "$build/primary.img" "file:/Windows/$program.exe=$work/$program.exe" >/dev/null
  qemu=$(ninja -C "$build" -t commands run-std | tail -1)
  qemu=$(sed -E 's/-audiodev [a-z]+,/-audiodev none,/' <<< "$qemu")
  rm -f "$build/debugcon.log"
  timeout "${QEMU_SECONDS:-120}" bash -c "$qemu -display none" </dev/null >/dev/null 2>&1 || true
  grep -a -E "$program:|autorun:|PANIC|user program:" "$build/debugcon.log" || echo "no output from $program"
  grep -a -q "$program: all .*checks passed" "$build/debugcon.log" || status=1
  grep -a -q "autorun: ended, result 0, exit code 0x0*2a " "$build/debugcon.log" || status=1
done
[ $status -eq 0 ] && echo "wow64 qemu: passed" || echo "wow64 qemu: FAILED"
exit $status
