#!/usr/bin/env python3
# A small NFS version 3 server for testing, written from RFC 1813 (NFS), RFC 1813 appendix I (MOUNT), RFC 1833 (port mapper) and RFC 5531 (RPC): the port
# mapper, MOUNT v3 and NFS v3 services all answer on ONE TCP port. It serves real folders as exports; "private" exports only let a given user id in
# (AUTH_SYS), a "readonly" export refuses writes. Files made by clients are recorded as owned by the user id they came with (the server runs
# unprivileged and cannot chown). Usage: nfs_mock_server.py PORT EXPORTS_ROOT   (exports: <root>/data, <root>/public (read-only), <root>/private (uid 1000))
import os, socketserver, struct, sys, threading, zlib

PORT = int(sys.argv[1])
ROOT = os.path.abspath(sys.argv[2])
EXPORTS = {  # export path -> (folder, read only, allowed uid or None)
    "/export/data": (os.path.join(ROOT, "data"), False, None),
    "/export/public": (os.path.join(ROOT, "public"), True, None),
    "/export/private": (os.path.join(ROOT, "private"), False, 1000),
    "/export/data/nested": (os.path.join(ROOT, "nested"), False, None),
}
PMAP, MOUNT, NFS = 100000, 100005, 100003
owners = {}      # real path -> (uid, gid) recorded for files clients made
handles = {}     # file handle -> (real path, export key)
lock = threading.Lock()

NF3 = {"REG": 1, "DIR": 2, "LNK": 5}
OK, PERM, NOENT, IO, ACCES, EXIST, NOTDIR, ISDIR, INVAL, NOSPC, ROFS, NAMETOOLONG, NOTEMPTY, STALE, BADHANDLE, NOTSUPP = 0, 1, 2, 5, 13, 17, 20, 21, 22, 28, 30, 63, 66, 70, 10001, 10004

def u32(v): return struct.pack(">I", v & 0xFFFFFFFF)
def u64(v): return struct.pack(">Q", v)
def opaque(b): return u32(len(b)) + b + b"\0" * (-len(b) % 4)
def string(s): return opaque(s.encode() if isinstance(s, str) else s)

class Reader:
    def __init__(self, data): self.d, self.i = data, 0
    def u32(self): v = struct.unpack_from(">I", self.d, self.i)[0]; self.i += 4; return v
    def u64(self): v = struct.unpack_from(">Q", self.d, self.i)[0]; self.i += 8; return v
    def opaque(self):
        n = self.u32(); b = self.d[self.i:self.i + n]; self.i += n + (-n % 4); return b
    def string(self): return self.opaque().decode("utf-8", "replace")
    def fixed(self, n): b = self.d[self.i:self.i + n]; self.i += n + (-n % 4); return b
    def boolean(self): return self.u32() != 0

def handle_for(path, key):
    st = os.lstat(path)
    fh = struct.pack(">IQQ", zlib.crc32(key.encode()), st.st_ino, st.st_dev & 0xFFFFFFFFFFFF) + b"mock"
    handles[fh] = (path, key)
    return fh

def fattr(path, key):
    st = os.lstat(path)
    import stat as S
    t = 2 if S.S_ISDIR(st.st_mode) else 5 if S.S_ISLNK(st.st_mode) else 1
    uid, gid = owners.get(path, (st.st_uid, st.st_gid))
    size = st.st_size if t != 2 else 4096
    mt = lambda x: struct.pack(">II", int(x), int((x % 1) * 1e9))
    return (u32(t) + u32(st.st_mode & 0o7777) + u32(st.st_nlink) + u32(uid) + u32(gid) + u64(size) + u64(st.st_blocks * 512) + u32(0) + u32(0) + u64(1) + u64(st.st_ino) +
            mt(st.st_atime) + mt(st.st_mtime) + mt(st.st_ctime))

def post_attr(path, key):
    try: return u32(1) + fattr(path, key)
    except OSError: return u32(0)

def wcc(path, key): return u32(0) + post_attr(path, key)

def check_name(name): return "/" not in name and name not in ("", ".", "..") and len(name) <= 255

