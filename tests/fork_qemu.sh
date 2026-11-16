#!/usr/bin/env bash
# fork(), wait and signals with their handlers on the real kernel under QEMU. Four programs are put at /Linux/usr/bin/forktest in turn and started by the
# kernel's autorun: tests/linux/forktest.c and signaltest.c (static, no libc: the calls themselves, handlers entered after calls, at timer ticks and on
# faults), spawntest.c and handlertest.c (static, glibc: fork(), posix_spawn(), abort(), signal(), siglongjmp out of a SIGSEGV handler, pthread_kill(),
# a SIGCHLD handler), ipctest.c (static, glibc: System V IPC, POSIX message queues and timers, inotify, pidfd, asynchronous I/O) and nettest.c (static,
# glibc: TCP and UDP sockets over the loopback, IPv4 and IPv6) and resolvetest.c (static, glibc: hosts and services by name, from /etc and from DNS
# through QEMU's DNS server; it needs this machine's own resolver to answer for ".localhost" and ".invalid", as systemd-resolved does). What each prints must be what the same program prints on this machine's own Linux kernel, line for line.
# Then signaltest.c as an AArch64, a RISC-V and a LoongArch program, which the kernel runs on its CPU emulator.
# Uses its own build folder (build-fork-qemu) so the normal build keeps no autorun.
set -euo pipefail
cd "$(dirname "$0")/.."
build=${BUILD:-build-fork-qemu}
work=${TMPDIR:-/tmp}/tcos-fork-qemu
rm -rf "$work"; mkdir -p "$work"
for program in forktest signaltest; do
  gcc -O2 -static -nostdlib -ffreestanding -fno-stack-protector -fno-pic -no-pie -mno-red-zone -fno-asynchronous-unwind-tables -fno-builtin -o "$work/$program" "tests/linux/$program.c"
done
for program in spawntest handlertest ipctest nettest resolvetest; do
  gcc -static -O1 -pthread -o "$work/$program" "tests/linux/$program.c" 2>/dev/null
done
# (A statically linked glibc program that looks hosts up cannot be run on this machine for what it should print: the C library then loads this machine's
# own lookup modules into it, which a static program does not survive. The same source linked the usual way prints it instead.)
gcc -O1 -o "$work/resolvetest-reference" tests/linux/resolvetest.c
cmake -S . -B "$build" -G Ninja -DTCOS_AUTORUN=/Linux/usr/bin/forktest >/dev/null
cmake --build "$build" >/dev/null
cmake --build "$build" --target vars >/dev/null
# The run-std target's QEMU command, with no sound and no window, the debug console going to debugcon.log.
qemu=$(ninja -C "$build" -t commands run-std | tail -1)
qemu=$(sed -E 's/-audiodev [a-z]+,/-audiodev none,/' <<< "$qemu")
status=0
# Starts one program as the kernel's autorun and compares what it prints with `expected`.
run_one() {
  local program=$1 elf=$2 expected=$3 like=$4 file=${1%% *}
  "$build/tools/primary_image" "$build/primary.img" "file:/Linux/usr/bin/forktest=$elf" >/dev/null
  rm -f "$build/debugcon.log"
  timeout "${RUN_SECONDS:-90}" bash -c "$qemu -display none" </dev/null >/dev/null 2>&1 || true
  # The program's lines, in the order they came (the kernel's own messages are in the same log).
  tr -d '\r' < "$build/debugcon.log" | grep -a -x -F -f "$expected" > "$work/$file-got.txt" || true
  if diff -u "$expected" "$work/$file-got.txt" > "$work/$file-diff.txt"; then
    echo "$program: $(wc -l < "$work/$file-got.txt") lines, as $like"
  else
    cat "$work/$file-diff.txt"
    status=1
  fi
  grep -a "autorun: ended" "$build/debugcon.log" || true
  grep -a -q -E "autorun: ended, result 0, exit code 0x0+( |$)" "$build/debugcon.log" || status=1
}
# Each program in turn is the one the kernel starts (at the autorun's path), after it ran here for what it should print.
for program in ${NATIVE-forktest signaltest spawntest handlertest ipctest nettest resolvetest}; do      # (NATIVE="ipctest" EMULATED="" runs one)
  reference=$program
  [ "$program" = resolvetest ] && reference=resolvetest-reference
  (cd "$work" && "./$reference" > "$program-expected.txt")
  run_one "$program" "$work/$program" "$work/$program-expected.txt" "on Linux"
done
# The same signal program built for the other CPUs runs on the kernel's CPU emulator (checked-in builds and what they print under qemu's user mode:
# tests/linux/build_signaltest.sh).
for cpu in ${EMULATED-aarch64 riscv64 loongarch64}; do
  run_one "signals-$cpu (emulated)" "tests/data/emulation/signals-$cpu.elf" "tests/data/emulation/signals-$cpu-expected.txt" "under qemu-$cpu"
done
[ $status -eq 0 ] && echo "fork qemu: passed" || echo "fork qemu: FAILED"
exit $status
