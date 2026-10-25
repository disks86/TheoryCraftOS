#!/usr/bin/env bash
# Cross-checks BefsFileSystem against Haiku's own BFS code (bfs_shell, which is Haiku's file system driver with a small shell around it):
# bfs_shell makes and fills the volumes, our code reads them and changes them, and bfs_shell's checkfs and cp judge the result. Also the other
# way: files written by our code are copied out by Haiku and compared. Skips quietly if bfs_shell is not available.
#
# bfs_shell is built from the Haiku sources (host tools only, no Haiku needed):
#   git clone --depth 1 https://github.com/haiku/haiku.git; git clone --depth 1 https://github.com/haiku/buildtools.git
#   (cd buildtools/jam && make)                                    # Haiku's jam
#   mkdir out && cd out && ../haiku/configure --host-only --no-downloads     # (needs nasm; a stub that prints "elf32"/"elf64" for -hf will do)
#   ../buildtools/jam/bin.linuxx86/jam -q -j8 '<build>bfs_shell'
#   export BFS_SHELL=$PWD/objects/linux/x86_64/release/tools/bfs_shell/bfs_shell
# Usage: tests/befs_interop.sh      (BFS_SHELL=path to the binary, or bfs_shell in PATH; BEFS_INTEROP_STRICT=1 also fails on the index gap;
#                                   BEFS_INTEROP_CONFIGS="bs image files bigsize|..." picks other volumes)
set -euo pipefail
cd "$(dirname "$0")/.."
shell=${BFS_SHELL:-$(command -v bfs_shell || true)}
[ -n "$shell" ] && [ -x "$shell" ] || { echo "bfs_shell not found (set BFS_SHELL, see the top of this script): skipped"; exit 0; }
work=$(mktemp -d "${TMPDIR:-/tmp}/befs-interop.XXXXXX")
trap 'rm -rf "$work"' EXIT
${CXX:-g++} -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I src -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -Wno-unused-variable \
  tests/BefsTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Befs/*.cpp -o "$work/BefsTest"

# What goes on the volumes (host side; bfs_shell addresses the volume as /myfs and the host as :path). The files of /frag fill the volume, every
# other one is removed again so that the free space is in small pieces, and the big file is copied last: it has to be spread over more runs
# than the inode and its indirect run array hold, which brings in the double indirect run arrays.
make_source() {  # block size, number of /frag files, size of big.bin
  src="$work/src"; rm -rf "$src" "$work/fragsrc"
  mkdir -p "$src/dir1/sub" "$src/many" "$src/emptydir" "$src/frag" "$work/fragsrc"
  head -c 200 /dev/urandom > "$src/small.txt"
  head -c 50000 /dev/urandom > "$src/medium.bin"
  head -c "$3" /dev/urandom > "$src/big.bin"
  head -c 700 /dev/urandom > "$src/dir1/sub/deep.txt"
  : > "$src/zero length"
  head -c 300 /dev/urandom > "$src/héllo wörld.txt"
  head -c 90 /dev/urandom > "$src/$(printf 'n%.0s' $(seq 1 255))"
  for i in $(seq 0 599); do head -c $(( (i * 37) % 2500 )) /dev/urandom > "$src/many/$(printf 'a_reasonably_long_file_name_%04d.dat' $i)"; done
  for i in $(seq 0 $(( $2 - 1 ))); do head -c 3000 /dev/urandom > "$work/fragsrc/$(printf 'f%05d' $i)"; done
  for i in $(seq 1 2 $(( $2 - 1 ))); do cp "$work/fragsrc/$(printf 'f%05d' $i)" "$src/frag/"; done   # (the odd ones stay on the volume)
}
common_commands() {
  echo "mkdir /myfs/dir1"; echo "mkdir /myfs/dir1/sub"; echo "mkdir /myfs/many"; echo "mkdir /myfs/emptydir"; echo "mkdir /myfs/frag"
  for f in small.txt medium.bin "zero length" "héllo wörld.txt" "$(printf 'n%.0s' $(seq 1 255))"; do echo "cp \":$src/$f\" \"/myfs/$f\""; done
  echo "cp :$src/dir1/sub/deep.txt /myfs/dir1/sub/deep.txt"
  for f in "$src"/many/*; do echo "cp :$f /myfs/many/$(basename "$f")"; done
  for f in "$work"/fragsrc/*; do echo "cp :$f /myfs/frag/$(basename "$f")"; done
  echo "quit"
}

# checkfs prints counts; every problem counter must be zero
checkfs() {  # image, label
  local out
  out=$(printf 'checkfs\nquit\n' | "$shell" "$1" 2>&1)
  if echo "$out" | grep -E -q '^\s*[1-9][0-9]* blocks (not allocated|already set|could be freed)|rror|arning|corrupt|invalid'; then
    echo "FAIL ($2): Haiku's checkfs complains:"; echo "$out" | sed -n '1,40p'; return 1
  fi
  echo "$out" | grep -E "indirect block runs|double indirect" | sed "s/^/  ($2) /"
  return 0
}

fail=0
configs=${BEFS_INTEROP_CONFIGS:-"1024 64M 2000 900000|2048 64M 2400 4600000|4096 128M 5200 17500000|8192 128M 600 2200000"}   # block size, image size, files in /frag, size of big.bin
IFS="|" read -ra config_list <<< "$configs"
for cfg in "${config_list[@]}"; do
  set -- $cfg; bs=$1; size=$2; make_source "$bs" "$3" "$4"
  img="$work/v.img"; rm -f "$img"; truncate -s "$size" "$img"
  "$shell" --initialize "$img" INTEROP "block_size $bs" > "$work/init.log" 2>&1 || { echo "FAIL ($bs): bfs_shell cannot initialize"; cat "$work/init.log"; fail=1; continue; }
  common_commands > "$work/stage1.cmds"
  "$shell" "$img" < "$work/stage1.cmds" > "$work/fill.log" 2>&1 || true
  # all the free space but 128 KiB goes to one big filler file, then the even /frag files go and the big file has to use the holes; the
  # filler goes last
  free_blocks=$(printf 'info\nquit\n' | "$shell" "$img" 2>&1 | sed -n 's/.*free size:.*(\([0-9]*\) blocks).*/\1/p')
  head -c $(( free_blocks * bs - 131072 )) /dev/zero > "$work/filler.bin"
  {
    echo "cp :$work/filler.bin /myfs/filler.bin"
    for i in $(seq 0 2 $(( $3 - 1 ))); do printf 'rm /myfs/frag/f%05d\n' $i; done
    echo "cp :$src/big.bin /myfs/big.bin"
    echo "rm /myfs/filler.bin"    # (room for our own operations later)
    echo "quit"
  } > "$work/stage2.cmds"
  "$shell" "$img" < "$work/stage2.cmds" >> "$work/fill.log" 2>&1 || true
  sed -i 's|fssh:/> ||g' "$work/fill.log"
  grep -q 'Error' "$work/fill.log" && { echo "FAIL ($bs): bfs_shell reported errors while filling"; grep 'Error' "$work/fill.log" | head -5; printf 'info\nquit\n' | "$shell" "$img" 2>&1 | grep 'free size'; fail=1; continue; }
  checkfs "$img" "$bs filled by Haiku" || fail=1

  # 1. Haiku wrote it, our code reads it
  rm -rf "$work/dump"; mkdir "$work/dump"
  "$work/BefsTest" dump "$img" "$work/dump" | tail -1
  diff -r "$src" "$work/dump" > "$work/diff.log" 2>&1 || { echo "FAIL ($bs): our reader differs from what Haiku wrote:"; head -5 "$work/diff.log"; fail=1; }

  # 2. our code changes a volume Haiku made; Haiku judges it
  rm -rf "$work/exp"; cp -r "$src" "$work/exp"
  "$work/BefsTest" real "$img" "$work/out.img" "$work/exp" | tail -1 || { echo "FAIL ($bs): our operations failed"; fail=1; }
  checkfs "$work/out.img" "$bs changed by us" || fail=1
  rm -rf "$work/back"; mkdir "$work/back"
  printf 'cp -r /myfs :%s\nquit\n' "$work/back" | "$shell" "$work/out.img" > /dev/null 2>&1 || true
  diff -r "$work/exp" "$work/back/myfs" > "$work/diff.log" 2>&1 || { echo "FAIL ($bs): Haiku reads something else than we wrote:"; head -8 "$work/diff.log"; fail=1; }
  # 3. what Haiku's queries find: the name index must list exactly the files and folders that are there
  if [ -z "${BEFS_INTEROP_SKIP_INDEX:-}" ]; then
    printf 'query "name==*"\nquit\n' | "$shell" "$work/out.img" 2>&1 | sed -n 's|^ *\(/myfs/.*\)$|\1|p' | sed 's|^/myfs||' | sort > "$work/indexed.txt"
    (cd "$work/exp" && find . -mindepth 1 | sed 's|^\.||') | sort > "$work/present.txt"
    comm -23 "$work/present.txt" "$work/indexed.txt" > "$work/not_indexed.txt"
    comm -13 "$work/present.txt" "$work/indexed.txt" > "$work/stale.txt"
    if [ -s "$work/not_indexed.txt" ] || [ -s "$work/stale.txt" ]; then
      # BefsFileSystem does not maintain Haiku's indices yet (new and renamed files are missing from them, removed ones stay): a known gap that
      # only fails the run with BEFS_INTEROP_STRICT=1
      echo "${BEFS_INTEROP_STRICT:+FAIL}${BEFS_INTEROP_STRICT:-KNOWN GAP} ($bs): the name index does not match the files: $(wc -l < "$work/not_indexed.txt") missing from it (e.g. $(head -2 "$work/not_indexed.txt" | tr '\n' ' ')), $(wc -l < "$work/stale.txt") stale entries (e.g. $(head -2 "$work/stale.txt" | tr '\n' ' '))"
      [ -n "${BEFS_INTEROP_STRICT:-}" ] && fail=1
    fi
  fi
  echo "block size $bs done"
done
[ $fail = 0 ] && echo "befs interop: all passed"
exit $fail
