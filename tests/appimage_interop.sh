#!/usr/bin/env bash
# Checks the SquashFS reader and the AppRun interpreter against real AppImages: each AppImage's own runtime extracts it (--appimage-extract, optionally
# only the files matching a pattern), and every file, folder and link it wrote must be in AppImageTest's listing of the image with the same size, contents
# hash and link target. Then prints what AppRun would start.
# Usage: tests/appimage_interop.sh <file.AppImage>[:pattern] ...      e.g. tests/appimage_interop.sh ~/Apps/Game.AppImage ~/big.AppImage:'usr/lib/*'
# Runs the AppImage's runtime (only to extract it, never the application). FAST=1 builds the tool without sanitizers (for images of hundreds of MB).
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -gt 0 ] || { echo "usage: $0 <file.AppImage>[:pattern] ..."; exit 2; }
out=${TMPDIR:-/tmp}/tcos-appimage-interop
mkdir -p "$out"
flags="-std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined"
[ "${FAST:-0}" = 1 ] && flags="-std=c++20 -O2"
libs=""
for lib in zlib:ZLIB libzstd:ZSTD liblzma:LZMA liblz4:LZ4 lzo2:LZO; do
  if pkg-config --exists "${lib%%:*}" 2>/dev/null; then libs="$libs -DTCOS_HAVE_${lib##*:} $(pkg-config --cflags --libs "${lib%%:*}")"; fi
done
g++ $flags -I src -w tests/AppImageTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp \
    src/FileSystem/Squash/*.cpp src/FileSystem/Iso/*.cpp src/FileSystem/Mount/GraftFileSystem.cpp src/Executable/AppImage.cpp src/Program/AppRunScript.cpp src/Program/AppImageLaunch.cpp \
    src/Process/ProgramEnvironment.cpp src/Media/Zlib.cpp src/Media/Zstd.cpp src/Media/Xz.cpp src/Media/Lz4.cpp src/Media/Lzo.cpp src/Crypto/Crypto.cpp $libs -o "$out/AppImageTest"

status=0
for spec in "$@"; do
  file=${spec%%:*}
  pattern=""
  [ "$spec" != "$file" ] && pattern=${spec#*:}
  file=$(readlink -f "$file")
  name=$(basename "$file")
  work="$out/$name"
  rm -rf "$work"
  mkdir -p "$work"
  echo "== $name ${pattern:+(only $pattern)}"
  (cd "$work" && if [ -n "$pattern" ]; then "$file" --appimage-extract "$pattern" >/dev/null; else "$file" --appimage-extract >/dev/null; fi)
  "$out/AppImageTest" --dump "$file" > "$work/dump.txt"
  python3 -I - "$work/squashfs-root" "$work/dump.txt" <<'EOF' || status=1
import os, sys
root, dump = sys.argv[1], sys.argv[2]
def fnv(path):
    h = 1469598103934665603
    with open(path, 'rb') as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk: break
            for b in chunk:
                h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h
ours, errors = {}, []
for line in open(dump, encoding='utf-8', errors='surrogateescape'):
    line = line.rstrip('\n')
    kind, rest = line[0], line[2:]
    if kind == 'E': errors.append(rest); continue
    if kind == 'L':
        path, target = rest.split(' -> ', 1); ours[path] = ('L', target)
    elif kind == 'D': ours[rest] = ('D',)
    else:
        path, size, h = rest.rsplit(' ', 2); ours[path] = ('F', int(size), int(h, 16))
bad = checked = 0
for dirpath, dirnames, filenames in os.walk(root):
    for n in dirnames + filenames:
        full = os.path.join(dirpath, n)
        rel = os.path.relpath(full, root)
        got = ours.get(rel)
        if os.path.islink(full): want = ('L', os.readlink(full))
        elif os.path.isdir(full): want = ('D',)
        else: want = ('F', os.path.getsize(full), fnv(full) if os.path.getsize(full) < (64 << 20) else None)
        if want[0] == 'F' and want[2] is None and got and got[0] == 'F': want = ('F', want[1], got[2])      # (big files: size only, hashing in Python is slow)
        checked += 1
        if got != want:
            bad += 1
            if bad <= 20: print('  MISMATCH', rel, 'runtime:', want, 'ours:', got)
print(f'  {checked} entries compared, {bad} mismatches, {len(errors)} read errors, {len(ours)} entries listed by us')
for e in errors[:10]: print('  ERROR', e)
sys.exit(1 if bad or errors else 0)
EOF
  "$out/AppImageTest" --apprun "$file" | sed 's/^/  /' | grep -v '^  env \(PATH\|HOME\|LANG\)=' || status=1
  rm -rf "$work/squashfs-root"
done
exit $status
