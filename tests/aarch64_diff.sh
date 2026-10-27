#!/usr/bin/env bash
# Differential test of the AArch64 emulator against qemu-aarch64: random instructions with random register state are assembled into one program, run under both,
# and every register, NZCV, FPSR and a scratch buffer are compared. Not part of run_host_tests.sh because it needs clang (aarch64 target), an AArch64-capable lld
# (the one in rustup's toolchain works) and qemu-aarch64.
# Usage: tests/aarch64_diff.sh [mode...] [--seeds N]   modes: int fp neon mem (default: all)
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-a64diff
mkdir -p "$work"
lld=${LD_LLD:-$(find "$HOME/.rustup" -name ld.lld -path '*gcc-ld*' 2>/dev/null | head -1)}
[ -n "$lld" ] || lld=$(command -v ld.lld) || { echo "no AArch64-capable ld.lld found (set LD_LLD)"; exit 2; }
modes=(); seeds=3
while [ $# -gt 0 ]; do case $1 in --seeds) seeds=$2; shift 2 ;; *) modes+=("$1"); shift ;; esac; done
[ ${#modes[@]} -gt 0 ] || modes=(int fp neon mem)
g++ -std=c++20 -O1 -I src -w tests/EmulationRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/Kernel/Sync.cpp tests/KernelStubs.cpp -pthread -o "$work/emurun"
fail=0
for mode in "${modes[@]}"; do
  for seed in $(seq 1 "$seeds"); do
    python3 tests/aarch64_diff/gen.py "$mode" "$seed" "$work"
    clang --target=aarch64-linux-gnu -march=armv8.2-a+fp16 -c "$work/t.s" -o "$work/t.o"
    "$lld" -static -nostdlib -e _start "$work/t.o" -o "$work/t.elf"
    qemu-aarch64 "$work/t.elf" > "$work/q.bin"
    "$work/emurun" "$work/t.elf" > "$work/e.raw" || true
    echo "== $mode seed $seed"
    python3 tests/aarch64_diff/compare.py "$work" || fail=1
  done
done
exit $fail
