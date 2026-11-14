#!/usr/bin/env bash
# statfs / fstatfs, link and flock on the real kernel under QEMU: tests/linux/locktest.c (static) is put at /Linux/usr/bin/locktest and started by the kernel's
# autorun. Passes if every check passes, the program exits with 7 and the kernel saw no system call it does not have. Uses its own build folder
# (build-lock-qemu) so the normal build keeps no autorun.
set -euo pipefail
cd "$(dirname "$0")/.."
build=${BUILD:-build-lock-qemu}
work=${TMPDIR:-/tmp}/tcos-lock-qemu
rm -rf "$work"; mkdir -p "$work"
gcc -static -O1 -pthread -o "$work/locktest" tests/linux/locktest.c
cmake -S . -B "$build" -G Ninja -DTCOS_AUTORUN=/Linux/usr/bin/locktest >/dev/null
cmake --build "$build" >/dev/null
cmake --build "$build" --target vars >/dev/null
"$build/tools/primary_image" "$build/primary.img" "file:/Linux/usr/bin/locktest=$work/locktest" >/dev/null
# The run-std target's QEMU command, with no sound and no window, the debug console going to debugcon.log.
qemu=$(ninja -C "$build" -t commands run-std | tail -1)
qemu=$(sed -E 's/-audiodev [a-z]+,/-audiodev none,/' <<< "$qemu")
rm -f "$build/debugcon.log"
timeout 90 bash -c "$qemu -display none" </dev/null >/dev/null 2>&1 || true
grep -a -E "locktest:|autorun:" "$build/debugcon.log" || echo "no output from the program"
status=0
grep -a -q "locktest: [0-9]* passed, 0 failed" "$build/debugcon.log" || status=1
grep -a -q "autorun: ended, result 0, exit code 0x0*7 " "$build/debugcon.log" || status=1
grep -a -q "does not have" "$build/debugcon.log" && status=1
[ $status -eq 0 ] && echo "lock qemu: passed" || echo "lock qemu: FAILED"
exit $status
