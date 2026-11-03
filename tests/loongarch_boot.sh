#!/usr/bin/env bash
# Runs the LoongArch exception, timer, IPI and second-core code on QEMU's "virt" machine with no firmware: tests/loongarch_boot/harness.cpp links the
# image's real Trap.S, Interrupts.cpp, Timer.cpp, Cpu.cpp and Secondary.S, QEMU loads the ELF at its physical address (-kernel) and starts the first core
# in direct address translation, the state UEFI hands over in. Needs clang, rust's ld.lld and qemu-system-loongarch64; skipped when one is missing.
set -uo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-loongarch-boot; mkdir -p "$work"
lld=$(ls "$HOME"/.rustup/toolchains/*/lib/rustlib/*/bin/gcc-ld/ld.lld 2>/dev/null | head -1)
if ! command -v qemu-system-loongarch64 >/dev/null || ! command -v clang >/dev/null || [ -z "${lld:-}" ]; then echo "skipped (needs qemu-system-loongarch64, clang and rust's ld.lld)"; exit 0; fi
mingw=$(ls -d /usr/x86_64-w64-mingw32/include/c++/* 2>/dev/null | tail -1)   # (libstdc++'s freestanding headers, as cmake/toolchain-loongarch64-uefi.cmake)
flags="--target=loongarch64-unknown-elf -march=loongarch64 -mabi=lp64s -mfpu=none -mstrict-align -std=c++20 -O1 -g -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -fno-threadsafe-statics -fshort-wchar -isystem $mingw -isystem $mingw/x86_64-w64-mingw32 -I src -I . -Wall -Wextra"
objs=()
for f in tests/loongarch_boot/harness.cpp src/Arch/LoongArch64/Interrupts.cpp src/Arch/LoongArch64/Timer.cpp src/Arch/LoongArch64/Cpu.cpp src/Arch/LoongArch64/Core.cpp src/Utility/Bounds.cpp src/runtime.cpp src/Hardware/Acpi/AcpiTables.cpp; do
  o="$work/$(basename "$f").o"; clang++ $flags -c "$f" -o "$o" || exit 1; objs+=("$o")
done
for f in tests/loongarch_boot/start.S src/Arch/LoongArch64/Trap.S src/Arch/LoongArch64/Secondary.S; do
  o="$work/$(basename "$f").o"; clang --target=loongarch64-unknown-elf -march=loongarch64 -mabi=lp64s -mfpu=none -c "$f" -o "$o" || exit 1; objs+=("$o")
done
"$lld" -static -nostdlib --image-base=0x200000 -z noseparate-code -z norelro --no-rosegment -e _start "${objs[@]}" -o "$work/harness.elf" || exit 1
( timeout 30 qemu-system-loongarch64 -machine virt -cpu la464 -smp 2 -m 256M -nographic -monitor none -serial file:"$work/serial.txt" -kernel "$work/harness.elf" -display none >/dev/null 2>&1 ) 2>/dev/null || true
cat "$work/serial.txt"
grep -q "^ALL PASSED" "$work/serial.txt"
