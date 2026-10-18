#!/usr/bin/env python3
# A small SFTP server for testing, built on paramiko: password login (alice / secret) and a folder served as the root. The host key is made on
# the first run in the given directory: --key rsa, ecdsa or ed25519. Usage: sftp_test_server.py PORT ROOT KEYDIR [--key TYPE] [--cipher NAME] [--mac NAME]
# (--cipher / --mac limit what the server offers, e.g. aes256-ctr, hmac-sha2-512)
# Prints "fingerprint HEX" (SHA-256 of the host key blob) once it listens.
import hashlib, os, socket, stat, subprocess, sys, threading
import paramiko
if os.environ.get("SFTP_LOG"): paramiko.util.log_to_file(os.environ["SFTP_LOG"], "DEBUG")

port, root, keydir = int(sys.argv[1]), os.path.abspath(sys.argv[2]), sys.argv[3]
keytype = sys.argv[sys.argv.index("--key") + 1] if "--key" in sys.argv else "ed25519"
os.makedirs(keydir, exist_ok=True)
path = os.path.join(keydir, "host_" + keytype)
if not os.path.exists(path):
    subprocess.run(["ssh-keygen", "-q", "-t", keytype, "-N", ""] + (["-m", "PEM"] if keytype != "ed25519" else []) + ["-f", path], check=True)
host_key = {"rsa": paramiko.RSAKey, "ecdsa": paramiko.ECDSAKey, "ed25519": paramiko.Ed25519Key}[keytype](filename=path)

def real(p):
    full = os.path.normpath(os.path.join(root, p.lstrip("/")))
    if full != root and not full.startswith(root + os.sep): raise OSError(13, "outside")
    return full

def attrs(path, name=None):
    a = paramiko.SFTPAttributes.from_stat(os.stat(path))
    if name: a.filename = name
    return a

class Handle(paramiko.SFTPHandle):
    def stat(self):
        try: return paramiko.SFTPAttributes.from_stat(os.fstat(self.readfile.fileno() if self.readfile else self.writefile.fileno()))
        except OSError as e: return paramiko.SFTPServer.convert_errno(e.errno)
    def chattr(self, attr):
        try:
            if attr.st_size is not None: os.ftruncate((self.writefile or self.readfile).fileno(), attr.st_size)
        except OSError as e: return paramiko.SFTPServer.convert_errno(e.errno)
        return paramiko.SFTP_OK

class Files(paramiko.SFTPServerInterface):
    def list_folder(self, p):
        try:
            full = real(p)
            return [attrs(os.path.join(full, n), n) for n in sorted(os.listdir(full))]
        except OSError as e: return paramiko.SFTPServer.convert_errno(e.errno)
    def stat(self, p):
        try: return attrs(real(p))
        except OSError as e: return paramiko.SFTPServer.convert_errno(e.errno)
    def lstat(self, p):
        try: return paramiko.SFTPAttributes.from_stat(os.lstat(real(p)))
        except OSError as e: return paramiko.SFTPServer.convert_errno(e.errno)
    def open(self, p, flags, attr):
        try:
            full = real(p)
            fd = os.open(full, flags, 0o644)
        except OSError as e: return paramiko.SFTPServer.convert_errno(e.errno)
        if flags & os.O_WRONLY: mode = "ab" if flags & os.O_APPEND else "wb"
        elif flags & os.O_RDWR: mode = "a+b" if flags & os.O_APPEND else "r+b"
        else: mode = "rb"
        f = os.fdopen(fd, mode)
        h = Handle(flags); h.filename = full; h.readfile = f; h.writefile = f
        return h
    def remove(self, p):
        try: os.remove(real(p))
        except OSError as e: return paramiko.SFTPServer.convert_errno(e.errno)
        return paramiko.SFTP_OK
    def rename(self, a, b):
        try: os.rename(real(a), real(b))
        except OSError as e: return paramiko.SFTPServer.convert_errno(e.errno)
        return paramiko.SFTP_OK
    def mkdir(self, p, attr):
        try: os.mkdir(real(p))
        except OSError as e: return paramiko.SFTPServer.convert_errno(e.errno)
        return paramiko.SFTP_OK
    def rmdir(self, p):
        try: os.rmdir(real(p))
        except OSError as e: return paramiko.SFTPServer.convert_errno(e.errno)
        return paramiko.SFTP_OK
    def chattr(self, p, attr):
        return paramiko.SFTP_OK
    def canonicalize(self, p):
        return "/" + os.path.relpath(real(p), root).replace(".", "", 1) if p != "/" else "/"

class Auth(paramiko.ServerInterface):
    def check_auth_password(self, user, password):
        return paramiko.AUTH_SUCCESSFUL if (user, password) == ("alice", "secret") else paramiko.AUTH_FAILED
    def get_allowed_auths(self, user): return "password"
    def check_channel_request(self, kind, chanid):
        return paramiko.OPEN_SUCCEEDED if kind == "session" else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

def serve(client):
    t = paramiko.Transport(client)
    t.add_server_key(host_key)
    if "--cipher" in sys.argv: t.get_security_options().ciphers = (sys.argv[sys.argv.index("--cipher") + 1],)
    if "--mac" in sys.argv: t.get_security_options().digests = (sys.argv[sys.argv.index("--mac") + 1],)
    t.set_subsystem_handler("sftp", paramiko.SFTPServer, Files)
    try: t.start_server(server=Auth())
    except Exception as e:
        sys.stderr.write("handshake: %r\n" % (e,)); return
    t.join()

sock = socket.socket(); sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("127.0.0.1", port)); sock.listen(8)
blob = host_key.asbytes()
print("fingerprint", hashlib.sha256(blob).hexdigest(), flush=True)
while True:
    c, _ = sock.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
