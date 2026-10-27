#!/usr/bin/env bash
# Runs a Windows test program on the CPU emulator, with this OS's own ntdll/kernel32/msvcrt (built from src/Windows), on real host threads. It needs the UEFI cross toolchain
# the project already uses (to build the DLLs) and mingw (x86_64-w64-mingw32-gcc) for the test program; it is not part of run_host_tests.sh for that reason.
# Usage: tests/emulated_windows.sh [program.c ...]   (default: emuthreads.c, sehtest.c, threadtest.c, crttest.c and cxxtest.cpp: threads, structured and C++ exceptions, and the C runtime
# including printf of floating point, which needs the x87 unit)
# sehtest.c and threadtest.c are built with clang (it has __try/__except in C) the way tests/windows/build.sh does; they need clang and the mingw headers.
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-emuwin
mkdir -p "$work/sys32"
if [ ! -f "$work/build/build.ninja" ]; then
  cmake -S . -B "$work/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-x86_64-uefi.cmake" -DCMAKE_BUILD_TYPE=Debug > /dev/null
fi
ninja -C "$work/build" windows_dlls > /dev/null
cp "$work"/build/windows/*.dll "$work/sys32/"
g++ -std=c++20 -O2 -pthread -I src -w tests/EmulationRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/Kernel/Sync.cpp tests/KernelStubs.cpp -o "$work/emurun"
programs=("$@")
[ ${#programs[@]} -gt 0 ] || programs=(tests/windows/emuthreads.c tests/windows/sehtest.c tests/windows/threadtest.c tests/windows/crttest.c tests/windows/cxxtest.cpp)
fail=0
for src in "${programs[@]}"; do
  name=$(basename "${src%.*}")
  case $name in
    sehtest|threadtest) clang --target=x86_64-w64-mingw32 -fms-extensions -O1 -Wno-unused-function -I/usr/x86_64-w64-mingw32/include -L/usr/x86_64-w64-mingw32/lib -fuse-ld=bfd -o "$work/$name.exe" "$src" ;;
    cxxtest) x86_64-w64-mingw32-g++ -O1 -Wall -Wextra -static -o "$work/$name.exe" "$src" ;;
    *) x86_64-w64-mingw32-gcc -O1 -Wall -Wextra -o "$work/$name.exe" "$src" ;;
  esac
  echo "== $name"
  "$work/emurun" "$work/$name.exe" "$work/sys32" | tee "$work/$name.out"
  grep -q ", 0 failed" "$work/$name.out" || fail=1
  grep -q "faulted=0" "$work/$name.out" || fail=1
done
exit $fail
