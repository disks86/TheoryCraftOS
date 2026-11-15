#!/usr/bin/env bash
# Builds the libraries of 32-bit Windows programs - what C:\Windows\SysWOW64 holds - from the same sources as the 64-bit ones (src/Windows), for i386:
# ntdll.dll, kernel32.dll and msvcrt.dll so far (docs/Wow64.md). Called by the build (CMakeLists.txt) when i686-w64-mingw32-gcc is there.
#
# The functions of the Windows interfaces are stdcall on i386, so the compiler names them "_Name@bytes". A library exports them by their plain names
# (--kill-at); the libraries that import them are linked against an import library made from the decorated names (dlltool -k), which is why each
# library that others import from is linked twice: once to learn its decorated exports, once for real.
# Usage: scripts/build-wow64-dlls.sh <output folder>
set -euo pipefail
cd "$(dirname "$0")/.."
out=$1
cc=${WOW64_CC:-i686-w64-mingw32-gcc}
dlltool=${WOW64_DLLTOOL:-i686-w64-mingw32-dlltool}
mkdir -p "$out"
# (SSE2 for the floating point of the C runtime; a function may be entered with the stack aligned to 4 bytes only, as 32-bit Windows code leaves it)
flags="-O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-value -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-overflow -msse2 -mincoming-stack-boundary=2 -ffreestanding -fno-builtin
       -fno-tree-loop-distribute-patterns -fno-stack-protector -fno-strict-aliasing -nostdlib -shared -Wl,--entry=_DllMain@12"
w=src/Windows
ntdll="$w/ntdll/ntdll.c $w/ntdll/security.c $w/ntdll/token.c"

$cc $flags -w -Wl,--image-base=0x70000000 -Wl,--output-def="$out/ntdll.def" -o "$out/ntdll_decorated.dll" $ntdll -lgcc
$dlltool -k -d "$out/ntdll.def" -D ntdll.dll -l "$out/libntdll.a"
$cc $flags -Wl,--kill-at -Wl,--image-base=0x70000000 -o "$out/ntdll.dll" $ntdll -lgcc

$cc $flags -w -Wl,--image-base=0x71000000 -Wl,--output-def="$out/kernel32.def" -o "$out/kernel32_decorated.dll" $w/kernel32/kernel32.c "$out/libntdll.a" -lgcc
$dlltool -k -d "$out/kernel32.def" -D kernel32.dll -l "$out/libkernel32.a"
$cc $flags -Wl,--kill-at -Wl,--image-base=0x71000000 -o "$out/kernel32.dll" $w/kernel32/kernel32.c $w/kernel32/kernel32.def "$out/libntdll.a" -lgcc

$cc $flags -Wl,--kill-at -Wl,--image-base=0x72000000 -o "$out/msvcrt.dll" $w/msvcrt/msvcrt.c $w/msvcrt/msvcrt.def "$out/libntdll.a" "$out/libkernel32.a" -lgcc
rm -f "$out/ntdll_decorated.dll" "$out/kernel32_decorated.dll"
