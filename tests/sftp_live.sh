#!/usr/bin/env bash
# Runs SftpFileSystem against real SSH servers on loopback: a paramiko based SFTP server (tests/sftp_test_server.py, paramiko installed from PyPI
# into a throwaway virtualenv, needs network access the first time) with password login, once for each host key type and with different ciphers
# and MACs, and - for the SSH transport alone - the machine's OpenSSH sshd (which cannot check passwords without root, so the login is refused
# after the key exchange and the host key check have worked). Covers listing, reading, writing, creating, renaming and deleting, a multi-megabyte
# file in both directions, the failures, the login and the host key being remembered. Not part of run_host_tests.sh.
# Usage: tests/sftp_live.sh
set -uo pipefail
cd "$(dirname "$0")/.."
repo=$PWD
work=${TMPDIR:-/tmp}/tcos-sftp-live
mkdir -p "$work"
venv=$work/venv
if [ ! -x "$venv/bin/python" ] || ! "$venv/bin/python" -c "import paramiko" 2>/dev/null; then
  python3 -m venv "$venv" && "$venv/bin/pip" install --quiet paramiko pyftpdlib pyopenssl || { echo "cannot set up paramiko"; exit 1; }
fi
tool=$work/SftpLive
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src -I tests -Wall -Wextra -Wno-unused-parameter tests/SftpLive.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
  src/FileSystem/FileSystemTypes.cpp src/Network/Sftp/*.cpp src/Network/Ssh/*.cpp src/Network/ServerNetwork.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp \
  src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Checksum.cpp src/Crypto/*.cpp -o "$tool" 2>&1 | grep -E "error" -A3
[ -x "$tool" ] || exit 1

root=$work/root
rm -rf "$root"; mkdir -p "$root/sub"
echo "hello sftp" > "$root/hello.txt"
echo nested > "$root/sub/inner.txt"
head -c 3000000 /dev/urandom > "$work/big.bin"

pids=()
cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; done; }
trap cleanup EXIT
pass=0; fail=0
# check NAME EXPECT-REGEX ARGS...
check() {
  local name=$1 expect=$2; shift 2
  local out; out=$(timeout 90 "$tool" "$@" 2>&1)
  if echo "$out" | grep -Eq "$expect"; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); echo "FAIL $name"; echo "$out" | sed 's/^/     /' | head -6; fi
}
start_server() {   # start_server PORT ARGS... ; sets $fingerprint
  local port=$1; shift
  "$venv/bin/python" tests/sftp_test_server.py "$port" "$root" "$work/keys" "$@" >"$work/server-$port.log" 2>&1 &
  pids+=($!)
  for _ in $(seq 40); do grep -q fingerprint "$work/server-$port.log" 2>/dev/null && break; sleep 0.25; done
  fingerprint=$(awk '/fingerprint/ {print $2}' "$work/server-$port.log")
}

port=$((24000 + RANDOM % 12000))
for key in ed25519 ecdsa rsa; do
  start_server $((port)) --key $key
  S=/127.0.0.1:$port
  U="--user alice --password secret"
  check "[$key] connect and list"   'hello.txt 11' $U ls $S
  check "[$key] pinned right key"   'hello.txt 11' $U --pin "$fingerprint" ls $S
  check "[$key] pinned wrong key"   'FAILED.*untrusted' $U --pin 0000000000000000000000000000000000000000000000000000000000000000 ls $S
  kill "${pids[-1]}"; wait "${pids[-1]}" 2>/dev/null; pids=("${pids[@]:0:${#pids[@]}-1}")
  port=$((port + 1))
done

start_server $port --key ed25519 --cipher aes256-ctr --mac hmac-sha2-512
check "aes256-ctr + hmac-sha2-512 (not etm)" 'hello.txt 11' --user alice --password secret ls /127.0.0.1:$port
kill "${pids[-1]}"; wait "${pids[-1]}" 2>/dev/null; pids=("${pids[@]:0:${#pids[@]}-1}")
port=$((port + 1))
start_server $port --key rsa --cipher aes128-ctr --mac hmac-sha2-256
check "aes128-ctr + hmac-sha2-256 (not etm)" 'hello.txt 11' --user alice --password secret ls /127.0.0.1:$port
kill "${pids[-1]}"; wait "${pids[-1]}" 2>/dev/null; pids=("${pids[@]:0:${#pids[@]}-1}")
port=$((port + 1))

start_server $port --key ed25519
S=/127.0.0.1:$port
U="--user alice --password secret"
check "no login yet"           'FAILED.*login required' ls $S
check "wrong password"         'FAILED.*login failed' --user alice --password wrong ls $S
check "unknown user"           'FAILED.*login failed' --user bob --password secret ls $S
check "list the root"          'sub/ 0' $U ls $S
check "list a folder"          'inner.txt 7' $U ls $S/sub
check "no such folder"         'FAILED.*not found' $U ls $S/nothing
check "stat file"              'hello.txt file 11' $U stat $S/hello.txt
check "stat folder"            'sub directory' $U stat $S/sub
check "stat missing"           'FAILED.*not found' $U stat $S/missing
check "read"                   'hello sftp' $U cat $S/hello.txt
check "read nested"            'nested' $U cat $S/sub/inner.txt
check "read a folder"          'FAILED.*(is a directory|invalid)' $U cat $S/sub
check "write new file"         'wrote 12 bytes' $U write $S/new.txt "first write."
check "read it back"           'first write\.' $U cat $S/new.txt
check "rewrite shorter"        'wrote 5 bytes' $U write $S/new.txt "again"
check "read rewritten"         '^again$' $U cat $S/new.txt
check "append"                 'wrote 5 bytes' $U append $S/new.txt "-more"
check "read appended"          'again-more' $U cat $S/new.txt
check "truncate"               '^$' $U truncate $S/new.txt 3
check "read truncated"         '^aga$' $U cat $S/new.txt
check "mkdir"                  '^$' $U mkdir $S/made
check "mkdir again"            'FAILED.*already exists' $U mkdir $S/made
check "put 3 MB"               'wrote 3000000 bytes' $U put $S/made/big.bin "$work/big.bin"
check "stat big"               'big.bin file 3000000' $U stat $S/made/big.bin
"$tool" $U cat $S/made/big.bin > "$work/big.back" 2>/dev/null
if cmp -s "$work/big.bin" "$work/big.back"; then pass=$((pass + 1)); echo "ok   get 3 MB, identical"; else fail=$((fail + 1)); echo "FAIL get 3 MB, identical"; fi
cmp -s "$work/big.bin" "$root/made/big.bin" && { pass=$((pass + 1)); echo "ok   3 MB on the server's disk"; } || { fail=$((fail + 1)); echo "FAIL 3 MB on the server's disk"; }
check "rename"                 '^$' $U mv $S/made/big.bin $S/made/moved.bin
check "listing after rename"   'moved.bin 3000000' $U ls $S/made
check "rename onto existing"   'FAILED.*already exists' $U mv $S/new.txt $S/hello.txt
check "remove non-empty folder" 'FAILED.*not empty' $U rm $S/made
check "remove file"            '^$' $U rm $S/made/moved.bin
check "remove folder"          '^$' $U rm $S/made
check "gone"                   'FAILED.*not found' $U stat $S/made
check "remove missing"         'FAILED.*not found' $U rm $S/made
check "unicode name"           'wrote 3 bytes' $U write "$S/café €.txt" abc
check "unicode listing"        'café €.txt 3' $U ls $S
check "unreachable server"     'FAILED.*network error' $U ls /127.0.0.1:1
check "host name (localhost)"  'hello.txt 11' $U ls /localhost:$port

# The SSH transport against OpenSSH itself: the key exchange, the host key check and the encryption must work for it to get as far as refusing
# the password.
if [ -x /usr/bin/sshd ] && command -v ssh-keygen >/dev/null; then
  mkdir -p "$work/sshd"
  for key in ed25519 rsa ecdsa; do
    [ -f "$work/sshd/$key" ] || ssh-keygen -q -t $key -N "" -f "$work/sshd/$key"
    sport=$((port + 10))
    cat > "$work/sshd/$key.conf" <<CONF
Port $sport
ListenAddress 127.0.0.1
HostKey $work/sshd/$key
PidFile $work/sshd/pid
PasswordAuthentication yes
UsePAM no
LogLevel DEBUG
CONF
    /usr/bin/sshd -D -e -f "$work/sshd/$key.conf" >"$work/sshd/$key.log" 2>&1 &
    spid=$!
    sleep 1
    check "OpenSSH $key host key: kex, signature, encryption" 'FAILED.*login failed' $U ls /127.0.0.1:$sport
    kill $spid 2>/dev/null; wait $spid 2>/dev/null
    port=$((port + 1))
  done
fi
echo; echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
