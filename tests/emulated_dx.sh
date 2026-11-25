#!/usr/bin/env bash
# TheoryCraftOS - a minimal UEFI x86_64 operating system.
# Copyright (C) 2026 Christopher Schaefer
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Runs a Windows test program on the CPU emulator with this OS's own system DLLs (src/Windows: dxgi.dll, d3d12.dll ...) AND the kernel's Direct3D: win32k with the
# NtGdiDdDDI* services over a dxgk::Kernel with the software renderer for its one adapter. It is tests/emulated_windows.sh plus that kernel half.
# Usage: tests/emulated_dx.sh [program.c ...]      (default: tests/windows/d3d12test.c)
# Needs the UEFI cross toolchain (to build the DLLs) and mingw (x86_64-w64-mingw32-gcc) for the program. The host objects are cached in $TMPDIR/tcos-emudx/obj (delete the folder
# after changing a header). Use a fresh $TMPDIR when the working copy moves: the DLL build keeps absolute paths.
set -euo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-emudx
export TCOS_EMU_SECONDS=${TCOS_EMU_SECONDS:-300}      # (the Direct3D suites run longer than the runner's 60 seconds)
mkdir -p "$work/sys32" "$work/obj"
if [ ! -f "$work/build/build.ninja" ]; then
  cmake -S . -B "$work/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-x86_64-uefi.cmake" -DCMAKE_BUILD_TYPE=Debug > /dev/null
fi
ninja -C "$work/build" windows_dlls > /dev/null
cp "$work"/build/windows/*.dll "$work/sys32/"

sources=(tests/EmulationRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Crypto/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp src/Kernel/Sync.cpp src/Kernel/Clipboard.cpp
         tests/KernelStubs.cpp src/Hardware/Keyboard/KeyCodes.cpp src/Process/Win32k/*.cpp src/Process/Win32k/Dxg/*.cpp src/Kernel/Dxgkrnl/*.cpp src/Graphics/VirtGpu/VirtGpuSession.cpp src/Hardware/VideoCodec/Dxva/*.cpp src/Hardware/VideoCodec/VideoProcess.cpp src/Hardware/VideoCodec/Software/*.cpp src/Hardware/VideoCodec/ColorConvert.cpp src/Hardware/VideoCodec/VideoFrameBuffer.cpp src/Hardware/VideoCodec/VideoTypes.cpp src/Math/FloatMath.cpp src/Graphics/Graphics3D/BuiltinShaders.cpp
         src/Graphics/Graphics3D/Software/*.cpp src/Graphics/Graphics2D/Software/*.cpp src/Hardware/GraphicsProcessingUnit/Font8x16.cpp)
flags="-std=c++20 -O1 -pthread -I src -w -DTCOS_EMU_DXG"
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

# The shaders the programs use, as C arrays (tests/data/dxbc is the kernel's own set of fixtures).
python3 - "$work/dxfixtures.h" <<'PY'
import sys
out = open(sys.argv[1], "w")
import glob, os
for path in sorted(glob.glob("tests/data/dxbc/*.dxbc") + glob.glob("tests/data/dxbc/*.dxil")):      # (dx_<name>_dxbc, dx_<name>_dxil: as tests/d3d12_qemu.sh names them)
    n = os.path.basename(path).replace(".", "_")
    data = open(path, "rb").read()
    out.write("static const unsigned char dx_%s[] = {%s};\n" % (n, ",".join(str(b) for b in data)))
PY

programs=("$@")
[ ${#programs[@]} -gt 0 ] || programs=(tests/windows/d3d12test.c)
fail=0
for src in "${programs[@]}"; do
  name=$(basename "${src%.*}")
  x86_64-w64-mingw32-gcc -O1 -Wall -Wextra -Wno-unused-parameter -I "$work" -o "$work/$name.exe" "$src" -ld3d11 -ld3d9 -ldxgi -ld3d12 -luuid -ldxguid -luser32 -lgdi32
  echo "== $name"
  "$work/emurun" "$work/$name.exe" "$work/sys32" | tee "$work/$name.out"
  grep -q -e ", 0 failed" -e "all [0-9]* checks passed" "$work/$name.out" || fail=1
  grep -q "faulted=0" "$work/$name.out" || fail=1
done
exit $fail
