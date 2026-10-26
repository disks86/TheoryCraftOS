#!/usr/bin/env bash
# Builds the Windows test programs with the mingw compiler of the OS's toolchain (they run on TheoryCraftOS's kernel32.dll, ntdll.dll and msvcrt.dll; see src/Windows).
#   k32test.exe: freestanding, imports kernel32 only
#   crttest.exe: an ordinary C program with the toolchain's C runtime (the Universal CRT with this mingw; the OS maps its api-ms-win-crt-* imports to msvcrt.dll)
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
