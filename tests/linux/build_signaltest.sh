#!/usr/bin/env bash
# Rebuilds the AArch64 and RISC-V builds of tests/linux/signaltest.c that are checked in (tests/data/emulation/signals-<cpu>.elf) and what they print on a
# real Linux kernel's signal code (signals-<cpu>-expected.txt), taken from a run under qemu's user mode. LinuxForkTest runs the programs on this OS's
# emulator and compares. Needs clang, an ld.lld (rust's will do), qemu-aarch64 and qemu-riscv64.
set -euo pipefail
cd "$(dirname "$0")/../.."
lld=$(command -v ld.lld || ls "$HOME"/.rustup/toolchains/*/lib/rustlib/*/bin/gcc-ld/ld.lld 2>/dev/null | head -1)
[ -n "$lld" ] || { echo "no ld.lld"; exit 1; }
work=${TMPDIR:-/tmp}/tcos-signaltest
ulimit -c 0      # (two of the program's children end by SIGSEGV on purpose: qemu must not leave core files for them)
mkdir -p "$work"
for cpu in aarch64 riscv64; do
  march=""
  [ "$cpu" = riscv64 ] && march="-march=rv64gc"
  clang --target=$cpu-linux-gnu $march -O2 -ffreestanding -nostdlib -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables -fno-builtin \
        -c tests/linux/signaltest.c -o "$work/signaltest-$cpu.o"
  "$lld" -static -e _start -o "tests/data/emulation/signals-$cpu.elf" "$work/signaltest-$cpu.o"
  # (qemu itself reports the two children that end by SIGSEGV on purpose; that is not the program's output)
  "qemu-$cpu" "tests/data/emulation/signals-$cpu.elf" > "tests/data/emulation/signals-$cpu-expected.txt" 2>/dev/null
  grep -q "^signals ok$" "tests/data/emulation/signals-$cpu-expected.txt" || { echo "$cpu: the program did not run to its end under qemu"; exit 1; }
  echo "$cpu: $(wc -l < "tests/data/emulation/signals-$cpu-expected.txt") lines"
done
