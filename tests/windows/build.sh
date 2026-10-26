#!/usr/bin/env bash
# Builds the Windows test programs with the mingw compiler of the OS's toolchain (they run on TheoryCraftOS's kernel32.dll, ntdll.dll and msvcrt.dll; see src/Windows).
#   k32test.exe: freestanding, imports kernel32 only
#   crttest.exe: an ordinary C program with the toolchain's C runtime (the Universal CRT with this mingw; the OS maps its api-ms-win-crt-* imports to msvcrt.dll)
#   ipctest.exe: run twice at the same time ("server"/"client", cmake -DTCOS_USERMODE_DEMO_PAIR=ON): named events, mutexes and semaphores between processes
#   tlscbtest.exe: TLS callbacks for the process and its threads
#   threadtest.exe: CreateThread and friends, critical sections, events, mutexes, TLS (clang)
#   sehtest.exe: __try/__except/__finally and RaiseException (clang, which has structured exception handling in C)
# Usage: tests/windows/build.sh [output-dir]   (default: build/windows-tests)
set -euo pipefail
cd "$(dirname "$0")/../.."
out=${1:-build/windows-tests}
mkdir -p "$out"
cc=${CC:-x86_64-w64-mingw32-gcc}
$cc -nostdlib -ffreestanding -fno-builtin -O1 -Wall -Wextra -Wno-unused-parameter -Wl,-e,start -Wl,--subsystem,console -o "$out/k32test.exe" tests/windows/k32test.c -lkernel32
echo "built $out/k32test.exe"
$cc -O1 -Wall -Wextra -o "$out/crttest.exe" tests/windows/crttest.c
echo "built $out/crttest.exe"
$cc -O1 -Wall -Wextra -o "$out/ipctest.exe" tests/windows/ipctest.c
echo "built $out/ipctest.exe (two processes sharing named events, mutexes, semaphores)"
$cc -O1 -Wall -Wextra -o "$out/tlscbtest.exe" tests/windows/tlscbtest.c
echo "built $out/tlscbtest.exe (TLS callbacks)"
$cc -O1 -Wall -Wextra -o "$out/procchild.exe" tests/windows/procchild.c
$cc -O1 -Wall -Wextra -o "$out/processtest.exe" tests/windows/processtest.c
echo "built $out/processtest.exe and procchild.exe (CreateProcess; put procchild.exe in System32: -DTCOS_USERMODE_DEMO_FILES=...)"
$cc -O0 -Wall -Wextra -o "$out/suspendtest.exe" tests/windows/suspendtest.c
echo "built $out/suspendtest.exe (SuspendThread, thread contexts, TerminateThread)"
if command -v clang >/dev/null; then
  clang --target=x86_64-w64-mingw32 -fms-extensions -O1 -Wall -I/usr/x86_64-w64-mingw32/include -L/usr/x86_64-w64-mingw32/lib -fuse-ld=bfd -o "$out/sehtest.exe" tests/windows/sehtest.c
  echo "built $out/sehtest.exe (SEH: __try/__except/__finally, clang)"
  clang --target=x86_64-w64-mingw32 -fms-extensions -O1 -Wall -Wno-unused-function -I/usr/x86_64-w64-mingw32/include -L/usr/x86_64-w64-mingw32/lib -fuse-ld=bfd -o "$out/threadtest.exe" tests/windows/threadtest.c
  clang --target=x86_64-w64-mingw32 -fms-extensions -O1 -I/usr/x86_64-w64-mingw32/include -L/usr/x86_64-w64-mingw32/lib -fuse-ld=bfd -shared -o "$out/tlsdll.dll" tests/windows/tlsdll.c -Wl,--out-implib,"$out/libtlsdll.a"
  clang --target=x86_64-w64-mingw32 -fms-extensions -O1 -I/usr/x86_64-w64-mingw32/include -L/usr/x86_64-w64-mingw32/lib -fuse-ld=bfd -o "$out/tlsdlltest.exe" tests/windows/tlsdlltest.c "$out/libtlsdll.a"
  echo "built $out/tlsdlltest.exe and tlsdll.dll (a DLL's static TLS; put tlsdll.dll next to it: -DTCOS_USERMODE_DEMO_FILES=...)"
  echo "built $out/threadtest.exe (threads, clang for __declspec(thread))"
  clang --target=x86_64-w64-mingw32 -fms-extensions -O1 -Wall -I/usr/x86_64-w64-mingw32/include -L/usr/x86_64-w64-mingw32/lib -fuse-ld=bfd -o "$out/smptest.exe" tests/windows/smptest.c
  echo "built $out/smptest.exe (threads on different cores, page tables shared between cores)"
fi
${CXX:-x86_64-w64-mingw32-g++} -std=c++17 -O1 -Wall -static -static-libgcc -static-libstdc++ -Wno-unused-result -o "$out/cxxtest.exe" tests/windows/cxxtest.cpp 2>/dev/null && echo "built $out/cxxtest.exe (C++ exceptions, libstdc++ and libgcc linked statically)" || echo "(no C++ build: $(basename "${CXX:-x86_64-w64-mingw32-g++}") failed)"
