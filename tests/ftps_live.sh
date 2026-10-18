#!/usr/bin/env bash
# Runs FtpFileSystem against a real FTPS server on loopback: pyftpdlib (installed from PyPI into a throwaway virtualenv, needs network access
# the first time) with TLS, user alice (read and write) and anonymous (read only). Covers explicit FTPS (AUTH TLS) and implicit FTPS (port 990,
# which needs a private network namespace: `unshare -rn`), certificates of both kinds and their being remembered, servers with and without MLSD,
# the login (good, wrong, anonymous, refused anonymous), listing, reading (also from the middle of a file), writing, appending, creating, renaming
# and deleting, a multi-megabyte file in both directions, and the failures. Not part of run_host_tests.sh.
# Usage: tests/ftps_live.sh
set -uo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-ftps-live
mkdir -p "$work"
venv=${TMPDIR:-/tmp}/tcos-sftp-live/venv
if [ ! -x "$venv/bin/python" ] || ! "$venv/bin/python" -c "import pyftpdlib, OpenSSL" 2>/dev/null; then
  venv=$work/venv
  if [ ! -x "$venv/bin/python" ] || ! "$venv/bin/python" -c "import pyftpdlib, OpenSSL" 2>/dev/null; then
    python3 -m venv "$venv" && "$venv/bin/pip" install --quiet paramiko pyftpdlib pyopenssl || { echo "cannot set up pyftpdlib"; exit 1; }
  fi
fi
tool=$work/FtpLive
root=$work/root
pids=()
cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; done; }
trap cleanup EXIT
pass=0; fail=0
check() {   # check NAME EXPECT-REGEX ARGS...
  local name=$1 expect=$2; shift 2
  local out; out=$(timeout 90 "$tool" "$@" 2>&1)
  if echo "$out" | grep -Eq "$expect"; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); echo "FAIL $name"; echo "$out" | sed 's/^/     /' | head -6; fi
}
start_server() {   # start_server PORT ARGS... ; sets $fingerprint
  local port=$1; shift
  "$venv/bin/python" tests/ftps_test_server.py "$port" "$root" "$work/certs" "$@" >"$work/server-$port.log" 2>&1 &
  pids+=($!)
  for _ in $(seq 40); do grep -q fingerprint "$work/server-$port.log" 2>/dev/null && break; sleep 0.25; done
  fingerprint=$(awk '/fingerprint/ {print $2}' "$work/server-$port.log")
}
stop_server() { kill "${pids[-1]}" 2>/dev/null; wait "${pids[-1]}" 2>/dev/null; pids=("${pids[@]:0:${#pids[@]}-1}"); }

reset_root() {
  rm -rf "$root"; mkdir -p "$root/sub"
  echo "hello ftps" > "$root/hello.txt"
  echo nested > "$root/sub/inner.txt"
  head -c 3000000 /dev/urandom > "$work/big.bin"
}

# The whole list of checks for one server: $1 is the host:port.
suite() {
  local S=/$1 U="--user alice --password secret"
  check "list the root"          'hello.txt 11' $U ls $S
  check "list a folder"          'inner.txt 7' $U ls $S/sub
  check "no such folder"         'FAILED.*not found' $U ls $S/nothing
  check "stat file"              'hello.txt file 11' $U stat $S/hello.txt
  check "stat folder"            'sub directory' $U stat $S/sub
  check "stat missing"           'FAILED.*not found' $U stat $S/missing
  check "read"                   'hello ftps' $U cat $S/hello.txt
  check "read nested"            'nested' $U cat $S/sub/inner.txt
  check "read a folder"          'FAILED.*(is a directory|not found)' $U cat $S/sub
  check "read from the middle"   '^ftps$' $U range $S/hello.txt 6 4
  check "write new file"         'wrote 12 bytes' $U write $S/new.txt "first write."
  check "read it back"           'first write\.' $U cat $S/new.txt
  check "rewrite shorter"        'wrote 5 bytes' $U write $S/new.txt "again"
  check "read rewritten"         '^again$' $U cat $S/new.txt
  check "append"                 'wrote 5 bytes' $U append $S/new.txt "-more"
  check "read appended"          'again-more' $U cat $S/new.txt
  check "truncate to zero"       '^$' $U truncate $S/new.txt 0
  check "empty now"              'new.txt file 0' $U stat $S/new.txt
  check "truncate elsewhere"     'FAILED.*unsupported' $U truncate $S/hello.txt 3
  check "mkdir"                  '^$' $U mkdir $S/made
  check "mkdir again"            'FAILED.*already exists' $U mkdir $S/made
  check "put 3 MB"               'wrote 3000000 bytes' $U put $S/made/big.bin "$work/big.bin"
  check "stat big"               'big.bin file 3000000' $U stat $S/made/big.bin
  "$tool" $U cat $S/made/big.bin > "$work/big.back" 2>/dev/null
  if cmp -s "$work/big.bin" "$work/big.back"; then pass=$((pass + 1)); echo "ok   get 3 MB, identical"; else fail=$((fail + 1)); echo "FAIL get 3 MB, identical"; fi
  cmp -s "$work/big.bin" "$root/made/big.bin" && { pass=$((pass + 1)); echo "ok   3 MB on the server's disk"; } || { fail=$((fail + 1)); echo "FAIL 3 MB on the server's disk"; }
  "$tool" $U range $S/made/big.bin 1234567 100000 > "$work/range.back" 2>/dev/null
  if cmp -s "$work/range.back" <(tail -c +1234568 "$work/big.bin" | head -c 100000); then pass=$((pass + 1)); echo "ok   read from the middle of a big file"; else fail=$((fail + 1)); echo "FAIL read from the middle of a big file"; fi
  check "rename"                 '^$' $U mv $S/made/big.bin $S/made/moved.bin
  check "listing after rename"   'moved.bin 3000000' $U ls $S/made
  check "rename onto existing"   'FAILED.*already exists' $U mv $S/hello.txt $S/new.txt
  check "remove non-empty folder" 'FAILED.*not empty' $U rm $S/made
  check "remove file"            '^$' $U rm $S/made/moved.bin
  check "remove folder"          '^$' $U rm $S/made
  check "gone"                   'FAILED.*not found' $U stat $S/made
  check "remove missing"         'FAILED.*not found' $U rm $S/made
  check "unicode name"           'wrote 3 bytes' $U write "$S/café €.txt" abc
  check "unicode listing"        'café €.txt 3' $U ls $S
  check "name with spaces"       'wrote 3 bytes' $U write "$S/a b c.txt" abc
  check "spaces listing"         'a b c.txt 3' $U ls $S
}

