#!/usr/bin/env bash
# Runs TlsSocket against the real thing: `openssl s_server` with freshly made certificates (RSA, P-256, P-384), in a matrix of protocol
# versions, cipher suites, key exchange groups and failure cases, plus bulk transfers both ways against a Python TLS echo server.
# Needs openssl and python3; not part of run_host_tests.sh because it uses the network stack of the machine it runs on (loopback only).
# Usage: tests/tls_live.sh
set -uo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-tls-live
mkdir -p "$work"
tool=$work/TlsLive
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src -I tests -Wall -Wextra -Wno-unused-parameter tests/TlsLive.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
  src/Network/Tls/*.cpp src/Network/Ipp/*.cpp src/Hardware/Printer/IppPrinter.cpp src/Crypto/*.cpp -o "$tool" || exit 1

cd "$work"
make_ca() { openssl req -x509 -newkey "${@:2}" -nodes -keyout "$1-ca.key" -out "$1-ca.pem" -days 365 -subj "/CN=Test CA $1" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null; }
make_server() {
  openssl req -newkey "${@:2}" -nodes -keyout "$1-srv.key" -out "$1-srv.csr" -subj "/CN=localhost" 2>/dev/null
  printf "subjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n" > "$1.ext"
  openssl x509 -req -in "$1-srv.csr" -CA "$1-ca.pem" -CAkey "$1-ca.key" -CAcreateserial -out "$1-srv.pem" -days 365 -extfile "$1.ext" 2>/dev/null
}
make_ca rsa rsa:2048;  make_server rsa rsa:2048
make_ca p256 ec -pkeyopt ec_paramgen_curve:prime256v1; make_server p256 ec -pkeyopt ec_paramgen_curve:prime256v1
make_ca p384 ec -pkeyopt ec_paramgen_curve:secp384r1;  make_server p384 ec -pkeyopt ec_paramgen_curve:secp384r1
head -c 3000000 /dev/urandom > big.bin

pass=0; fail=0
port=$((21000 + RANDOM % 15000))
# run NAME CERT "server args" "client args" EXPECT-REGEX   (EXPECT is matched against the client's output; "FAILED" cases expect the failure)
run() {
  local name=$1 cert=$2 sargs=$3 cargs=$4 expect=$5
  port=$((port + 1))
  # shellcheck disable=SC2086
  openssl s_server -accept $port -cert "$cert-srv.pem" -key "$cert-srv.key" $sargs >server.log 2>&1 &
  local pid=$!
  sleep 0.3
  # shellcheck disable=SC2086
  local out; out=$(timeout 60 "$tool" 127.0.0.1 $port --name localhost --ca "$cert-ca.pem" $cargs 2>&1)
  kill $pid 2>/dev/null; wait $pid 2>/dev/null
  if echo "$out" | grep -Eq "$expect"; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); echo "FAIL $name"; echo "$out" | sed 's/^/     /' | head -6; fi
}
OK13='handshake ok: version 0x0304'
OK12='handshake ok: version 0x0303'

for c in rsa p256 p384; do
  run "$c default"          $c "-www" ""        "200 ok"
  run "$c tls1.3"           $c "-www" "--tls13" "$OK13"
  run "$c tls1.2"           $c "-www -tls1_2" "--tls12" "$OK12"
  run "$c server only 1.2, client both" $c "-www -tls1_2" "" "$OK12"
  run "$c server only 1.3, client both" $c "-www -tls1_3" "" "$OK13"
done
for s in TLS_AES_128_GCM_SHA256 TLS_AES_256_GCM_SHA384 TLS_CHACHA20_POLY1305_SHA256; do
  run "1.3 suite $s" p256 "-www -tls1_3 -ciphersuites $s" "--tls13" "$s"
done
for s in ECDHE-RSA-AES128-GCM-SHA256 ECDHE-RSA-AES256-GCM-SHA384 ECDHE-RSA-CHACHA20-POLY1305; do
  run "1.2 suite $s" rsa "-www -tls1_2 -cipher $s" "--tls12" "$OK12"
done
for s in ECDHE-ECDSA-AES128-GCM-SHA256 ECDHE-ECDSA-AES256-GCM-SHA384 ECDHE-ECDSA-CHACHA20-POLY1305; do
  run "1.2 suite $s (P-256)" p256 "-www -tls1_2 -cipher $s" "--tls12" "$OK12"
  run "1.2 suite $s (P-384)" p384 "-www -tls1_2 -cipher $s" "--tls12" "$OK12"
done
for g in X25519 P-256 P-384; do
  run "1.3 group $g" rsa "-www -tls1_3 -groups $g" "--tls13" "$OK13"
  run "1.2 group $g" rsa "-www -tls1_2 -groups $g" "--tls12" "$OK12"
done
run "1.3 ALPN"            rsa "-www -tls1_3 -alpn http/1.1" "--alpn http/1.1" 'alpn "http/1.1"'
run "1.2 ALPN"            rsa "-www -tls1_2 -alpn http/1.1" "--alpn http/1.1" 'alpn "http/1.1"'
run "1.2 with EMS required"  rsa "-www -tls1_2" "--tls12 --ems" "$OK12.*ems"
run "1.2 bigger records"  rsa "-WWW -tls1_2" "--tls12 --get /big.bin --bytes 3000000" "received 3[0-9]{6} bytes"
run "1.3 bigger records"  rsa "-WWW -tls1_3" "--tls13 --get /big.bin --bytes 3000000" "received 3[0-9]{6} bytes"
# failures
run "wrong host name"     rsa "-www" "--name example.org" "FAILED handshake.*host name does not match"
run "untrusted CA"        rsa "-www" "--ca p256-ca.pem" "FAILED handshake.*no path to a trusted"
run "insecure accepts anything" rsa "-www" "--ca p256-ca.pem --insecure" "$OK13"
run "1.3-only client, 1.2-only server" rsa "-www -tls1_2" "--tls13" "FAILED handshake"
run "1.2-only client, 1.3-only server" rsa "-www -tls1_3" "--tls12" "FAILED handshake"

# the certificate is checked against the address when there is no name; a certificate for another address is refused
port=$((port + 1))
openssl s_server -accept $port -cert rsa-srv.pem -key rsa-srv.key -www >server.log 2>&1 &
pid=$!; sleep 0.3
out=$(timeout 30 "$tool" 127.0.0.1 $port --ca rsa-ca.pem 2>&1); kill $pid 2>/dev/null; wait $pid 2>/dev/null
if echo "$out" | grep -q "handshake ok"; then pass=$((pass + 1)); echo "ok   host check by IP address"; else fail=$((fail + 1)); echo "FAIL host check by IP address"; echo "$out" | sed 's/^/     /' | head -4; fi
printf 'subjectAltName=IP:10.9.8.7\n' > otherip.ext
openssl req -newkey rsa:2048 -nodes -keyout otherip.key -out otherip.csr -subj "/CN=other" 2>/dev/null
openssl x509 -req -in otherip.csr -CA rsa-ca.pem -CAkey rsa-ca.key -CAcreateserial -out otherip.pem -days 30 -extfile otherip.ext 2>/dev/null
port=$((port + 1))
openssl s_server -accept $port -cert otherip.pem -key otherip.key -www >server.log 2>&1 &
pid=$!; sleep 0.3
out=$(timeout 30 "$tool" 127.0.0.1 $port --ca rsa-ca.pem 2>&1); kill $pid 2>/dev/null; wait $pid 2>/dev/null
if echo "$out" | grep -q "host name does not match"; then pass=$((pass + 1)); echo "ok   certificate for another address refused"; else fail=$((fail + 1)); echo "FAIL certificate for another address"; echo "$out" | sed 's/^/     /' | head -4; fi

# key updates in both directions (s_server's K asks us to update too, k only updates its own keys)
port=$((port + 1))
(sleep 0.7; printf 'A\n'; sleep 1.5; printf 'K\n'; sleep 0.3; printf 'B\n'; sleep 1.5; printf 'k\n'; sleep 0.3; printf 'C\n'; sleep 2.5) | openssl s_server -accept $port -cert rsa-srv.pem -key rsa-srv.key -tls1_3 >server.out 2>server.log &
pid=$!; sleep 0.5
out=$(timeout 30 "$tool" 127.0.0.1 $port --name localhost --ca rsa-ca.pem --hold 6 2>&1); sleep 1; kill $pid 2>/dev/null; wait $pid 2>/dev/null
if echo "$out" | grep -q "received \[A,B,C,\]" && grep -q hello6 server.out; then pass=$((pass + 1)); echo "ok   key updates"; else fail=$((fail + 1)); echo "FAIL key updates"; echo "$out" | sed 's/^/     /' | head -4; fi

# a printer over IPPS: the IPP driver talking HTTP over TlsSocket to a Python server that answers Print-Job and Get-Printer-Attributes
cat > ipps.py <<'PY'
import http.server, ssl, struct, sys
cert, port = sys.argv[1], int(sys.argv[2])
def attr(tag, name, value): return bytes([tag]) + struct.pack(">H", len(name)) + name.encode() + struct.pack(">H", len(value)) + value
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        op, rid = struct.unpack(">HI", body[2:8])
        pos = 8; doc = b""
        while body[pos] != 3:                       # skip the attribute groups
            if body[pos] < 16: pos += 1; continue
            nl = struct.unpack(">H", body[pos+1:pos+3])[0]; vl = struct.unpack(">H", body[pos+3+nl:pos+5+nl])[0]; pos += 5 + nl + vl
        doc = body[pos+1:]
        out = bytes([1, 1, 0, 0]) + struct.pack(">I", rid) + bytes([1]) + attr(0x47, "attributes-charset", b"utf-8") + attr(0x48, "attributes-natural-language", b"en")
        if op == 2:
            open("ipps-doc.bin", "wb").write(doc)
            out += bytes([2]) + attr(0x21, "job-id", struct.pack(">i", 77)) + attr(0x23, "job-state", struct.pack(">i", 3))
        elif op == 0x0b:
            out += bytes([4]) + attr(0x23, "printer-state", struct.pack(">i", 3)) + attr(0x44, "printer-state-reasons", b"none") + attr(0x41, "printer-make-and-model", b"TLS test printer")
        out += bytes([3])
        self.send_response(200); self.send_header("Content-Type", "application/ipp"); self.send_header("Content-Length", str(len(out))); self.end_headers(); self.wfile.write(out)
    def log_message(self, *a): pass
srv = http.server.HTTPServer(("127.0.0.1", port), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert + "-srv.pem", cert + "-srv.key")
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
srv.handle_request(); srv.handle_request()         # one request for the job, one for the status
PY
for c in rsa p256; do
  port=$((port + 1)); rm -f ipps-doc.bin
  python3 ipps.py $c $port &
  pid=$!; sleep 0.6
  out=$(timeout 60 "$tool" 127.0.0.1 $port --ca $c-ca.pem --ipps-print "hello over ipps" 2>&1); wait $pid 2>/dev/null
  if echo "$out" | grep -q "job: delivered, ipp status 0x0000, http 200, job-id 77, encrypted 1" && echo "$out" | grep -q "report: ok" && [ "$(cat ipps-doc.bin 2>/dev/null)" = "hello over ipps" ]; then pass=$((pass + 1)); echo "ok   IPPS print job and status ($c certificate)"; else fail=$((fail + 1)); echo "FAIL IPPS ($c)"; echo "$out" | sed 's/^/     /' | head -5; fi
done

# bulk data both ways against an echo server in Python (TLS 1.2 and 1.3)
cat > echo.py <<'PY'
import socket, ssl, sys
version, cert, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert + "-srv.pem", cert + "-srv.key")
v = ssl.TLSVersion.TLSv1_2 if version == "12" else ssl.TLSVersion.TLSv1_3
ctx.minimum_version = ctx.maximum_version = v
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(("127.0.0.1", port)); s.listen(1)
c, _ = s.accept(); c = ctx.wrap_socket(c, server_side=True)
while True:
    d = c.recv(65536)
    if not d: break
    c.sendall(d)
c.close()
PY
for v in 12 13; do
  port=$((port + 1))
  python3 echo.py $v rsa $port &
  pid=$!
  sleep 0.5
  out=$(timeout 60 "$tool" 127.0.0.1 $port --name localhost --ca rsa-ca.pem --echo 2000000 2>&1)
  wait $pid 2>/dev/null
  if echo "$out" | grep -q "echo ok"; then pass=$((pass + 1)); echo "ok   echo 2 MB over TLS 1.${v#1}"; else fail=$((fail + 1)); echo "FAIL echo over TLS $v"; echo "$out" | sed 's/^/     /' | head -6; fi
done
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
