#!/usr/bin/env bash
# The kernel's Wayland compositor reached by a real Linux program through the OS's own system call layer: tests/linux/waylandtest.c (no C library, for
# x86-64, AArch64 and RISC-V) finds the compositor through XDG_RUNTIME_DIR/WAYLAND_DISPLAY, keeps its picture in a memfd it maps and passes with
# SCM_RIGHTS, maps the keymap it is sent, and grows its pool; it runs on the kernel's CPU emulator (tests/EmulationRun.cpp built with -DTCOS_EMU_WAYLAND:
# the compositor, its AF_UNIX transport and a stand-in desktop). Each must pass its checks, and its window must have reached the desktop framed, with the
# picture it drew last read in place from its memory.
# Usage: tests/wayland_emulated.sh [x86_64|aarch64|riscv64 ...]   (default: all three). Needs clang + rust's ld.lld.
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-waylandemu
mkdir -p "$work/obj"
sources=(tests/EmulationRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Crypto/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp
         src/Kernel/Sync.cpp tests/KernelStubs.cpp src/Wayland/*.cpp src/Math/FloatMath.cpp src/Graphics/Graphics2D/Software/SoftwareRenderer2D.cpp
         src/Hardware/GraphicsProcessingUnit/Font8x16.cpp)
flags="-std=c++20 -O1 -pthread -I src -w -DTCOS_EMU_WAYLAND"
objs=()
jobs=()
for f in "${sources[@]}"; do
  o="$work/obj/$(echo "$f" | tr '/' '_').o"
  objs+=("$o")
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$(find src -name '*.hpp' -newer "$o" -print -quit)" ]; then jobs+=("$f"); fi
done
if [ ${#jobs[@]} -gt 0 ]; then
  printf '%s\n' "${jobs[@]}" | xargs -P "$(nproc)" -I{} sh -c 'o="'"$work"'/obj/$(echo "{}" | tr / _).o"; g++ '"$flags"' -c "{}" -o "$o" || { rm -f "$o"; exit 255; }'
fi
g++ -pthread "${objs[@]}" -o "$work/emurun"

targets=("$@")
[ ${#targets[@]} -gt 0 ] || targets=(x86_64 aarch64 riscv64)
lld=$(ls "$HOME"/.rustup/toolchains/*/lib/rustlib/*/bin/gcc-ld/ld.lld 2>/dev/null | head -1 || true)
fail=0
for t in "${targets[@]}"; do
  echo "== wayland client: $t"
  case $t in
    aarch64) extra=(-mno-outline-atomics) ;;
    riscv64) extra=(-march=rv64gc -mno-relax) ;;
    *) extra=() ;;
  esac
  linker=(-fuse-ld=lld)
  [ -n "$lld" ] && linker=("--ld-path=$lld")
  clang --target="$t-linux-gnu" "${extra[@]}" -O1 -static -nostdlib -ffreestanding -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables -fno-builtin \
    "${linker[@]}" -Wall -Wextra -o "$work/waylandtest-$t" tests/linux/waylandtest.c
  "$work/emurun" "$work/waylandtest-$t" | tee "$work/$t.out"
  out="$work/$t.out"
  grep -q 'framed window "Linux Wayland" pixel ffaa5500' "$out" || { echo "the window did not reach the desktop with its last picture"; fail=1; }
  grep -q ", 0 failed" "$out" || fail=1
  grep -q "faulted=0" "$out" || fail=1
done
[ $fail -eq 0 ] && echo "wayland emulated: all passed" || echo "wayland emulated: FAILED"
exit $fail
