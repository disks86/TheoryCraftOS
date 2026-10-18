#!/usr/bin/env python3
# A small FTPS server for testing, built on pyftpdlib (with pyOpenSSL): explicit FTPS (AUTH TLS), or with --implicit TLS from the first byte; both
# need TLS on the control and data connections. Users: alice / secret (read and write) and, unless --no-anonymous, "anonymous" (read only). The
# certificate is made on the first run in the given directory (self-signed, EC or RSA with --rsa). Prints "fingerprint HEX" (SHA-256 of the
# certificate in DER form) once it listens. Usage: ftps_test_server.py PORT ROOT CERTDIR [--implicit] [--no-anonymous] [--rsa] [--no-mlsd]
import hashlib, os, ssl, subprocess, sys
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.handlers import TLS_FTPHandler, FTPHandler
from pyftpdlib.servers import ThreadedFTPServer

port, root, certdir = int(sys.argv[1]), os.path.abspath(sys.argv[2]), sys.argv[3]
implicit, anonymous, rsa, mlsd = "--implicit" in sys.argv, "--no-anonymous" not in sys.argv, "--rsa" in sys.argv, "--no-mlsd" not in sys.argv
os.makedirs(certdir, exist_ok=True)
cert, key = os.path.join(certdir, "cert-%s.pem" % ("rsa" if rsa else "ec")), os.path.join(certdir, "key-%s.pem" % ("rsa" if rsa else "ec"))
if not os.path.exists(cert):
    algo = ["-newkey", "rsa:2048"] if rsa else ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1"]
    subprocess.run(["openssl", "req", "-x509", "-nodes", "-days", "30", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                    "-keyout", key, "-out", cert] + algo, check=True, capture_output=True)
der = ssl.PEM_cert_to_DER_cert(open(cert).read())

authorizer = DummyAuthorizer()
authorizer.add_user("alice", "secret", root, perm="elradfmwMT")
if anonymous: authorizer.add_anonymous(root, perm="elr")

class Handler(TLS_FTPHandler):
    certfile, keyfile = cert, key
    tls_control_required = True
    tls_data_required = True
    banner_sent = False
    def handle(self):
        if implicit:
            self.secure_connection(self.ssl_context)   # (the banner goes out once the handshake is done)
        else:
            super().handle()
    def handle_ssl_established(self):
        super().handle_ssl_established()
        if implicit and not self.banner_sent:
            self.banner_sent = True
            super().handle()
    if not mlsd:
        proto_cmds = {k: v for k, v in TLS_FTPHandler.proto_cmds.items() if k not in ("MLSD", "MLST")}
        def ftp_FEAT(self, line):
            self.push("211-Features:\r\n UTF8\r\n REST STREAM\r\n SIZE\r\n211 End\r\n")
        def ftp_MLSD(self, path): self.respond("500 not understood")
        def ftp_MLST(self, path): self.respond("500 not understood")

Handler.authorizer = authorizer
Handler.passive_ports = range(port + 1000, port + 1100) if port < 60000 else None
server = ThreadedFTPServer(("127.0.0.1", port), Handler)
print("fingerprint", hashlib.sha256(der).hexdigest(), flush=True)
server.serve_forever()