class Handler(socketserver.BaseRequestHandler):
    def read_exact(self, n):
        data = b""
        while len(data) < n:
            chunk = self.request.recv(n - len(data))
            if not chunk: raise EOFError
            data += chunk
        return data
    def handle(self):
        try:
            while True:
                record = b""
                while True:
                    word = struct.unpack(">I", self.read_exact(4))[0]
                    record += self.read_exact(word & 0x7FFFFFFF)
                    if word & 0x80000000: break
                reply = self.dispatch(record)
                self.request.sendall(struct.pack(">I", 0x80000000 | len(reply)) + reply)
        except (EOFError, ConnectionError):
            pass
    def dispatch(self, record):
        r = Reader(record)
        xid, mtype, rpcvers, prog, vers, proc = [r.u32() for _ in range(6)]
        flavor = r.u32(); cred = r.opaque(); r.u32(); r.opaque()
        uid = gid = None
        if flavor == 1:
            c = Reader(cred); c.u32(); c.string(); uid = c.u32(); gid = c.u32()
        if os.environ.get('NFS_MOCK_LOG'): print('call', prog, vers, proc, flush=True)
        head = u32(xid) + u32(1) + u32(0) + u32(0) + u32(0)   # reply, accepted, AUTH_NONE verifier
        try:
            with lock:
                body = self.call(prog, vers, proc, r, uid, gid)
        except struct.error:
            return head + u32(4)   # GARBAGE_ARGS
        if body is None:
            if os.environ.get('NFS_MOCK_LOG'): print('unhandled', prog, vers, proc, flush=True)
            return head + u32(3 if prog in (PMAP, MOUNT, NFS) else 1)
        return head + u32(0) + body
    def call(self, prog, vers, proc, r, uid, gid):
        if prog == PMAP and vers == 2: return self.portmap(proc, r)
        if prog == MOUNT and vers == 3: return self.mount(proc, r, uid)
        if prog == NFS and vers == 3: return self.nfs(proc, r, uid, gid)
        return None
    def portmap(self, proc, r):
        if proc == 0: return b""
        if proc == 3:
            p, v, prot, _ = r.u32(), r.u32(), r.u32(), r.u32()
            ok = prot == 6 and ((p == MOUNT and v == 3) or (p == NFS and v == 3) or (p == PMAP and v == 2))
            return u32(PORT if ok else 0)
        if proc == 4:
            out = b""
            for p, v in ((PMAP, 2), (MOUNT, 3), (NFS, 3)): out += u32(1) + u32(p) + u32(v) + u32(6) + u32(PORT)
            return out + u32(0)
        return None
    def export_for(self, path):
        for key, (folder, ro, who) in EXPORTS.items():
            if key == path: return key
        return None
    def mount(self, proc, r, uid):
        if proc == 0: return b""
        if proc == 1:
            path = r.string().rstrip("/") or "/"
            if path not in EXPORTS: return u32(NOENT)
            folder, ro, who = EXPORTS[path]
            if who is not None and uid != who: return u32(ACCES)
            fh = handle_for(folder, path)
            return u32(0) + opaque(fh) + u32(1) + u32(1)
        if proc == 3 or proc == 4: return b""
        if proc == 2: return u32(0)
        if proc == 5:
            out = b""
            for key in EXPORTS: out += u32(1) + string(key) + u32(0)
            return out + u32(0)
        return None
    # --- NFS
    def access(self, key, uid):
        who = EXPORTS[key][2]
        return who is None or uid == who
    def nfs(self, proc, r, uid, gid):
        if proc == 0: return b""
        if proc == 19:   # FSINFO
            fh = r.opaque()
            return u32(0) + u32(0) + u32(32768) + u32(32768) + u32(512) + u32(32768) + u32(32768) + u32(512) + u32(8192) + u64(2**40) + struct.pack(">II", 0, 1) + u32(0x1B)
        if proc == 20: return u32(0) + u32(0) + u32(0) + u32(255) + u32(1) + u32(1) + u32(1) + u32(0)   # PATHCONF
        fh = r.opaque()
        if fh not in handles: return u32(STALE) + (u32(0) if proc in (3, 6, 16, 17) else wcc_none(proc))
        path, key = handles[fh]
        if not self.access(key, uid): return u32(ACCES) + (u32(0) if proc in (1, 3, 4, 6, 16, 17) else (u32(0) if proc == 1 else wcc_none(proc)))
        readonly = EXPORTS[key][1]
        write_ops = (2, 7, 8, 9, 12, 13, 14)
        if readonly and proc in write_ops: return u32(ROFS) + wcc_none(proc)
        try:
            return self.nfs_op(proc, r, fh, path, key, uid, gid)
        except FileNotFoundError: return u32(NOENT) + wcc_none(proc)
        except NotADirectoryError: return u32(NOTDIR) + wcc_none(proc)
        except IsADirectoryError: return u32(ISDIR) + wcc_none(proc)
        except FileExistsError: return u32(EXIST) + wcc_none(proc)
        except PermissionError: return u32(ACCES) + wcc_none(proc)
        except OSError as e:
            return u32(NOTEMPTY if e.errno == 39 else NOSPC if e.errno == 28 else IO) + wcc_none(proc)
    def nfs_op(self, proc, r, fh, path, key, uid, gid):
        if proc == 1: return u32(0) + fattr(path, key)
        if proc == 2:   # SETATTR
            size = None
            if r.boolean(): r.u32()
            if r.boolean(): r.u32()
            if r.boolean(): r.u32()
            if r.boolean(): size = r.u64()
            if size is not None:
                with open(path, "r+b") as f: f.truncate(size)
            return u32(0) + wcc(path, key)
        if proc == 3:   # LOOKUP
            name = r.string()
            child = os.path.join(path, name)
            if not check_name(name) and name not in (".", ".."): return u32(INVAL) + post_attr(path, key)
            if name == ".": child = path
            if not os.path.isdir(path): return u32(NOTDIR) + post_attr(path, key)
            if not os.path.lexists(child): return u32(NOENT) + post_attr(path, key)
            return u32(0) + opaque(handle_for(child, key)) + post_attr(child, key) + post_attr(path, key)
        if proc == 4: want = r.u32(); return u32(0) + post_attr(path, key) + u32(want)
        if proc == 6:   # READ
            offset, count = r.u64(), r.u32()
            if os.path.isdir(path): return u32(ISDIR) + post_attr(path, key)
            with open(path, "rb") as f:
                f.seek(offset); data = f.read(min(count, 65536))
            eof = offset + len(data) >= os.path.getsize(path)
            return u32(0) + post_attr(path, key) + u32(len(data)) + u32(1 if eof else 0) + opaque(data)
        if proc == 7:   # WRITE
            offset, count, stable = r.u64(), r.u32(), r.u32(); data = r.opaque()
            with open(path, "r+b") as f:
                f.seek(offset); f.write(data[:count]); f.flush(); os.fsync(f.fileno())
            return u32(0) + wcc(path, key) + u32(count) + u32(2) + u64(0x1122334455667788)
        if proc in (8, 9):   # CREATE, MKDIR
            name = r.string()
            if not check_name(name): return u32(INVAL) + wcc(path, key)
            mode = r.u32() if proc == 8 else None
            child = os.path.join(path, name)
            if proc == 8:
                how = mode
                if how == 0 or how == 1:
                    if os.path.lexists(child) and how == 1: return u32(EXIST) + wcc(path, key)
                    open(child, "wb").close() if how == 0 or not os.path.lexists(child) else None
                else: open(child, "xb").close()
            else:
                os.mkdir(child)
            owners[child] = (uid if uid is not None else 0, gid if gid is not None else 0)
            return u32(0) + u32(1) + opaque(handle_for(child, key)) + post_attr(child, key) + wcc(path, key)
        if proc in (12, 13):   # REMOVE, RMDIR
            name = r.string(); child = os.path.join(path, name)
            if not check_name(name): return u32(INVAL) + wcc(path, key)
            if proc == 12:
                if os.path.isdir(child): return u32(ISDIR) + wcc(path, key)
                os.remove(child)
            else:
                if not os.path.isdir(child): return u32(NOTDIR) + wcc(path, key)
                os.rmdir(child)
            owners.pop(child, None)
            return u32(0) + wcc(path, key)
        if proc == 14:   # RENAME
            from_name = r.string(); to_fh = r.opaque(); to_name = r.string()
            if to_fh not in handles: return u32(STALE) + wcc(path, key) + u32(0)
            to_path, to_key = handles[to_fh]
            if to_key != key or not check_name(from_name) or not check_name(to_name): return u32(INVAL) + wcc(path, key) + u32(0)
            src, dst = os.path.join(path, from_name), os.path.join(to_path, to_name)
            if not os.path.lexists(src): return u32(NOENT) + wcc(path, key) + wcc(to_path, key)
            os.replace(src, dst)
            if src in owners: owners[dst] = owners.pop(src)
            return u32(0) + wcc(path, key) + wcc(to_path, key)
        if proc in (16, 17):   # READDIR, READDIRPLUS
            cookie = r.u64(); r.fixed(8); r.u32()
            if proc == 17: r.u32()
            if not os.path.isdir(path): return u32(NOTDIR) + post_attr(path, key)
            names = [".", ".."] + sorted(os.listdir(path))
            out = u32(0) + post_attr(path, key) + b"\0" * 8
            used = 0
            i = cookie
            while i < len(names):
                name = names[i]
                child = path if name == "." else os.path.dirname(path) if name == ".." else os.path.join(path, name)
                try: ino = os.lstat(child).st_ino
                except OSError: i += 1; continue
                entry = u32(1) + u64(ino) + string(name) + u64(i + 1)
                if proc == 17:
                    entry += post_attr(child, key) + (u32(1) + opaque(handle_for(child, key)) if name not in (".", "..") else u32(0))
                if used + len(entry) > 20000 and i > cookie: break
                out += entry; used += len(entry); i += 1
            return out + u32(0) + u32(1 if i >= len(names) else 0)
        if proc == 21: return u32(0) + wcc(path, key) + u64(0x1122334455667788)
        if proc == 18:   # FSSTAT
            return u32(0) + post_attr(path, key) + u64(2**40) + u64(2**39) + u64(2**39) + u64(10**6) + u64(10**6) + u64(10**6) + u32(0)
        return u32(NOTSUPP) + wcc_none(proc)

def wcc_none(proc):
    return u32(0) if proc in (1, 3, 4, 6, 16, 17) else u32(0) + u32(0) if proc in (2, 7, 8, 9, 12, 13, 14, 21) else b""

class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True

if __name__ == "__main__":
    for folder, _, _ in EXPORTS.values(): os.makedirs(folder, exist_ok=True)
    with Server(("127.0.0.1", PORT), Handler) as s:
        print("serving", flush=True)
        s.serve_forever()
