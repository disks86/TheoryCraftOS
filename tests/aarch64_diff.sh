#!/usr/bin/env bash
# Differential test of the AArch64 emulator against qemu-aarch64: random instructions with random register state are assembled into one program, run under both,
# and every register, NZCV, FPSR and a scratch buffer are compared. Not part of run_host_tests.sh because it needs clang (aarch64 target), an AArch64-capable lld
# (the one in rustup's toolchain works), llvm-mc and qemu-aarch64 (run with -cpu max).
# Usage: tests/aarch64_diff.sh [mode...] [--seeds N] [--filter REGEX] [--perkey N]   modes: int int2 fp fp16 neon mem words (default: int fp neon mem);
# words runs LLVM-decoded random instructions (all of them valid for the all-features CPU), at most --perkey per mnemonic/operand shape, optionally only those matching REGEX.
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-a64diff
mkdir -p "$work"
lld=${LD_LLD:-$(find "$HOME/.rustup" -name ld.lld -path '*gcc-ld*' 2>/dev/null | head -1)}
[ -n "$lld" ] || lld=$(command -v ld.lld) || { echo "no AArch64-capable ld.lld found (set LD_LLD)"; exit 2; }
modes=(); seeds=3; filter=""; perkey=4
while [ $# -gt 0 ]; do case $1 in --seeds) seeds=$2; shift 2 ;; --filter) filter=$2; shift 2 ;; --perkey) perkey=$2; shift 2 ;; *) modes+=("$1"); shift ;; esac; done
[ ${#modes[@]} -gt 0 ] || modes=(int fp neon mem)
g++ -std=c++20 -O1 -I src -w tests/EmulationRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/Kernel/Sync.cpp tests/KernelStubs.cpp -pthread -o "$work/emurun" &
g++ -std=c++20 -O1 -I src -w tests/aarch64_diff/probe.cpp src/Emulation/*.cpp src/Process/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/Kernel/Sync.cpp tests/KernelStubs.cpp -pthread -o "$work/probe" &
wait
# qprobe: runs each word under qemu and reports the ones it does not implement (SIGILL), so that instructions only LLVM knows are not compared
clang --target=aarch64-linux-gnu -march=armv9.5-a -O1 -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -c tests/aarch64_diff/qprobe.c -o "$work/qprobe.o"
clang --target=aarch64-linux-gnu -march=armv9.5-a -c tests/aarch64_diff/qstart.s -o "$work/qstart.o"
"$lld" -static -nostdlib -e _start "$work/qstart.o" "$work/qprobe.o" -o "$work/qprobe.elf"
export A64_PROBE="$work/probe" A64_QPROBE="$work/qprobe.elf"
fail=0
for mode in "${modes[@]}"; do
  for seed in $(seq 1 "$seeds"); do
    if [ "$mode" = words ]; then python3 tests/aarch64_diff/words.py "$seed" "$work" "$filter" "$perkey"; fi
    if [ "$mode" = svemem ]; then SVEMEM=1 python3 tests/aarch64_diff/words.py "$seed" "$work" "$filter" "$perkey"; fi
    python3 tests/aarch64_diff/gen.py "$mode" "$seed" "$work"
    llvm-mc -triple=aarch64-linux-gnu -mattr=+all -filetype=obj "$work/t.s" -o "$work/t.o"
    "$lld" -static -nostdlib -e _start "$work/t.o" -o "$work/t.elf"
    qemu-aarch64 -cpu max,sve-max-vq=1,sme128=on "$work/t.elf" > "$work/q.bin"
    "$work/emurun" "$work/t.elf" > "$work/e.raw" || true
    echo "== $mode seed $seed"
    python3 tests/aarch64_diff/compare.py "$work" || fail=1
  done
done
exit $fail
