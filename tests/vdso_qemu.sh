#!/usr/bin/env bash
# The Linux vDSO on the real kernel under QEMU: tests/linux/vdsotest.c, built static (glibc) at /Linux/usr/bin/vdsotest and dynamic (glibc's ld.so and
# libc.so.6 from this machine) at /Linux/usr/bin/vdsotest-dyn, started by the kernel's autorun as `vdsotest --then /usr/bin/vdsotest-dyn`. Each checks
# that AT_SYSINFO_EHDR names the vDSO, that its clock functions (and the C library's) agree with the system calls, in a thread and a forked child too,
# that the kernel published the counter rate in the data page (so clock reads stay in user mode), and that clock_gettime is much cheaper than the system
# call. Passes if both builds pass, the autorun sees exit code 7 and the kernel saw no system call it does not have. The same program is first run on this
# machine's own Linux as a sanity check. Uses its own build folder (build-vdso-qemu) so the normal build keeps no autorun.
set -euo pipefail
cd "$(dirname "$0")/.."
build=${BUILD:-build-vdso-qemu}
work=${TMPDIR:-/tmp}/tcos-vdso-qemu
rm -rf "$work"; mkdir -p "$work"
gcc -static -O1 -pthread -o "$work/vdsotest" tests/linux/vdsotest.c
gcc -O1 -pthread -o "$work/vdsotest-dyn" tests/linux/vdsotest.c
native=0; "$work/vdsotest" --then "$work/vdsotest-dyn" >/dev/null || native=$?
[ $native -eq 7 ] || { echo "vdso qemu: the test fails on this machine's Linux"; exit 1; }
libs=()
while read -r path; do
  [ -n "$path" ] && libs+=("file:/Linux$path=$(readlink -f "$path")")
done < <(ldd "$work/vdsotest-dyn" | grep -oE '/[^ ]+' | grep -v '^/Linux' | sort -u)
cmake -S . -B "$build" -G Ninja -DTCOS_AUTORUN=/Linux/usr/bin/vdsotest "-DTCOS_AUTORUN_ARGUMENTS=--then;/usr/bin/vdsotest-dyn" >/dev/null
cmake --build "$build" >/dev/null
cmake --build "$build" --target vars >/dev/null
"$build/tools/primary_image" "$build/primary.img" "file:/Linux/usr/bin/vdsotest=$work/vdsotest" "file:/Linux/usr/bin/vdsotest-dyn=$work/vdsotest-dyn" "${libs[@]}" >/dev/null
# The run-std target's QEMU command, with no sound and no window, the debug console going to debugcon.log.
qemu=$(ninja -C "$build" -t commands run-std | tail -1)
qemu=$(sed -E 's/-audiodev [a-z]+,/-audiodev none,/' <<< "$qemu")
rm -f "$build/debugcon.log"
timeout ${QEMU_SECONDS:-120} bash -c "$qemu -display none" </dev/null >/dev/null 2>&1 || true
grep -a -E "vdsotest|autorun:|does not have" "$build/debugcon.log" || echo "no output from the program"
status=0
grep -a -q "vdsotest: [0-9]* passed, 0 failed" "$build/debugcon.log" || status=1
grep -a -q "vdsotest-dynamic: [0-9]* passed, 0 failed" "$build/debugcon.log" || status=1
grep -a -q "vdsotest: TheoryCraftOS vDSO at" "$build/debugcon.log" || status=1
grep -a -q "autorun: ended, result 0, exit code 0x0*7 " "$build/debugcon.log" || status=1
grep -a -q "does not have" "$build/debugcon.log" && status=1
[ $status -eq 0 ] && echo "vdso qemu: passed" || echo "vdso qemu: FAILED"
exit $status
