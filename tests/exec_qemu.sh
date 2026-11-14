#!/usr/bin/env bash
# /proc/self/exe, readlink and execve on the real kernel under QEMU: tests/linux/exectest.c (static) is put at /Linux/usr/bin/exectest and started by the
# kernel's autorun; it execs itself and the second program exits with 7. Passes if both halves pass and the autorun sees exit code 7 (the code of the
# program the first one became). Uses its own build folder (build-exec-qemu) so the normal build keeps no autorun.
set -euo pipefail
cd "$(dirname "$0")/.."
build=${BUILD:-build-exec-qemu}
work=${TMPDIR:-/tmp}/tcos-exec-qemu
rm -rf "$work"; mkdir -p "$work"
gcc -static -O1 -o "$work/exectest" tests/linux/exectest.c
cmake -S . -B "$build" -G Ninja -DTCOS_AUTORUN=/Linux/usr/bin/exectest >/dev/null
cmake --build "$build" >/dev/null
cmake --build "$build" --target vars >/dev/null
"$build/tools/primary_image" "$build/primary.img" "file:/Linux/usr/bin/exectest=$work/exectest" >/dev/null
# The run-std target's QEMU command, with no sound and no window, the debug console going to debugcon.log.
qemu=$(ninja -C "$build" -t commands run-std | tail -1)
qemu=$(sed -E 's/-audiodev [a-z]+,/-audiodev none,/' <<< "$qemu")
rm -f "$build/debugcon.log"
timeout 90 bash -c "$qemu -display none" </dev/null >/dev/null 2>&1 || true
grep -a -E "exectest:|autorun:" "$build/debugcon.log" || echo "no output from the program"
status=0
grep -a -q "exectest: first: [0-9]* passed, 0 failed" "$build/debugcon.log" || status=1
grep -a -q "exectest: second: [0-9]* passed, 0 failed" "$build/debugcon.log" || status=1
grep -a -q "autorun: ended, result 0, exit code 0x0*7 " "$build/debugcon.log" || status=1
[ $status -eq 0 ] && echo "exec qemu: passed" || echo "exec qemu: FAILED"
exit $status
