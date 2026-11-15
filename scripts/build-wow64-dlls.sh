#!/usr/bin/env bash
# Builds the libraries of 32-bit Windows programs - what C:\Windows\SysWOW64 holds - from the same sources as the 64-bit ones (src/Windows), for i386
# (docs/Wow64.md). Called by the build (CMakeLists.txt) when i686-w64-mingw32-gcc is there. The list of libraries is LIBRARIES below; CMakeLists.txt
# has the same list (WOW64_LIBRARIES) for what goes on the disk image.
#
# The functions of the Windows interfaces are stdcall on i386, so the compiler names them "_Name@bytes". A library exports them by their plain names
# (--kill-at); the libraries that import them are linked against an import library made from the decorated names (dlltool -k), which is why each
# library is linked twice: once to learn its decorated exports, once for real. The exports that are not implemented yet are stubs that take the right
# number of bytes off the stack (scripts/gen-wow64-stubs.py).
# Usage: scripts/build-wow64-dlls.sh <output folder> [library ...]      (no libraries: all of them, in order)
set -euo pipefail
cd "$(dirname "$0")/.."
out=$1
shift
cc=${WOW64_CC:-i686-w64-mingw32-gcc}
dlltool=${WOW64_DLLTOOL:-i686-w64-mingw32-dlltool}
nm=${WOW64_NM:-i686-w64-mingw32-nm}
mingw_lib=$(dirname "$($cc -print-file-name=libkernel32.a)")
mkdir -p "$out"
# (SSE2 for the floating point of the C runtime; a function may be entered with the stack aligned to 4 bytes only, as 32-bit Windows code leaves it)
flags="-O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-value -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-overflow -Wno-builtin-declaration-mismatch
       -msse2 -mincoming-stack-boundary=2 -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-stack-protector -fno-strict-aliasing -nostdlib -shared
       -include src/Windows/tcos_wow64_prefix.h -Wl,--entry=_DllMain@12 -Wl,--enable-stdcall-fixup"
w=src/Windows

# build <name> <image base> <stub result> <sources and the import libraries it is linked against ...>
# The library's .def (forwarders, ordinals) is added if it has one; stubs.c gives a library without an entry point one that does nothing.
build() {
  local name=$1 base=$2 result=$3
  shift 3
  local inputs="$*" def=""
  local lists="$w/stubs/$name.inc"
  [ -f "$lists" ] || { lists="$out/${name}_none.inc"; : > "$lists"; }      # (a library with no list of stubs)
  [ "$name" = ntdll ] && lists="$lists+$w/ntdll/wow64_unthunked.inc"      # (the services without a function of their own: see scripts/gen-wow64-thunks.py)
  python3 scripts/gen-wow64-stubs.py "$name" "$lists" "$result" "$nm" "$mingw_lib" "$out/${name}_stubs.S" "$w/$name/${name}32.implemented"
  $cc $flags -w -DSTUB_DLLMAIN -Wl,--image-base="$base" -Wl,--output-def="$out/$name.def" -o "$out/${name}_decorated.dll" $inputs "$out/${name}_stubs.S" $w/stubs/stubs.c -lgcc
  $dlltool -k -d "$out/$name.def" -D "$name.dll" -l "$out/lib$name.a"
  if [ -f "$w/$name/$name.def" ]; then      # (its forwarders and ordinals, with the names as this build has them)
    python3 scripts/gen-wow64-def.py "$w/$name/$name.def" "$out/$name.def" "$out/${name}_final.def"
    def="$out/${name}_final.def"
    # (exports only the 32-bit library has: <dll>32.def, a list of lines for the EXPORTS section)
    if [ -f "$w/$name/${name}32.def" ]; then grep -v '^;' "$w/$name/${name}32.def" >> "$out/${name}_final.def"; fi
  fi
  $cc $flags -DSTUB_DLLMAIN -Wl,--kill-at -Wl,--image-base="$base" -o "$out/$name.dll" $inputs $def "$out/${name}_stubs.S" $w/stubs/stubs.c -lgcc
  rm -f "$out/${name}_decorated.dll"
}
lib() { for name in "$@"; do printf '%s ' "$out/lib$name.a"; done; }

build_one() {
  case $1 in
    ntdll) build ntdll 0x70000000 0xC0000002 $w/ntdll/ntdll.c $w/ntdll/security.c $w/ntdll/token.c ;;
    kernel32) build kernel32 0x71000000 0 $w/kernel32/kernel32.c $(lib ntdll) ;;
    msvcrt) build msvcrt 0x72000000 0 $w/msvcrt/msvcrt.c $(lib ntdll kernel32) ;;
    advapi32) build advapi32 0x72800000 0 $(printf "$w/advapi32/%s.c " advapi32 security kbsec lsa scm eventlog etw secext crypt cred policy lsa_ns perf misc) $(lib ntdll kernel32) ;;
    win32u) build win32u 0x73000000 0 $w/win32u/win32u.c ;;      # (no imports: its services are INT 2E)
    gdi32) build gdi32 0x73800000 0 $w/gdi32/gdi32.c $(lib ntdll kernel32) ;;
    user32) build user32 0x74000000 0 $w/user32/user32.c $(lib ntdll kernel32 win32u gdi32) ;;
    comctl32) build comctl32 0x74800000 0 $w/comctl32/comctl32.c $(lib ntdll kernel32) ;;
    version) build version 0x74900000 0 $w/version/version.c $(lib ntdll kernel32) ;;
    oleaut32) build oleaut32 0x75000000 0 $w/oleaut32/oleaut32.c $(lib ntdll kernel32 advapi32) ;;
    ole32) build ole32 0x75800000 0 $w/ole32/ole32.c $(lib ntdll kernel32 advapi32) ;;
    shlwapi) build shlwapi 0x76000000 0 $w/shlwapi/shlwapi.c $(lib ntdll kernel32 advapi32 oleaut32 user32) ;;
    shcore) build shcore 0x76400000 0 $w/shcore/shcore.c $(lib ntdll kernel32 advapi32 ole32 win32u) ;;
    shell32) build shell32 0x76800000 0 $w/shell32/shell32.c $(lib ntdll kernel32 advapi32 user32 ole32) ;;
    *) echo "build-wow64-dlls.sh: no rule for $1" >&2; exit 1 ;;
  esac
}

LIBRARIES="ntdll kernel32 msvcrt advapi32 win32u gdi32 user32 comctl32 version oleaut32 ole32 shlwapi shcore shell32"
for name in ${*:-$LIBRARIES}; do build_one "$name"; done
