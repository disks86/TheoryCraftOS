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
# Fast loop for src/Windows/oleaut32: builds oleaut32.dll with the host's mingw compiler (over Wine's kernel32), and runs Wine's oleaut32 tests against it with Wine's
# native-DLL override. This checks the library code, not the OS under it (scripts/winetest.sh oleaut32 does that, in QEMU).
#   SKIP="ops date" scripts/oleaut32-wine.sh build      leaves pieces (src/Windows/oleaut32/<piece>.inc) out, for work on one while others are unfinished
#   scripts/oleaut32-wine.sh build                      build the DLL into $OUT (default build/oleaut32-wine)
#   scripts/oleaut32-wine.sh tests                      build the tests once (tests/windows/wine.py build)
#   scripts/oleaut32-wine.sh run <test> [seconds]       run oleaut32_<test>.exe (vartest, vartype, safearray, varformat, olefont, olepicture, dispatch, usrmarshal, ...)
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${OUT:-build/oleaut32-wine}
TESTS=${TESTS:-$OUT/tests}      # (one agent builds them with `tests`; others can point TESTS at that directory)
mkdir -p "$OUT"
export WINEPREFIX=${WINEPREFIX:-$PWD/$OUT/prefix} WINEDEBUG=${WINEDEBUG:--all} WINEARCH=win64
cc=${CC:-x86_64-w64-mingw32-gcc}
case "$1" in
build)
  src=src/Windows
  stubs=$OUT/stubs; mkdir -p "$stubs"
  # (the stub list without what the sources define by now)
  python3 - "$src" "$stubs" "${SKIP:-}" <<'PY'
import glob, re, sys
src, out = sys.argv[1], sys.argv[2]
skip = sys.argv[3].split()
names = set()
for path in glob.glob(src + "/oleaut32/*.c") + glob.glob(src + "/oleaut32/*.inc"):
    if path.split("/")[-1][:-4] in skip: continue
    for line in open(path, errors="replace"):
        m = re.match(r"^\s*(?:EXPORT|__declspec\(dllexport\))[^()=;]*?\b(\w+)\s*\(", line)
        if m: names.add(m.group(1))
lines = open(src + "/stubs/oleaut32.inc").read().split("\n")
open(out + "/oleaut32.inc", "w").write("\n".join(l for l in lines if not (l.startswith("X(") and l[2:-1] in names)))
PY
  cp $src/stubs/stubs.c "$stubs/stubs.c"

  # the exports that are still stubs, so that the tests link and run
  $cc -O2 -Wall -Wextra -Wno-unused-parameter -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-stack-protector -fasynchronous-unwind-tables -nostdlib -shared \
    -Wl,--entry=DllMain -Wno-builtin-declaration-mismatch -DSTUB_FILE=oleaut32.inc -DSTUB_RESULT=0 -DSTUB_DLLMAIN -Wl,--image-base=0x180A00000 \
    -o "$OUT/oleaut32.dll" $src/oleaut32/oleaut32.c $stubs/stubs.c -lkernel32 -lntdll -ladvapi32 $(for n in ${SKIP:-}; do echo -DOA_SKIP_$n; done) "${@:2}"
  ;;
tests) python3 tests/windows/wine.py build "$TESTS" oleaut32 >/dev/null; ls "$TESTS"/*.exe ;;
run)
  t=$2; secs=${3:-300}
  [ -d "$WINEPREFIX/drive_c" ] || wineboot -u >/dev/null 2>&1
  # (the test prefix is ours: the DLL replaces the placeholder in its System32)
  [ -f "$WINEPREFIX/drive_c/windows/system32/oleaut32.dll.wine" ] || cp "$WINEPREFIX/drive_c/windows/system32/oleaut32.dll" "$WINEPREFIX/drive_c/windows/system32/oleaut32.dll.wine"
  cp "$OUT/oleaut32.dll" "$WINEPREFIX/drive_c/windows/system32/oleaut32.dll"
  cd "$TESTS"
  WINEDLLOVERRIDES="oleaut32=n,b" timeout "$secs" wine "oleaut32_$t.exe" 2>&1 | grep -v "^WARNING: radv" || true
  ;;
esac
