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
flags="$flags ${WOW64_EXTRA_FLAGS:-}"      # (more compiler flags, for a build that looks for something: -DTCOS_HEAP_CHECK, see src/Windows/ntdll/heap.inc)
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
    *)
      # Any other library: its own code if it has any (src/Windows/<dll>/<dll>.c), over ntdll, kernel32 and the libraries it imports; without code of
      # its own it is its stubs. The image base is only a wish (the loader moves a library whose place is taken).
      [ -f "$w/stubs/$1.inc" ] || [ -f "$w/$1/$1.c" ] || { echo "build-wow64-dlls.sh: no library $1" >&2; exit 1; }
      local imports="ntdll kernel32"
      case $1 in
        dxgi) imports="$imports gdi32 user32 win32u" ;;      # (win32u: the Direct3D services with the kernel's layout, src/Windows/dxcommon.h)
        opengl32|vulkan-1) imports="$imports gdi32 user32" ;;
        d3d12) imports="$imports gdi32 win32u" ;;
        d3d11) imports="$imports gdi32 win32u d3d12 dxgi" ;;
        d3d9) imports="$imports gdi32 user32 win32u d3d12" ;;      # (Direct3D 9 is a layer on d3d12.dll; gdi32 and user32 to put the picture into the window)      # (Direct3D 11 is a layer on d3d12.dll; dxgi for the adapter and the swap chain)
        imm32) imports="$imports win32u" ;;
        crypt32) imports="$imports advapi32 bcrypt" ;;
        secur32) imports="$imports advapi32" ;;
        mmdevapi|xaudio2_9|xaudio2_8) imports="$imports ole32" ;;
        comdlg32) imports="$imports user32" ;;
      esac
      if [ -f "$w/$1/$1.c" ]; then build "$1" 0x6a000000 0 "$w/$1/$1.c" $(lib $imports)
      else build "$1" 0x6a000000 0
      fi ;;
  esac
}

# The first fourteen in the order they need each other; the rest need only those (and bcrypt before crypt32). Not built for i386: vulkan-1, whose
# source does not compile for it yet (a 32-bit program's Vulkan handles are 64-bit numbers, not pointers; docs/Wow64.md).
LIBRARIES="ntdll kernel32 msvcrt advapi32 win32u gdi32 user32 comctl32 version oleaut32 ole32 shlwapi shcore shell32
           rpcrt4 secur32 bcrypt crypt32 ncrypt userenv setupapi iphlpapi psapi wininet winhttp wintrust netapi32 dwmapi uxtheme mswsock gdiplus msimg32 opengl32 glu32 dnsapi mpr wtsapi32 powrprof cfgmgr32 dxgi d3d12 d3d9 d3d11 dwrite avrt winspool wevtapi pdh winmm dsound mmdevapi xaudio2_8 xaudio2_9 imm32 dbghelp wldap32 urlmon msi propsys hid usp10 winscard odbc32 xinput1_4 dinput8 d2d1 d3dcompiler_47 lz32 oleacc oledlg credui ntdsapi samlib activeds normaliz sensapi authz rasapi32 bcryptprimitives combase mfplat mf mfreadwrite dcomp uiautomationcore dhcpcsvc esent ndfapi tbs winusb cryptui bthprops comdlg32 ws2_32 shfolder"
# The libraries others are linked against are built one after the other; the rest, which nothing here imports from, side by side.
ordered=" ntdll kernel32 msvcrt advapi32 win32u gdi32 user32 comctl32 version oleaut32 ole32 shlwapi shcore shell32 bcrypt dxgi d3d12 "      # (dxgi, d3d12: d3d11 is linked against them)
pids=()
for name in ${*:-$LIBRARIES}; do
  case "$ordered" in
    *" $name "*) build_one "$name" ;;
    *) build_one "$name" & pids+=($!) ;;
  esac
done
failed=0
for pid in "${pids[@]}"; do wait "$pid" || failed=1; done
exit $failed
