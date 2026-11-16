#!/usr/bin/env bash
# fork(), wait, kill and the signal calls on the real kernel under QEMU: tests/linux/forktest.c (static, no libc: the calls themselves) and
# tests/linux/spawntest.c (static, glibc: fork(), posix_spawn(), abort(), exec in a child) are put at /Linux/usr/bin/forktest in turn and started by the
# kernel's autorun. What each prints must be what the same program prints on this machine's own Linux kernel, line for line.
# Uses its own build folder (build-fork-qemu) so the normal build keeps no autorun.
set -euo pipefail
cd "$(dirname "$0")/.."
build=${BUILD:-build-fork-qemu}
work=${TMPDIR:-/tmp}/tcos-fork-qemu
rm -rf "$work"; mkdir -p "$work"
gcc -O2 -static -nostdlib -ffreestanding -fno-stack-protector -fno-pic -no-pie -mno-red-zone -fno-asynchronous-unwind-tables -fno-builtin -o "$work/forktest" tests/linux/forktest.c
gcc -static -O1 -o "$work/spawntest" tests/linux/spawntest.c 2>/dev/null
cmake -S . -B "$build" -G Ninja -DTCOS_AUTORUN=/Linux/usr/bin/forktest >/dev/null
cmake --build "$build" >/dev/null
cmake --build "$build" --target vars >/dev/null
# The run-std target's QEMU command, with no sound and no window, the debug console going to debugcon.log.
qemu=$(ninja -C "$build" -t commands run-std | tail -1)
qemu=$(sed -E 's/-audiodev [a-z]+,/-audiodev none,/' <<< "$qemu")
status=0
# Each program in turn is the one the kernel starts (at the autorun's path), after it ran here for what it should print.
for program in forktest spawntest; do
  (cd "$work" && "./$program" > "$program-expected.txt")
  "$build/tools/primary_image" "$build/primary.img" "file:/Linux/usr/bin/forktest=$work/$program" >/dev/null
  rm -f "$build/debugcon.log"
  timeout "${RUN_SECONDS:-90}" bash -c "$qemu -display none" </dev/null >/dev/null 2>&1 || true
  # The program's lines, in the order they came (the kernel's own messages are in the same log).
  tr -d '\r' < "$build/debugcon.log" | grep -a -x -F -f "$work/$program-expected.txt" > "$work/$program-got.txt" || true
  if diff -u "$work/$program-expected.txt" "$work/$program-got.txt" > "$work/$program-diff.txt"; then
    echo "$program: $(wc -l < "$work/$program-got.txt") lines, as on Linux"
  else
    cat "$work/$program-diff.txt"
    status=1
  fi
  grep -a "autorun: ended" "$build/debugcon.log" || true
  grep -a -q -E "autorun: ended, result 0, exit code 0x0+( |$)" "$build/debugcon.log" || status=1
done
[ $status -eq 0 ] && echo "fork qemu: passed" || echo "fork qemu: FAILED"
exit $status
