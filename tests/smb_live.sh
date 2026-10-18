#!/usr/bin/env bash
# Runs SmbFileSystem against a real SMB server on loopback: impacket's SMB2 server (installed from PyPI into a throwaway virtualenv, needs network
# access the first time) serving a folder, with one user account that has a password and guests allowed. Covers the login (good and wrong
# password, anonymous), share listing, listing, reading, writing, creating, renaming and deleting, a multi-megabyte file in both directions, and
# the failures. Signing (HMAC-SHA-256) is exercised by the logged-in runs. Not part of run_host_tests.sh.
# Usage: tests/smb_live.sh
set -uo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-smb-live
mkdir -p "$work"
venv=$work/venv
if [ ! -x "$venv/bin/python" ]; then
  python3 -m venv "$venv" && "$venv/bin/pip" install --quiet impacket || { echo "cannot set up impacket"; exit 1; }
fi
tool=$work/SmbLive
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src -I tests -Wall -Wextra -Wno-unused-parameter tests/SmbLive.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
  src/FileSystem/FileSystemTypes.cpp src/Network/Smb/*.cpp src/Network/ServerNetwork.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp \
  src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Checksum.cpp src/Crypto/*.cpp -o "$tool" || exit 1

share=$work/share
rm -rf "$share"; mkdir -p "$share/sub"
echo "hello from the share" > "$share/hello.txt"
echo nested > "$share/sub/inner.txt"
head -c 3000000 /dev/urandom > "$work/big.bin"

port=$((24000 + RANDOM % 12000))
"$venv/bin/python" "$venv/bin/smbserver.py" -smb2support -username alice -password secret -ip 127.0.0.1 -port $port SHARE "$share" >"$work/server.log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null; wait $pid 2>/dev/null' EXIT
sleep 2

pass=0; fail=0
S=/127.0.0.1:$port
# run NAME EXPECT-REGEX ARGS...   (EXPECT is matched against the tool's output)
run() {
  local name=$1 expect=$2; shift 2
  local out; out=$(timeout 60 "$tool" "$@" 2>&1)
  if echo "$out" | grep -Eq "$expect"; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); echo "FAIL $name"; echo "$out" | sed 's/^/     /' | head -6; fi
}
U="--user alice --password secret"

run "login, signing on"        'signing on, user' $U info $S
run "anonymous (guest) login"  'signing off, guest' info $S
run "wrong password"           'FAILED.*login failed' --user alice --password wrong ls $S
run "unknown user"             'FAILED.*login failed|guest' --user nobody --password x info $S
run "share list"               'SHARE/' $U ls $S
run "list a share"             'hello.txt 21' $U ls $S/SHARE
run "list a folder"            'inner.txt 7' $U ls $S/SHARE/sub
run "share name any case"      'hello.txt' $U ls $S/share
run "no such share"            'FAILED.*not found' $U ls $S/NOSUCH
run "no such folder"           'FAILED.*not found' $U ls $S/SHARE/nothing
run "stat file"                'hello.txt file 21' $U stat $S/SHARE/hello.txt
run "stat folder"              'sub directory' $U stat $S/SHARE/sub
run "stat missing"             'FAILED.*not found' $U stat $S/SHARE/missing
run "read"                     'hello from the share' $U cat $S/SHARE/hello.txt
run "read nested"              'nested' $U cat $S/SHARE/sub/inner.txt
run "write new file"           'wrote 12 bytes' $U write $S/SHARE/new.txt "first write."
run "read it back"             'first write\.' $U cat $S/SHARE/new.txt
run "rewrite shorter"          'wrote 5 bytes' $U write $S/SHARE/new.txt "again"
run "read rewritten"           '^again$' $U cat $S/SHARE/new.txt
run "append"                   'wrote 5 bytes' $U append $S/SHARE/new.txt "-more"
run "read appended"            'again-more' $U cat $S/SHARE/new.txt
run "mkdir"                    '^$' $U mkdir $S/SHARE/made
run "mkdir again"              'FAILED.*already exists' $U mkdir $S/SHARE/made
run "put 3 MB"                 'wrote 3000000 bytes' $U put $S/SHARE/made/big.bin "$work/big.bin"
run "stat big"                 'big.bin file 3000000' $U stat $S/SHARE/made/big.bin
"$tool" $U cat $S/SHARE/made/big.bin > "$work/big.back" 2>/dev/null
if cmp -s "$work/big.bin" "$work/big.back"; then pass=$((pass + 1)); echo "ok   get 3 MB, identical"; else fail=$((fail + 1)); echo "FAIL get 3 MB, identical"; fi
cmp -s "$work/big.bin" "$share/made/big.bin" && { pass=$((pass + 1)); echo "ok   3 MB on the server's disk"; } || { fail=$((fail + 1)); echo "FAIL 3 MB on the server's disk"; }
run "rename"                   '^$' $U mv $S/SHARE/made/big.bin $S/SHARE/made/moved.bin
run "listing after rename"     'moved.bin 3000000' $U ls $S/SHARE/made
run "rename onto existing"     'FAILED.*already exists' $U mv $S/SHARE/new.txt $S/SHARE/hello.txt
run "remove non-empty folder"  'FAILED.*not empty' $U rm $S/SHARE/made
run "remove file"              '^$' $U rm $S/SHARE/made/moved.bin
run "remove folder"            '^$' $U rm $S/SHARE/made
run "gone"                     'FAILED.*not found' $U stat $S/SHARE/made
run "remove missing"           'FAILED.*not found' $U rm $S/SHARE/made
run "unicode name"             'wrote 3 bytes' $U write "$S/SHARE/café €.txt" abc
run "unicode listing"          'café €.txt 3' $U ls $S/SHARE
run "unreachable server"       'FAILED.*network error' ls /127.0.0.1:1
run "host name (localhost)"    'SHARE/' $U ls /localhost:$port
echo; echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
