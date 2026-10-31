#!/usr/bin/env bash
# Differential test of the RISC-V emulator against qemu-riscv64: random instructions with random register state are assembled into one program, run under both, and every
# register, fcsr and a scratch buffer are compared. Not part of run_host_tests.sh because it needs clang (riscv64 target), a RISC-V-capable lld (the one in rustup's toolchain
# works) and qemu-riscv64.
# Usage: tests/riscv_diff.sh [mode...] [--seeds N]   modes: int mem fp comp bit half zc amo2 crypto vbit vint vmem vfp (default: all)
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-rvdiff
mkdir -p "$work"
lld=${LD_LLD:-$(find "$HOME/.rustup" -name ld.lld -path '*gcc-ld*' 2>/dev/null | head -1)}
[ -n "$lld" ] || lld=$(command -v ld.lld) || { echo "no RISC-V-capable ld.lld found (set LD_LLD)"; exit 2; }
modes=(); seeds=3
while [ $# -gt 0 ]; do case $1 in --seeds) seeds=$2; shift 2 ;; *) modes+=("$1"); shift ;; esac; done
[ ${#modes[@]} -gt 0 ] || modes=(int mem fp comp bit)
g++ -std=c++20 -O1 -I src -w tests/EmulationRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/Kernel/Sync.cpp tests/KernelStubs.cpp -pthread -o "$work/emurun"
fail=0
for mode in "${modes[@]}"; do
  for seed in $(seq 1 "$seeds"); do
    python3 tests/riscv_diff/gen.py "$mode" "$seed" "$work"
    march=rv64gc; cpu=rv64; case $mode in vhalf) march=rv64gcv_zfh_zvfh_zfbfmin_zvfbfmin_zvfbfwma; cpu=max,vlen=128,rvv_ta_all_1s=true,rvv_ma_all_1s=true ;; vbit) march=rv64gcv_zba_zbb_zvbb_zvbc_zvkb; cpu=max,vlen=128,rvv_ta_all_1s=true,rvv_ma_all_1s=true ;; zc) march=rv64imafd_zca_zba_zbb_zcb_zcmop_zimop_zawrs_zihintntl_zicbop; cpu=max ;; amo2) march=rv64gc_zabha_zacas; cpu=max ;; crypto) march=rv64gc_zbb_zbkb_zbkx_zknd_zkne_zknh_zksed_zksh; cpu=max ;; half) march=rv64gc_zfh_zfa_zfbfmin; cpu=max ;; bit) march=rv64gc_zba_zbb_zbs_zbc_zicond; cpu=max ;; v*) march=rv64gcv_zba_zbb; cpu=max,vlen=128,rvv_ta_all_1s=true,rvv_ma_all_1s=true ;; esac
    clang --target=riscv64-linux-gnu -march=$march -mabi=lp64d -c "$work/t.s" -o "$work/t.o"
    "$lld" -static -nostdlib -e _start "$work/t.o" -o "$work/t.elf"
    qemu-riscv64 -cpu $cpu "$work/t.elf" > "$work/q.bin"
    "$work/emurun" "$work/t.elf" > "$work/e.raw" || true
    echo "== $mode seed $seed"
    python3 tests/riscv_diff/compare.py "$work" || fail=1
  done
done
exit $fail
