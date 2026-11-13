#!/usr/bin/env bash
# The kernel's X server reached by real Linux and Windows programs through the OS's own system call layers: tests/linux/x11test.c (Linux, no C library,
# for x86-64, AArch64 and RISC-V: socket(2)/connect(2) to /tmp/.X11-unix/X0 and to the abstract name) and tests/windows/x11wintest.c (Windows, ws2_32
# AF_UNIX to the abstract name) run on the kernel's CPU emulator (tests/EmulationRun.cpp built with -DTCOS_EMU_X11: the X server, its AF_UNIX transport
# and a stand-in desktop). Each must pass its checks and its windows must have reached the desktop as framed windows.
# Usage: tests/x11_emulated.sh [x86_64|aarch64|riscv64|windows ...]   (default: all four). Needs clang + rust's ld.lld (Linux) and mingw (Windows).
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-x11emu
mkdir -p "$work/obj" "$work/sys32"
sources=(tests/EmulationRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Crypto/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp
         src/Kernel/Sync.cpp tests/KernelStubs.cpp src/X11/*.cpp src/Math/FloatMath.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp
         src/Graphics/Graphics2D/Software/*.cpp)
flags="-std=c++20 -O1 -pthread -I src -w -DTCOS_EMU_X11"
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
[ ${#targets[@]} -gt 0 ] || targets=(x86_64 aarch64 riscv64 windows)
lld=$(ls "$HOME"/.rustup/toolchains/*/lib/rustlib/*/bin/gcc-ld/ld.lld 2>/dev/null | head -1 || true)
fail=0
for t in "${targets[@]}"; do
  echo "== x11 client: $t"
  if [ "$t" = windows ]; then
    if ! command -v x86_64-w64-mingw32-gcc >/dev/null; then echo "skipped (no mingw)"; continue; fi
    if [ ! -f "$work/build/build.ninja" ]; then
      cmake -S . -B "$work/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-x86_64-uefi.cmake" -DCMAKE_BUILD_TYPE=Debug > /dev/null
    fi
    ninja -C "$work/build" windows_dlls > /dev/null
    cp "$work"/build/windows/*.dll "$work/sys32/"
    x86_64-w64-mingw32-gcc -O1 -Wall -Wextra -o "$work/x11wintest.exe" tests/windows/x11wintest.c -lws2_32
    "$work/emurun" "$work/x11wintest.exe" "$work/sys32" | tee "$work/windows.out"
    out="$work/windows.out"
    grep -q 'framed window "Windows X11 client"' "$out" || { echo "the window did not reach the desktop"; fail=1; }
  else
    case $t in
      aarch64) extra=(-mno-outline-atomics) ;;
      riscv64) extra=(-march=rv64gc -mno-relax) ;;
      *) extra=() ;;
    esac
    linker=(-fuse-ld=lld)
    [ -n "$lld" ] && linker=("--ld-path=$lld")
    clang --target="$t-linux-gnu" "${extra[@]}" -O1 -static -nostdlib -ffreestanding -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables "${linker[@]}" \
      -Wall -Wextra -o "$work/x11test-$t" tests/linux/x11test.c
    "$work/emurun" "$work/x11test-$t" | tee "$work/$t.out"
    out="$work/$t.out"
    grep -q 'framed window "Linux X11 (path)"' "$out" && grep -q 'framed window "Linux X11 (abstract)"' "$out" || { echo "the windows did not reach the desktop"; fail=1; }
  fi
  grep -q ", 0 failed" "$out" || fail=1
  grep -q "faulted=0" "$out" || fail=1
done
[ $fail -eq 0 ] && echo "x11 emulated: all passed" || echo "x11 emulated: FAILED"
exit $fail
