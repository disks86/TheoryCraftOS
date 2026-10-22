#!/usr/bin/env bash
# Runs HttpClient against real servers on loopback: a Python HTTP server and the same server wrapped in TLS (certificates made on the spot,
# RSA and P-256), covering plain answers, chunked, compressed, redirects (also across the two servers), POST, big bodies, HEAD, a body sink,
# and the failures: wrong host name, untrusted certificate, refused connection, https -> http redirect. Needs openssl and python3; not part of
# run_host_tests.sh because it uses the sockets of the machine it runs on.
# Usage: tests/http_live.sh
set -uo pipefail
cd "$(dirname "$0")/.."
work=${TMPDIR:-/tmp}/tcos-http-live
mkdir -p "$work"
tool=$work/HttpLive
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src -I tests -Wall -Wextra -Wno-unused-parameter tests/HttpLive.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
  src/Network/Http/*.cpp src/Network/Tls/TlsSocket.cpp src/Network/Tls/TlsKeys.cpp src/Network/Tls/TlsCertificate.cpp src/Network/Tls/TlsSession.cpp src/Network/Tls/Tls13Client.cpp src/Network/Quic/*.cpp src/Network/Dns/*.cpp src/Crypto/*.cpp src/Media/Zlib.cpp -o "$tool" || exit 1

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
make_ca other rsa:2048   # a CA the client is not told about

cat > server.py <<'PY'
import gzip, http.server, os, socketserver, ssl, sys, threading, zlib

plain_port, tls_port, certfile, keyfile = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
BIG = b"".join(b"line %d of a big body\n" % i for i in range(150000))   # about 3.5 MB

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def send_body(self, status, body, headers=(), chunked=False):
        self.send_response(status)
        for k, v in headers: self.send_header(k, v)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
            for i in range(0, len(body), 3000):
                piece = body[i:i + 3000]
                self.wfile.write(b"%X;x=y\r\n" % len(piece) + piece + b"\r\n")
            self.wfile.write(b"0\r\nTrailer: 1\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(body))); self.end_headers()
            if self.command != "HEAD": self.wfile.write(body)
        self.close_connection = True
    def do_HEAD(self): self.do_GET()
    def do_GET(self):
        p = self.path
        if p == "/plain": self.send_body(200, b"hello from python\n", [("Content-Type", "text/plain")])
        elif p == "/chunked": self.send_body(200, BIG[:50000], chunked=True)
        elif p == "/gzip": self.send_body(200, gzip.compress(BIG[:200000]), [("Content-Encoding", "gzip")])
        elif p == "/deflate": self.send_body(200, zlib.compress(BIG[:200000]), [("Content-Encoding", "deflate")])
        elif p == "/big": self.send_body(200, BIG)
        elif p == "/close": self.send_response(200); self.end_headers(); self.wfile.write(b"until the end\n"); self.close_connection = True
        elif p == "/redirect": self.send_body(302, b"", [("Location", "/plain")])
        elif p == "/redirect2": self.send_body(301, b"", [("Location", "redirect")])
        elif p == "/to-other": self.send_body(302, b"", [("Location", "http://localhost:%d/plain" % plain_port)])
        elif p == "/to-tls": self.send_body(302, b"", [("Location", "https://localhost:%d/plain" % tls_port)])
        elif p == "/loop": self.send_body(302, b"", [("Location", "/loop")])
        elif p == "/echo-auth": self.send_body(200, (self.headers.get("Authorization") or "none").encode())
        elif p == "/missing": self.send_body(404, b"no such page\n")
        else: self.send_body(404, b"unknown\n")
    def do_POST(self):
        data = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.send_body(200, b"posted " + data + b" as " + (self.headers.get("Content-Type") or "").encode())

class Server(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True; allow_reuse_address = True

plain = Server(("127.0.0.1", plain_port), Handler)
tls = Server(("127.0.0.1", tls_port), Handler)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(certfile, keyfile)
tls.socket = ctx.wrap_socket(tls.socket, server_side=True)
threading.Thread(target=plain.serve_forever, daemon=True).start()
tls.serve_forever()
PY

pass=0; fail=0
port=$((22000 + RANDOM % 12000)); pp=$port; tp=$((port + 1))
pids=()
start_server() { python3 server.py $pp $tp "$1-srv.pem" "$1-srv.key" >server.log 2>&1 & pids+=($!); sleep 0.8; }
stop_servers() { for p in "${pids[@]}"; do kill $p 2>/dev/null; wait $p 2>/dev/null; done; pids=(); }
trap stop_servers EXIT
# run NAME EXPECT-REGEX URL [client options]    (EXPECT is matched against the client's output)
run() {
  local name=$1 expect=$2; shift 2
  local out; out=$(timeout 60 "$tool" "$@" 2>&1)
  if echo "$out" | grep -Eq "$expect"; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); echo "FAIL $name"; echo "$out" | sed 's/^/     /' | head -6; fi
}

for c in rsa p256; do
  stop_servers; start_server $c
  P=http://localhost:$pp; T=https://localhost:$tp; CA="--ca $c-ca.pem"
  run "$c http plain"            'status 200, 18 body bytes, 0 redirects.*\nhello from python|status 200, 18 body bytes' $P/plain
  run "$c https plain"           'status 200, 18 body bytes, 0 redirects' $T/plain $CA
  run "$c https resumes a session" 'TLS session resumed' $T/plain $CA --repeat 2
  run "$c https by address"      'status 200, 18 body bytes' https://127.0.0.1:$tp/plain $CA
  run "$c https chunked"         'status 200, 50000 body bytes' $T/chunked $CA --quiet
  run "$c http chunked"          'status 200, 50000 body bytes' $P/chunked --quiet
  run "$c https gzip"            'status 200, 200000 body bytes, 0 redirects.*decoded' $T/gzip $CA --quiet
  run "$c https deflate"         'status 200, 200000 body bytes.*decoded' $T/deflate $CA --quiet
  run "$c https big (3.5 MB)"    'status 200, [0-9]{7} body bytes' $T/big $CA --quiet --max-body 20000000
  run "$c https big via sink"    'status 200, [0-9]{7} body bytes' $T/big $CA --sink
  run "$c https big too large"   'FAILED: answer too large' $T/big $CA --max-body 1000000
  run "$c https until close"     'status 200, 14 body bytes' $T/close $CA
  run "$c https redirect"        'status 200, 18 body bytes, 1 redirects, https://localhost:'"$tp"'/plain' $T/redirect $CA
  run "$c https relative 301"    'status 200, 18 body bytes, 2 redirects' $T/redirect2 $CA
  run "$c http -> https"         'status 200, 18 body bytes, 1 redirects, https://' $P/to-tls $CA
  run "$c https -> http refused" 'FAILED: redirect from https to http' $T/to-other $CA
  run "$c redirect loop"         'FAILED: too many redirects' $T/loop $CA
  run "$c no redirects followed" 'status 302, 0 body bytes, 0 redirects' $T/redirect $CA --max-redirects 0
  run "$c 404 is an answer"      'status 404, 13 body bytes' $T/missing $CA
  run "$c HEAD"                  'status 200, 0 body bytes' $T/plain $CA --head
  run "$c POST"                  'status 200, .* body bytes' $T/echo $CA --post 'x=1&y=2' 
  run "$c header sent"           'status 200, 10 body bytes' $T/echo-auth $CA --header 'Authorization: Bearer abc'
  run "$c wrong host name"       'FAILED: TLS failed' https://127.0.0.1:$tp/plain --ca other-ca.pem
  run "$c untrusted CA"          'FAILED: TLS failed.*certificate' $T/plain --ca other-ca.pem
  run "$c insecure accepted"     'status 200, 18 body bytes' $T/plain --insecure
  run "$c no trust, no config"   'FAILED: TLS failed' $T/plain
  run "$c refused"               'FAILED: connection failed' http://127.0.0.1:1/
  run "$c unknown name"          'FAILED: host name not found' http://no-such-host.invalid/
done
stop_servers
echo; echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
