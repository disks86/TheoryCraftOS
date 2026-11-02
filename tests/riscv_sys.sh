#!/usr/bin/env bash
# Differential test of the RISC-V privileged architecture: tests/riscv_sys/sys.S runs in machine mode on qemu-system-riscv64 (-M virt -bios none) and on RiscV64Machine, and the
# text each prints through the UART has to match. Needs clang (riscv64 target), a RISC-V-capable lld (rustup's works) and qemu-system-riscv64.
# Usage: tests/riscv_sys.sh [payload ...]   (default: every tests/riscv_sys/*.S)
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-rvsys
mkdir -p "$work"
lld=${LD_LLD:-$(find "$HOME/.rustup" -name ld.lld -path '*gcc-ld*' 2>/dev/null | head -1)}
[ -n "$lld" ] || lld=$(command -v ld.lld) || { echo "no RISC-V-capable ld.lld found (set LD_LLD)"; exit 2; }
g++ -std=c++20 -O1 -I src -w tests/RiscV64SystemRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/Kernel/Sync.cpp tests/KernelStubs.cpp -pthread -o "$work/sysrun"
payloads=("$@"); [ ${#payloads[@]} -gt 0 ] || payloads=(tests/riscv_sys/*.S)
fail=0
for src in "${payloads[@]}"; do
  name=$(basename "$src" .S)
  clang -I tests/riscv_sys --target=riscv64-unknown-elf -march=rv64gcv_zba_zbb_zbs_zicsr_zifencei -mabi=lp64d -nostdlib -c "$src" -o "$work/$name.o"
  "$lld" -static -nostdlib -Ttext=0x80000000 -e _start "$work/$name.o" -o "$work/$name.elf"
  llvm-objcopy -O binary "$work/$name.elf" "$work/$name.bin"
  timeout 60 qemu-system-riscv64 -M virt -cpu ${QEMU_CPU:-rv64,h=false,v=true,vlen=128,sstc=true,svadu=false,svade=true,sscofpmf=false,pmu-num=0} -m 64M -bios none -nographic -monitor none -serial file:"$work/$name.q.txt" -kernel "$work/$name.elf" -display none || true
  "$work/sysrun" "$work/$name.bin" > "$work/$name.e.txt" || true
  if diff -q "$work/$name.q.txt" "$work/$name.e.txt" > /dev/null; then echo "== $name: identical ($(wc -l < "$work/$name.q.txt") lines)"; else echo "== $name: DIFFERENT"; diff "$work/$name.q.txt" "$work/$name.e.txt" | head -${DIFF_LINES:-30}; fail=1; fi
done
exit $fail
