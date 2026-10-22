#!/usr/bin/env bash
# Runs HttpClient's HTTP/3 against a local aioquic server (needs openssl, python3 with aioquic: pip install aioquic): plain answers, a big body, gzip, redirects,
# POST, headers both ways (many, long, repeated), a stream the server rejects, and the Alt-Svc path (the first request over TCP, the next over QUIC) and the
# fallback to TCP when nothing answers on UDP. Not part of run_host_tests.sh because it uses the sockets of the machine it runs on.
# Usage: tests/http3_live.sh [python-with-aioquic]
set -uo pipefail
cd "$(dirname "$0")/.."
py=${1:-${TCOS_PYTHON:-python3}}
work=${TMPDIR:-/tmp}/tcos-http3-live
mkdir -p "$work"
tool=$work/HttpLive
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src -I tests -Wall -Wextra -Wno-unused-parameter tests/HttpLive.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
  src/Network/Http/*.cpp src/Network/Tls/TlsSocket.cpp src/Network/Tls/TlsKeys.cpp src/Network/Tls/TlsCertificate.cpp src/Network/Tls/TlsSession.cpp src/Network/Tls/Tls13Client.cpp src/Network/Quic/*.cpp src/Network/Dns/*.cpp \
  src/Crypto/*.cpp src/Media/Zlib.cpp -o "$tool" || exit 1
server=$PWD/tests/http3_test_server.py
chaos=$PWD/tests/udp_chaos_proxy.py
cd "$work"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout ca.key -out ca.pem -days 30 -subj "/CN=Test CA" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout other-ca.key -out other-ca.pem -days 30 -subj "/CN=Other CA" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout srv.key -out srv.csr -subj "/CN=localhost" 2>/dev/null
printf "subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature\nextendedKeyUsage=serverAuth\n" > srv.ext
openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out srv.pem -days 30 -extfile srv.ext 2>/dev/null

port=$((22000 + RANDOM % 12000)); tcp_only=$((port + 1))
"$py" "$server" $port $tcp_only srv.pem srv.key >server.log 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null' EXIT
sleep 1.5
# a UDP forwarder in front of the server that loses 5% of the datagrams, duplicates 3% and delays each by up to 3 ms (so they also arrive out of order)
proxy=$((port + 2))
"$py" "$chaos" $proxy $port 0.05 0.03 3 7 >proxy.log 2>&1 &
proxy_pid=$!
trap 'kill $pid $proxy_pid 2>/dev/null' EXIT
sleep 0.5
pass=0; fail=0
run() {   # run NAME EXPECT-REGEX args...
  local name=$1 expect=$2; shift 2
  local out; out=$(timeout 60 "$tool" "$@" --ca ${CA:-ca.pem} 2>&1)
  if { [ "${INVERT:-}" ] && ! echo "$out" | grep -Eq "$expect" && echo "$out" | grep -Eq "status 200"; } || { [ -z "${INVERT:-}" ] && echo "$out" | grep -Eq "$expect"; }; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); echo "FAIL $name"; echo "$out" | sed 's/^/     /' | head -8; fi
}
U=https://localhost:$port; P="--http3 prefer"
run "plain"                'status 200, 14 body bytes, 0 redirects.*\nover HTTP/3|over HTTP/3' $U/plain $P
run "body arrives"         'hello over h3' $U/plain $P
run "big (3.5 MB)"         'status 200, [0-9]{7} body bytes.*' $U/big $P --quiet --max-body 20000000
run "big via sink"         'status 200, [0-9]{7} body bytes' $U/big $P --sink
run "big too large"        'FAILED: answer too large' $U/big $P --max-body 1000000
run "gzip"                 'status 200, 200000 body bytes.*decoded' $U/gzip $P --quiet
run "redirect"             'status 200, 14 body bytes, 1 redirects' $U/redirect $P
run "404"                  'status 404, 13 body bytes' $U/missing $P
run "no content-length"    'status 200, 15 body bytes' $U/nolength $P
run "204"                  'status 204, 0 body bytes' $U/empty $P
run "server closes after the answer" 'status 200, 8 body bytes' $U/goodbye $P
run "HEAD"                 'status 200' $U/plain $P --head
run "POST"                 'POST x=1&y=2 as text/plain' $U/echo $P --post 'x=1&y=2'
run "request headers"      '"user-agent": "TheoryCraftOS' $U/headers $P
run "extra header"         '"x-custom": "v alue"' $U/headers $P --header 'X-Custom: v alue'
INVERT=1 run "connection header dropped" '"connection"' $U/headers $P --header 'Connection: keep-alive'
run "stream rejected -> tcp" 'status 200, 9 body bytes' https://localhost:$port/reset $P   # (QUIC refuses with H3_REQUEST_REJECTED, TCP answers)
CA=other-ca.pem run "untrusted certificate" 'FAILED: TLS failed' $U/plain $P
run "alt-svc: first over tcp"  'round 1: http3 0' $U/plain --http3 auto --repeat 2
run "alt-svc: then over quic"  'over HTTP/3' $U/plain --http3 auto --repeat 2
run "auto without alt-svc"   'status 200, 9 body bytes' https://localhost:$tcp_only/plain --http3 auto
run "fallback to tcp"        'status 200, 9 body bytes' https://localhost:$tcp_only/plain $P
run "quic: second connection resumes" 'TLS session resumed' $U/plain --http3 prefer --repeat 2
run "off"                    'status 200, 9 body bytes' $U/plain --http3 off
L=https://localhost:$proxy
run "lossy: plain"         'status 200, 14 body bytes.*|over HTTP/3' $L/plain $P
run "lossy: big (3.5 MB)"  'status 200, [0-9]{7} body bytes' $L/big $P --quiet --max-body 20000000
run "lossy: gzip"          'status 200, 200000 body bytes.*decoded' $L/gzip $P --quiet
run "lossy: POST"          'POST x=1&y=2 as text/plain' $L/echo $P --post 'x=1&y=2'
run "lossy: big POST"      'status 200, 300019 body bytes' $L/echo $P --post-size 300000 --quiet
run "big POST"             'status 200, 300019 body bytes' $U/echo $P --post-size 300000 --quiet
echo; echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
