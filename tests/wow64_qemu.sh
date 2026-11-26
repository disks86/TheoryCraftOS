#!/usr/bin/env bash
# 32-bit Windows programs on the real kernel under QEMU (docs/Wow64.md). Fourteen programs, each started by the kernel's autorun in a boot of its own:
#   hello32  tests/windows/hello32.c: PE32 for i386 with no libraries. It runs in the CPU's compatibility mode and calls the NT services through INT 2E.
#   crt32    tests/windows/crt32.c: built the ordinary way, with a C runtime; it runs on the 32-bit ntdll.dll, kernel32.dll and msvcrt.dll of
#            C:\Windows\SysWOW64 (the heap, files, the environment, thread-local storage, threads, a library loaded while it runs).
#   seh32    tests/windows/seh32.c: exceptions - faults delivered with an i386 CONTEXT, the handlers at FS:[0], RtlUnwind, _except_handler3.
#   gui32    tests/windows/gui32.c: the other libraries - the registry, the shell's folders, a shortcut through COM, and a window with its messages.
#   lib32    tests/windows/lib32.c: the libraries beyond those - Winsock, the common dialogs' structures, the multimedia timer and wave output, the
#            input method manager, a walk of its own stack with dbghelp, bcrypt.
#   old32    tests/windows/old32.c: a program built against the old C runtime (msvcrt.dll), which finds stdout and stderr by the size of a stream.
#   dx32     tests/windows/dx32.c: Direct3D - gdi32's D3DKMT functions with a 32-bit program's structures, and all of tests/windows/d3d12test.c
#            (dxgi.dll and d3d12.dll over the kernel's software adapter).
#   d3d11test  tests/windows/d3d11test.c: Direct3D 11 (d3d11.dll, a layer on d3d12.dll) - a device, buffers, draws read back, constants a stage,
#            textures, depth, blending, a swap chain on a window. The same source built for x86-64 tests the 64-bit library.
#   d3d10test  tests/windows/d3d10test.c: Direct3D 10 (d3d10.dll, d3d10_1.dll, d3d10core.dll over d3d11.dll's device) - draws read back, states, a geometry
#              shader and stream output, queries, Map, Direct3D 10 <-> 11 interfaces, state blocks, a swap chain and a triangle drawn through an effect
#   d3d9test  tests/windows/d3d9test.c: Direct3D 9 (d3d9.dll: the fixed-function pipeline and a program's own shaders, a layer on d3d12.dll) - a device on a window, draws read
#            back, buffers, textures and their stages, blending, depth, a texture drawn into, Present, Reset. Also built for x86-64.
#   d3d8test  tests/windows/d3d8test.c: Direct3D 8 (d3d8.dll, a layer on d3d9.dll's interfaces) - Direct3D 8's references and identities, draws read back
#            (FVF vertices, buffers with SetIndices' base vertex index, a vertex shader of D3DVSD tokens, pixel shaders 1.0 and 1.1), state blocks by
#            handle, CopyRects, Reset. Also built for x86-64 (tests/d3d12_qemu.sh d3d8test).
#   com32    tests/windows/com32.c: COM between processes (docs/WindowsCom.md) - the program starts itself as a local server and as a service by creating
#            objects of their classes, and calls them: numbers, strings, VARIANTs, objects, callbacks, several threads. Needs widl and windres.
#   com64    the same source built for x86-64 (the one 64-bit program here: the code that passes calls on is different for each processor).
#   xml32    tests/windows/xml32.c: MSXML's document object model (msxml3.dll, msxml6.dll) - every DOMDocument class, an update response walked the way
#            Chromium's updater walks one, entities, CDATA, namespaces, white space, files, and documents that are not well formed.
#   cab32    tests/windows/cab32.c: cabinet.dll's FDI the way Chrome's mini_installer uses it - the library loaded by its path, three of the cabinet
#            reader's fixtures (stored, MSZIP, LZX) expanded through the program's own file functions and compared with the fixtures' manifest, files
#            passed over, a caller that stops, what is no cabinet, cabinets cut short. The kernel decodes (NtTcosCabinet).
#   cab64    the same source built for x86-64 (mini_installer is a 64-bit program).
# Passes if all the checks of each pass and the autorun sees exit code 42. Uses its own build folder (build-wow64-qemu) so the normal build keeps no
# autorun. Needs i686-w64-mingw32-gcc.
#   share32  tests/windows/d3dsharetest.c: Direct3D shared handles - resources, heaps and fences between two devices and with a second process that inherits the
#            handles, and gdi32's D3DKMT sharing functions with a 32-bit program's structures.
# Usage: tests/wow64_qemu.sh [hello32] [crt32] [seh32] [gui32] [lib32] [old32] [dx32] [d3d11test] [d3d10test] [d3d9test] [d3d8test] [com32] [com64] [xml32] [cab32] [cab64] [share32] [d3d12videotest] [d3d11videotest] [d3d12encodetest] [d3d12encodecodectest]      (no arguments: all)
set -euo pipefail
cd "$(dirname "$0")/.."
build=${BUILD:-build-wow64-qemu}
work=${TMPDIR:-/tmp}/tcos-wow64-qemu
rm -rf "$work"; mkdir -p "$work"
programs=("$@")
[ ${#programs[@]} -eq 0 ] && programs=(hello32 crt32 seh32 gui32 lib32 old32 dx32 d3d11test d3d10test d3d9test d3d8test com32 com64 xml32 cab32 cab64 share32 d3d12videotest d3d11videotest d3d12encodetest d3d12encodecodectest)
i686-w64-mingw32-gcc -O1 -ffreestanding -nostdlib -Isrc/Windows -Wl,--entry=_start32 -Wl,--subsystem,console -o "$work/hello32.exe" tests/windows/hello32.c
i686-w64-mingw32-gcc -O1 -o "$work/crt32.exe" tests/windows/crt32.c
i686-w64-mingw32-gcc -O1 -shared -o "$work/plug32.dll" tests/windows/plug32.c      # (a library crt32 loads while it runs)
i686-w64-mingw32-gcc -O1 -o "$work/seh32.exe" tests/windows/seh32.c
i686-w64-mingw32-gcc -O1 -o "$work/gui32.exe" tests/windows/gui32.c -ladvapi32 -luser32 -lgdi32 -lshell32 -lole32 -lcomctl32 -luuid
i686-w64-mingw32-gcc -O1 -fno-omit-frame-pointer -o "$work/lib32.exe" tests/windows/lib32.c -lws2_32 -lcomdlg32 -lwinmm -limm32 -ldbghelp -lbcrypt -luser32 -lntdll
i686-w64-mingw32-gcc -O1 -mcrtdll=msvcrt-os -o "$work/old32.exe" tests/windows/old32.c
# (the shaders dx32 and d3d11test draw with, as C arrays: every one of the kernel's own fixtures, dx_<name>_dxbc and dx_<name>_dxil, as tests/emulated_dx.sh and
# tests/d3d12_qemu.sh make them, so a new fixture needs no list here)
python3 - "$work/dxfixtures.h" <<'PY'
import glob, os, sys
out = open(sys.argv[1], "w")
for path in sorted(glob.glob("tests/data/dxbc/*.dxbc") + glob.glob("tests/data/dxbc/*.dxil") + glob.glob("tests/data/fx/*.fxo")):
    n = os.path.basename(path).replace(".", "_")
    data = open(path, "rb").read()
    out.write("static const unsigned char dx_%s[] = {%s};\n" % (n, ",".join(str(b) for b in data)))
PY
i686-w64-mingw32-gcc -O1 -I "$work" -o "$work/dx32.exe" tests/windows/dx32.c -ldxgi -ld3d12 -luuid -lgdi32
i686-w64-mingw32-gcc -O1 -I "$work" -o "$work/d3d11test.exe" tests/windows/d3d11test.c -ld3d11 -ld3d12 -ldxgi -luuid -ldxguid -luser32
i686-w64-mingw32-gcc -O1 -I "$work" -o "$work/d3d10test.exe" tests/windows/d3d10test.c -ld3d10 -ld3d11 -ldxgi -luuid -ldxguid -luser32
i686-w64-mingw32-gcc -O1 -o "$work/d3d9test.exe" tests/windows/d3d9test.c -ld3d9 -luser32 -lgdi32
i686-w64-mingw32-gcc -O1 -o "$work/d3d8test.exe" tests/windows/d3d8test.c -ld3d8 -luser32
# (com32's interfaces: the type library is a resource of the program, the header and the identifiers are generated)
widl -m32 -I /usr/include/wine/windows -t -T "$work/com32.tlb" -h -H "$work/com32_h.h" -u -U "$work/com32_i.c" tests/windows/com32.idl
(cd "$work" && echo '1 TYPELIB "com32.tlb"' | i686-w64-mingw32-windres -o com32res.o)
i686-w64-mingw32-gcc -O1 -I "$work" -o "$work/com32.exe" tests/windows/com32.c "$work/com32_i.c" "$work/com32res.o" -lole32 -loleaut32 -ladvapi32 -lshell32 -luuid
if command -v x86_64-w64-mingw32-gcc >/dev/null; then
  mkdir -p "$work/64"
  widl -m64 -I /usr/include/wine/windows -t -T "$work/64/com32.tlb" -h -H "$work/64/com32_h.h" -u -U "$work/64/com32_i.c" tests/windows/com32.idl
  (cd "$work/64" && echo '1 TYPELIB "com32.tlb"' | x86_64-w64-mingw32-windres -o com32res.o)
  x86_64-w64-mingw32-gcc -O1 -I "$work/64" -o "$work/com64.exe" tests/windows/com32.c "$work/64/com32_i.c" "$work/64/com32res.o" -lole32 -loleaut32 -ladvapi32 -lshell32 -luuid
fi
i686-w64-mingw32-gcc -O1 -o "$work/xml32.exe" tests/windows/xml32.c -lole32 -loleaut32
# (the cabinets cab32 expands, as C arrays, with what tests/data/cabinet/manifest.txt says their files are)
python3 - "$work/cabfixtures.h" <<'PY'
import struct, sys
out = open(sys.argv[1], "w")
manifest = [line.split() for line in open("tests/data/cabinet/manifest.txt") if line.strip()]
out.write("typedef struct { const char *name; unsigned size, crc; unsigned short attributes, folder; } CabFile;\n")
out.write("typedef struct { const char *name; const unsigned char *data; unsigned size; const CabFile *files; int count; int folders, set_id, reserve; } CabFixture;\n")
names = ("mszip", "stored", "lzx21")
for n in names:
    data = open("tests/data/cabinet/%s.cab" % n, "rb").read()
    out.write("static const unsigned char cab_%s_data[] = {%s};\n" % (n, ",".join(str(b) for b in data)))
    out.write("static const CabFile cab_%s_files[] = {\n" % n)
    for cabinet, folder, size, crc, attributes, name in manifest:
        if cabinet == n + ".cab":
            out.write('  {"%s", %s, 0x%su, 0x%s, %s},\n' % ("".join("\\x%02x" % b for b in bytes.fromhex(name)), size, crc, attributes, folder))
    out.write("};\n")
out.write("static const CabFixture cab_fixtures[] = {\n")
for n in names:
    data = open("tests/data/cabinet/%s.cab" % n, "rb").read()
    folders, files, flags, set_id = struct.unpack_from("<HHHH", data, 26)
    out.write('  {"cabtest_%s.cab", cab_%s_data, sizeof cab_%s_data, cab_%s_files, %d, %d, %d, %d},\n' % (n, n, n, n, files, folders, set_id, 1 if flags & 4 else 0))
out.write("};\n#define CAB_FIXTURES %d\n" % len(names))
PY
i686-w64-mingw32-gcc -O1 -I "$work" -o "$work/cab32.exe" tests/windows/cab32.c
if command -v x86_64-w64-mingw32-gcc >/dev/null; then x86_64-w64-mingw32-gcc -O1 -I "$work" -o "$work/cab64.exe" tests/windows/cab32.c; fi
i686-w64-mingw32-gcc -O1 -o "$work/share32.exe" tests/windows/d3dsharetest.c -ld3d11 -ld3d12 -ldxgi -luuid -ldxguid
# (Direct3D 12 and 11 video as 32-bit programs: the DXVA structures and the video interfaces' 32-bit layouts)
i686-w64-mingw32-gcc -O1 -o "$work/d3d12videotest.exe" tests/windows/d3d12videotest.c -ld3d12 -ldxgi -luuid -ldxguid
i686-w64-mingw32-gcc -O1 -I "$work" -o "$work/d3d11videotest.exe" tests/windows/d3d11videotest.c -ld3d11 -ld3d12 -ldxgi -luuid -ldxguid -luser32
i686-w64-mingw32-gcc -O1 -o "$work/d3d12encodetest.exe" tests/windows/d3d12encodetest.c -ld3d12 -ldxgi -luuid -ldxguid      # (video encoding and motion estimation)
i686-w64-mingw32-gcc -O1 -o "$work/d3d12encodecodectest.exe" tests/windows/d3d12encodecodectest.c -ld3d12 -ldxgi -luuid -ldxguid      # (HEVC and AV1 encoding)
status=0
for program in "${programs[@]}"; do
  arguments=
  [ "$program" = share32 ] && arguments=processes      # (and a second process)
  cmake -S . -B "$build" -G Ninja "-DTCOS_AUTORUN=/Windows/$program.exe" "-DTCOS_AUTORUN_ARGUMENTS=$arguments" -DTCOS_AUTORUN_ELEVATED=OFF -DTCOS_TRACE_SERVICE_FAILURES=OFF >/dev/null      # (not elevated: whatever an earlier build of this folder had)
  cmake --build "$build" >/dev/null
  cmake --build "$build" --target vars >/dev/null
  "$build/tools/primary_image" "$build/primary.img" "file:/Windows/$program.exe=$work/$program.exe" "file:/Windows/plug32.dll=$work/plug32.dll" >/dev/null
  qemu=$(ninja -C "$build" -t commands run-std | tail -1)
  qemu=$(sed -E 's/-audiodev [a-z]+,/-audiodev none,/' <<< "$qemu")
  rm -f "$build/debugcon.log"
  timeout "${QEMU_SECONDS:-120}" bash -c "$qemu -display none" </dev/null >/dev/null 2>&1 || true
  if [ "$program" = d3d12videotest ] || [ "$program" = d3d12encodetest ] || [ "$program" = d3d12encodecodectest ]; then
    grep -a -E "$program|FAILED|autorun:|PANIC|user program:" "$build/debugcon.log" || echo "no output from $program"
    grep -a -q -E "^$program: [0-9]* checks, 0 failed" "$build/debugcon.log" || status=1
    grep -a -q "autorun: ended, result 0, exit code 0x0* " "$build/debugcon.log" || status=1
    continue
  fi
  if [ "$program" = share32 ]; then
    # (d3dsharetest's own report, and exit code 0)
    grep -a -E "d3dsharetest|FAILED|autorun:|PANIC|user program:" "$build/debugcon.log" || echo "no output from $program"
    grep -a -q -E "^d3dsharetest: [0-9]* checks, 0 failed" "$build/debugcon.log" || status=1
    grep -a -q "autorun: ended, result 0, exit code 0x0* " "$build/debugcon.log" || status=1
    continue
  fi
  grep -a -E "$program:|autorun:|PANIC|user program:" "$build/debugcon.log" || echo "no output from $program"
  grep -a -q "$program: all .*checks passed" "$build/debugcon.log" || status=1
  grep -a -q "autorun: ended, result 0, exit code 0x0*2a " "$build/debugcon.log" || status=1
done
[ $status -eq 0 ] && echo "wow64 qemu: passed" || echo "wow64 qemu: FAILED"
exit $status
