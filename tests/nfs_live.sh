#!/usr/bin/env bash
# Runs NfsFileSystem against tests/nfs_mock_server.py on loopback (a small NFS version 3 server written from the RFCs: port mapper, MOUNT and NFS on
# one port; real folders as exports, one only for user id 1000, one read-only, one inside another). If libnfs's nfs-ls / nfs-cat are installed the
# mock itself is first checked against them (that needs the port mapper on port 111, so it runs in a private network namespace: `unshare -rn`).
# Covers listing the export tree, reading, writing, creating, renaming, deleting, a multi-megabyte file in both directions, AUTH_SYS ids and the
# failures. Not part of run_host_tests.sh.
# Usage: tests/nfs_live.sh
set -uo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-nfs-live
mkdir -p "$work"
tool=$work/NfsLive
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src -I tests -Wall -Wextra -Wno-unused-parameter tests/NfsLive.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
  src/FileSystem/FileSystemTypes.cpp src/Network/Nfs/*.cpp src/Network/ServerNetwork.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp src/Network/Dhcp/*.cpp \
  src/Network/Dns/*.cpp src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Checksum.cpp -o "$tool" || exit 1

root=$work/root
rm -rf "$root"; mkdir -p "$root/data/sub" "$root/data/nested" "$root/public" "$root/private" "$root/nested"
echo "hello nfs" > "$root/data/hello.txt"
echo inner > "$root/data/sub/inner.txt"
echo "read me" > "$root/public/readme"
echo secret > "$root/private/secret.txt"
echo deep > "$root/nested/deep.txt"
head -c 3000000 /dev/urandom > "$work/big.bin"

port=$((24000 + RANDOM % 12000))
python3 tests/nfs_mock_server.py $port "$root" >"$work/server.log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null; wait $pid 2>/dev/null' EXIT
sleep 1

pass=0; fail=0
S=/127.0.0.1:$port
run() {
  local name=$1 expect=$2; shift 2
  local out; out=$(timeout 60 "$tool" "$@" 2>&1)
  if echo "$out" | grep -Eq "$expect"; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); echo "FAIL $name"; echo "$out" | sed 's/^/     /' | head -6; fi
}
U="--uid 1000 --gid 1000"

run "server root"              'export/' ls $S
run "export tree folder"       'data/' ls $S/export
run "list an export"           'hello.txt 10' ls $S/export/data
run "list a folder"            'inner.txt 6' ls $S/export/data/sub
run "nested export shows"      'nested/' ls $S/export/data
run "inside the nested export" 'deep.txt 5' ls $S/export/data/nested
run "no such export folder"    'FAILED.*not found' ls $S/export/nothing
run "no such folder"           'FAILED.*not found' ls $S/export/data/nothing
run "stat file"                'hello.txt file 10' stat $S/export/data/hello.txt
run "stat folder"              'sub directory' stat $S/export/data/sub
run "stat missing"             'FAILED.*not found' stat $S/export/data/missing
run "read"                     'hello nfs' cat $S/export/data/hello.txt
run "read nested"              'inner' cat $S/export/data/sub/inner.txt
run "private denied (65534)"   'FAILED.*(denied|access)' ls $S/export/private
run "private allowed (1000)"   'secret.txt 7' $U ls $S/export/private
run "private read (1000)"      'secret' $U cat $S/export/private/secret.txt
run "read-only export lists"   'readme 8' ls $S/export/public
run "read-only export reads"   'read me' cat $S/export/public/readme
run "read-only export write"   'FAILED' write $S/export/public/new.txt x
run "write new file"           'wrote 12 bytes' write $S/export/data/new.txt "first write."
run "read it back"             'first write\.' cat $S/export/data/new.txt
run "rewrite shorter"          'wrote 5 bytes' write $S/export/data/new.txt "again"
run "read rewritten"           '^again$' cat $S/export/data/new.txt
run "append"                   'wrote 5 bytes' append $S/export/data/new.txt "-more"
run "read appended"            'again-more' cat $S/export/data/new.txt
run "truncate"                 '^$' truncate $S/export/data/new.txt 3
run "read truncated"           '^aga$' cat $S/export/data/new.txt
run "mkdir"                    '^$' mkdir $S/export/data/made
run "mkdir again"              'FAILED.*already exists' mkdir $S/export/data/made
run "put 3 MB"                 'wrote 3000000 bytes' put $S/export/data/made/big.bin "$work/big.bin"
run "stat big"                 'big.bin file 3000000' stat $S/export/data/made/big.bin
"$tool" cat $S/export/data/made/big.bin > "$work/big.back" 2>/dev/null
if cmp -s "$work/big.bin" "$work/big.back"; then pass=$((pass + 1)); echo "ok   get 3 MB, identical"; else fail=$((fail + 1)); echo "FAIL get 3 MB, identical"; fi
cmp -s "$work/big.bin" "$root/data/made/big.bin" && { pass=$((pass + 1)); echo "ok   3 MB on the server's disk"; } || { fail=$((fail + 1)); echo "FAIL 3 MB on the server's disk"; }
run "rename"                   '^$' mv $S/export/data/made/big.bin $S/export/data/made/moved.bin
run "listing after rename"     'moved.bin 3000000' ls $S/export/data/made
run "remove non-empty folder"  'FAILED.*not empty' rm $S/export/data/made
run "remove file"              '^$' rm $S/export/data/made/moved.bin
run "remove folder"            '^$' rm $S/export/data/made
run "gone"                     'FAILED.*not found' stat $S/export/data/made
run "remove missing"           'FAILED.*not found' rm $S/export/data/made
run "unicode name"             'wrote 3 bytes' write "$S/export/data/café €.txt" abc
run "unicode listing"          'café €.txt 3' ls $S/export/data
run "unreachable server"       'FAILED.*network error' ls /127.0.0.1:1
run "host name (localhost)"    'export/' ls /localhost:$port

# The mock against libnfs (the reference client), if it is installed: needs port 111, so a private network namespace.
nfsls=$(command -v nfs-ls || true)
if [ -n "$nfsls" ] && unshare -rn true 2>/dev/null; then
  loader=; [ -x "$nfsls" ] || loader=/lib64/ld-linux-x86-64.so.2
  out=$(unshare -rn bash -c "ip link set lo up; python3 tests/nfs_mock_server.py 111 '$root' >/dev/null 2>&1 & sleep 1; $loader $nfsls 'nfs://127.0.0.1/export/data/?version=3' 2>&1; $loader $(command -v nfs-cat) 'nfs://127.0.0.1/export/data/hello.txt?version=3' 2>&1; kill %1")
  if echo "$out" | grep -q "hello.txt" && echo "$out" | grep -q "hello nfs"; then pass=$((pass + 1)); echo "ok   mock server agrees with libnfs"; else fail=$((fail + 1)); echo "FAIL mock server vs libnfs"; echo "$out" | head -5; fi
fi
echo; echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
