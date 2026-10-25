#!/usr/bin/env bash
# Cross-checks NtfsFileSystem against ntfs-3g's own tools (package ntfsprogs): mkntfs makes the volumes, our code mounts and changes them,
# ntfsfix -n and ntfscat check the result, and files written by ntfscp are read back by our code. Skips quietly if mkntfs is missing.
# Usage: tests/ntfs_interop.sh
set -euo pipefail
cd "$(dirname "$0")/.."
command -v mkntfs >/dev/null && command -v ntfsfix >/dev/null && command -v ntfscat >/dev/null && command -v ntfscp >/dev/null || { echo "ntfsprogs not installed: skipped"; exit 0; }
work=$(mktemp -d "${TMPDIR:-/tmp}/ntfs-interop.XXXXXX")
trap 'rm -rf "$work"' EXIT
${CXX:-g++} -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I src -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -Wno-unused-variable \
  tests/NtfsTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ntfs/*.cpp -o "$work/NtfsTest"
mkdir "$work/src"
head -c 100 /dev/urandom > "$work/src/tiny.bin"; head -c 300 /dev/urandom > "$work/src/A Fairly Long File Name.txt"; head -c 3000000 /dev/urandom > "$work/src/big.bin"
for i in $(seq 1 300); do head -c $((i * 13)) /dev/urandom > "$work/src/file_number_$i.with.long.name"; done
fail=0
for cfg in "32M 512 4096" "64M 512 1024" "16M 512 512"; do
  set -- $cfg; img="$work/v.img"; rm -f "$img"; truncate -s "$1" "$img"
  mkntfs -F -Q -q -s "$2" -c "$3" -L INTEROP "$img" >/dev/null 2>&1
  # 1. our code changes a fresh mkntfs volume
  rm -rf "$work/exp"; mkdir "$work/exp"
  "$work/NtfsTest" real "$img" "$work/out.img" "$work/exp" | tail -1
  ntfsfix -n "$work/out.img" 2>&1 | grep -q "processed successfully" || { echo "FAIL ($cfg): ntfsfix"; fail=1; }
  while read -r p; do p=${p#.}; ntfscat -q "$work/out.img" "$p" 2>/dev/null | cmp -s - "$work/exp$p" || { echo "FAIL ($cfg): $p differs"; fail=1; }; done < <(cd "$work/exp" && find . -type f)
  # 2. ntfs-3g writes, our code reads
  cp "$img" "$work/t.img"
  for f in "$work"/src/*; do ntfscp -f -q "$work/t.img" "$f" "/$(basename "$f")" >/dev/null 2>&1; done
  rm -rf "$work/dump"; mkdir "$work/dump"
  "$work/NtfsTest" dump "$work/t.img" "$work/dump" | tail -1
  for f in "$work"/src/*; do cmp -s "$f" "$work/dump/$(basename "$f")" || { echo "FAIL ($cfg): read of $(basename "$f")"; fail=1; }; done
  echo "geometry $cfg done"
done
[ $fail = 0 ] && echo "ntfs interop: all passed"
exit $fail
