#!/usr/bin/env bash
# Rebuilds the LoongArch signal frames that are checked in (tests/data/emulation/sigframes-loongarch64-<kind>.txt): what tests/linux/sigframe_loongarch.c
# prints under qemu's user mode, which follows the Linux kernel's signal code. Three builds of the program, by the registers it has used when the signal
# comes: the floating-point ones (fpu), the 128-bit vector ones (lsx) and the 256-bit ones (lasx). LinuxSyscallTest compares the frames made here with them
# and returns through them. Needs clang, an ld.lld (rust's will do) and qemu-loongarch64.
set -euo pipefail
cd "$(dirname "$0")/../.."
lld=$(command -v ld.lld || ls "$HOME"/.rustup/toolchains/*/lib/rustlib/*/bin/gcc-ld/ld.lld 2>/dev/null | head -1)
[ -n "$lld" ] || { echo "no ld.lld"; exit 1; }
work=${TMPDIR:-/tmp}/tcos-sigframe
ulimit -c 0
mkdir -p "$work"
vectors=0
for kind in fpu lsx lasx; do
  # (-mno-lsx: the compiler must not use the vector registers by itself, or every build would be the lsx one)
  clang --target=loongarch64-linux-gnu -mno-lsx -DVECTORS=$vectors -O2 -ffreestanding -nostdlib -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables \
        -fno-builtin -c tests/linux/sigframe_loongarch.c -o "$work/sigframe-$kind.o"
  "$lld" -static -e _start -o "$work/sigframe-$kind.elf" "$work/sigframe-$kind.o"
  out="tests/data/emulation/sigframes-loongarch64-$kind.txt"
  # (the address the frame is at changes from run to run: the test does not need it)
  qemu-loongarch64 "$work/sigframe-$kind.elf" | sed -E 's/^at [0-9a-f]+ /at - /' > "$out"
  grep -q "^frames ok$" "$out" || { echo "$kind: the program did not run to its end under qemu"; exit 1; }
  echo "$kind: $(grep -c '^frame ' "$out") frames, $(sed -n 2p "$out")"
  vectors=$((vectors + 1))
done