if [ "${1:-}" = "--implicit-suite" ]; then   # (inside the network namespace)
  ip link set lo up
  g_port=990
  reset_root
  start_server 990 --implicit
  check "implicit FTPS (port 990)" 'hello.txt 11' --user alice --password secret ls /127.0.0.1:990
  suite 127.0.0.1:990
  check "implicit: certificate pinned right" 'hello.txt 11' --user alice --password secret --pin "$fingerprint" ls /127.0.0.1:990
  check "implicit: certificate pinned wrong" 'FAILED.*untrusted' --user alice --password secret --pin 0000000000000000000000000000000000000000000000000000000000000000 ls /127.0.0.1:990
  echo "$pass $fail" > "$work/implicit.result"
  exit 0
fi

g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src -I tests -Wall -Wextra -Wno-unused-parameter tests/FtpLive.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
  src/FileSystem/FileSystemTypes.cpp src/Network/Ftp/*.cpp src/Network/ServerNetwork.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp src/Network/Dhcp/*.cpp \
  src/Network/Dns/*.cpp src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Checksum.cpp src/Network/Tls/*.cpp src/Crypto/*.cpp -o "$tool" 2>&1 | grep -E "error" -A3
[ -x "$tool" ] || exit 1

port=$((24000 + RANDOM % 12000))
reset_root
start_server $port
S=/127.0.0.1:$port
U="--user alice --password secret"
check "login, certificate remembered" 'hello.txt 11' $U --pin "$fingerprint" ls $S
check "wrong certificate"      'FAILED.*untrusted' $U --pin 0000000000000000000000000000000000000000000000000000000000000000 ls $S
check "wrong password"         'FAILED.*login' --user alice --password wrong ls $S
check "unknown user"           'FAILED.*login' --user bob --password secret ls $S
check "anonymous list"         'hello.txt 11' ls $S
check "anonymous read"         'hello ftps' cat $S/hello.txt
check "anonymous cannot write" 'FAILED' write $S/anon.txt x
check "anonymous cannot mkdir" 'FAILED.*(denied|not possible)' mkdir $S/anon
suite 127.0.0.1:$port
stop_server

port=$((port + 1)); reset_root
start_server $port --no-anonymous
check "anonymous refused"      'FAILED.*login required' ls /127.0.0.1:$port
stop_server

port=$((port + 1)); reset_root
start_server $port --rsa
check "RSA certificate"        'hello.txt 11' $U --pin "$fingerprint" ls /127.0.0.1:$port
stop_server

port=$((port + 1)); reset_root
start_server $port --no-mlsd
echo "-- a server without MLSD (LIST output is parsed)"
suite 127.0.0.1:$port
stop_server

check "unreachable server"     'FAILED.*network error' $U ls /127.0.0.1:1

# Implicit FTPS (port 990) needs a private network namespace to bind that port.
if unshare -rn true 2>/dev/null; then
  rm -f "$work/implicit.result"
  unshare -rn env TMPDIR="${TMPDIR:-/tmp}" "$0" --implicit-suite 2>&1
  if [ -f "$work/implicit.result" ]; then read -r p f < "$work/implicit.result"; pass=$((pass + p)); fail=$((fail + f)); else fail=$((fail + 1)); echo "FAIL implicit suite did not finish"; fi
fi
echo; echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
