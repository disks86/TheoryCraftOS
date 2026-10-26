#!/usr/bin/env bash
# Checks the QPU programs of the coloured-vertex pipeline against Mesa's own packer: every instruction word is decoded with v3d_qpu_instr_unpack, printed with
# v3d_qpu_decode, and packed again with v3d_qpu_instr_pack; "repack=ok" without "(differs)" means Mesa reads the instruction as intended and writes the same bits.
# Mesa's sources (MIT licensed) are fetched from sources.debian.org into a temporary directory and are not kept in the repository.
# Usage: scripts/qpu_oracle/verify.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
version=25.0.7-2+deb13u1
base=https://sources.debian.org/data/main/m/mesa/$version/src
mkdir -p "$work/util" "$work/broadcom/common"
for f in qpu_pack.c qpu_instr.c qpu_instr.h qpu_disasm.c qpu_disasm.h; do curl -sf "$base/broadcom/qpu/$f" -o "$work/$f"; done
for f in macros.h bitscan.h detect_os.h detect_arch.h detect_cc.h compiler.h u_math.h; do curl -sf "$base/util/$f" -o "$work/util/$f"; done
curl -sf "$base/broadcom/common/v3d_device_info.h" -o "$work/broadcom/common/v3d_device_info.h"
cp scripts/qpu_oracle/ralloc.h "$work/util/ralloc.h"
gcc -std=gnu11 -I"$work" -I"$work/util" -w "$work"/qpu_pack.c "$work"/qpu_instr.c "$work"/qpu_disasm.c scripts/qpu_oracle/oracle.c -o "$work/oracle"
g++ -std=c++20 -I src scripts/qpu_oracle/dump_shaders.cpp src/Hardware/GraphicsProcessingUnit/Broadcom/V3dShaders.cpp src/Hardware/GraphicsProcessingUnit/Broadcom/V3dQpu.cpp tests/KernelStubs.cpp -o "$work/dump"
"$work/dump" | grep -v '^#' | "$work/oracle" | tee "$work/out.txt"
if grep -q "FAIL\|differs" "$work/out.txt"; then echo "MISMATCH"; exit 1; fi
echo "all instructions decode and repack identically"
