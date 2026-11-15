#!/usr/bin/env bash
# A 32-bit Windows program on the real kernel under QEMU: tests/windows/hello32.c (PE32 for i386, no libraries) is put at /Windows/hello32.exe and started
# by the kernel's autorun. It runs in the CPU's compatibility mode and calls the NT services through INT 2E. Passes if all of its checks pass and the
# autorun sees exit code 42. Uses its own build folder (build-wow64-qemu) so the normal build keeps no autorun.
set -euo pipefail
cd "$(dirname "$0")/.."
build=${BUILD:-build-wow64-qemu}
work=${TMPDIR:-/tmp}/tcos-wow64-qemu
rm -rf "$work"; mkdir -p "$work"
i686-w64-mingw32-gcc -O1 -ffreestanding -nostdlib -Isrc/Windows -Wl,--entry=_start32 -Wl,--subsystem,console -o "$work/hello32.exe" tests/windows/hello32.c
cmake -S . -B "$build" -G Ninja -DTCOS_AUTORUN=/Windows/hello32.exe >/dev/null
cmake --build "$build" >/dev/null
cmake --build "$build" --target vars >/dev/null
"$build/tools/primary_image" "$build/primary.img" "file:/Windows/hello32.exe=$work/hello32.exe" >/dev/null
qemu=$(ninja -C "$build" -t commands run-std | tail -1)
qemu=$(sed -E 's/-audiodev [a-z]+,/-audiodev none,/' <<< "$qemu")
rm -f "$build/debugcon.log"
timeout "${QEMU_SECONDS:-120}" bash -c "$qemu -display none" </dev/null >/dev/null 2>&1 || true
grep -a -E "hello32:|autorun:|PANIC|user program:" "$build/debugcon.log" || echo "no output from the program"
status=0
grep -a -q "hello32: all checks passed" "$build/debugcon.log" || status=1
grep -a -q "autorun: ended, result 0, exit code 0x0*2a " "$build/debugcon.log" || status=1
[ $status -eq 0 ] && echo "wow64 qemu: passed" || echo "wow64 qemu: FAILED"
exit $status
