#!/usr/bin/env bash
# Builds the Windows test programs with the mingw compiler of the OS's toolchain (they run on TheoryCraftOS's kernel32.dll and ntdll.dll; see src/Windows).
# Usage: tests/windows/build.sh [output-dir]   (default: build/windows-tests)
set -euo pipefail
cd "$(dirname "$0")/../.."
out=${1:-build/windows-tests}
mkdir -p "$out"
cc=${CC:-x86_64-w64-mingw32-gcc}
$cc -nostdlib -ffreestanding -fno-builtin -O1 -Wall -Wextra -Wno-unused-parameter -Wl,-e,start -Wl,--subsystem,console -o "$out/k32test.exe" tests/windows/k32test.c -lkernel32
echo "built $out/k32test.exe"
