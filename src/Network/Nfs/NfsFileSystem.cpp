/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "Network/Nfs/NfsFileSystem.hpp"
#include "FileSystem/Path.hpp"
#include "Network/Nfs/Xdr.hpp"
#include "Utility/Move.hpp"

namespace network {

using namespace filesystem;

namespace {

constexpr uint32_t kPortmapper = 100000, kMountd = 100005, kNfs = 100003;
// NFS version 3 procedures.
enum Nfs3 : uint32_t {
  kNull = 0, kGetattr = 1, kSetattr = 2, kLookup = 3, kRead = 6, kWrite = 7, kCreate = 8, kMkdir = 9, kRemove = 12, kRmdir = 13, kRename = 14, kReaddirplus = 17, kFsinfo = 19,
};
constexpr uint32_t kMaxFileHandle = 64;

RpcAuth sys_auth(uint32_t uid, uint32_t gid) {
  RpcAuth a;
  a.sys = true;
  a.uid = uid;
  a.gid = gid;
  return a;
}

}  // namespace

Status NfsFileSystem::map_status(uint32_t s) {
  switch (s) {
    case 0: return Status::Ok;
    case 1: case 13: return Status::AccessDenied;   // not owner, permission denied
    case 2: return Status::NotFound;
    case 17: return Status::AlreadyExists;
    case 20: return Status::NotADirectory;
    case 21: return Status::IsADirectory;
    case 66: return Status::NotEmpty;
    case 28: case 69: return Status::NoSpace;       // no space, over quota
    case 30: return Status::ReadOnly;
    case 22: case 63: return Status::InvalidPath;   // invalid argument, name too long
    case 70: case 10001: case 71: return Status::Network;   // stale or bad file handle: the export has changed under us
    case 10004: case 18: return Status::Unsupported;
    default: return Status::IoError;
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------- encoding

namespace {

void put_handle(XdrWriter& w, const uint8_t* bytes, uint32_t length) { w.opaque(ConstByteView(bytes, length)); }

// fattr3: type, mode, nlink, uid, gid, size, used, rdev, fsid, fileid, atime, mtime, ctime.
struct Fattr {
  uint32_t type = 0, mode = 0;
  uint64_t size = 0;
};
bool read_fattr(XdrReader& r, Fattr* a) {
  a->type = r.u32();
  a->mode = r.u32();
  r.u32();   // nlink
  r.u32();   // uid
  r.u32();   // gid
  a->size = r.u64();
  r.skip(8 + 8 + 8 + 8);   // used, rdev, fsid, fileid
  r.skip(8 * 3);           // atime, mtime, ctime
  return r.ok();
}

// post_op_attr: a flag, then the attributes if it is set.
bool read_post_op_attr(XdrReader& r, Fattr* a) {
  if (!r.boolean()) return false;
  return read_fattr(r, a);
}

// wcc_data: the attributes before (a flag and size, mtime, ctime) and after the operation.
void skip_wcc(XdrReader& r) {
  if (r.boolean()) r.skip(8 + 8 + 8);
  if (r.boolean()) r.skip(84);
}

// sattr3 that sets a mode and/or a size and leaves the rest.
void write_sattr(XdrWriter& w, bool set_mode, uint32_t mode, bool set_size, uint64_t size) {
  w.boolean(set_mode);
  if (set_mode) w.u32(mode);
  w.boolean(false);   // uid
  w.boolean(false);   // gid
  w.boolean(set_size);
  if (set_size) w.u64(size);
  w.u32(0);   // atime: leave
  w.u32(0);   // mtime: leave
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------- handles

class NfsFileSystem::File final : public IFile {
 public:
  File(NfsFileSystem& fs, SharedPointer<Server> server, const FileHandle& handle, uint64_t size, bool writable)
      : fs_(fs), server_(util::move(server)), handle_(handle), size_(size), writable_(writable) {}
  uint64_t size() const override { return size_; }
  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    uint64_t done = 0;
    while (done < buffer.size()) {
      uint32_t want = static_cast<uint32_t>(buffer.size() - done < server_->read_size ? buffer.size() - done : server_->read_size);
      ByteBuffer args(fs_.allocator_), reply(fs_.allocator_);
      XdrWriter w(&args);
      put_handle(w, handle_.bytes, handle_.length);
      w.u64(offset + done);
      w.u32(want);
      Status s = fs_.nfs_call(*server_, kRead, args.view(), &reply);
      if (s == Status::Ok) {
        XdrReader r(reply.view());
        const uint32_t status = r.u32();
        if (status != 0) {
          s = map_status(status);
        } else {
          Fattr ignored;
          read_post_op_attr(r, &ignored);
          r.u32();   // count
          const bool eof = r.boolean();
          const ConstByteView data = r.opaque(want);
          if (!r.ok()) {
            s = Status::Network;
          } else {
            buffer.subview(done, data.size()).copy_from(data);
            done += data.size();
            if (eof || data.empty()) break;
            continue;
          }
        }
      }
      *transferred = done;
      return s;
    }
    *transferred = done;
    return Status::Ok;
  }
  Status write(uint64_t offset, ConstByteView data, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    if (!writable_) return Status::ReadOnly;
    uint64_t done = 0;
    while (done < data.size()) {
      const uint32_t count = static_cast<uint32_t>(data.size() - done < server_->write_size ? data.size() - done : server_->write_size);
      ByteBuffer args(fs_.allocator_), reply(fs_.allocator_);
      XdrWriter w(&args);
      put_handle(w, handle_.bytes, handle_.length);
      w.u64(offset + done);
      w.u32(count);
      w.u32(2);   // FILE_SYNC: on the server's disk before the answer
      w.opaque(data.subview(done, count));
      if (!w.ok()) return Status::OutOfMemory;
      Status s = fs_.nfs_call(*server_, kWrite, args.view(), &reply);
      uint32_t wrote = 0;
      if (s == Status::Ok) {
        XdrReader r(reply.view());
        const uint32_t status = r.u32();
        if (status != 0) {
          s = map_status(status);
        } else {
          skip_wcc(r);
          wrote = r.u32();
          if (!r.ok() || wrote == 0 || wrote > count) s = Status::Network;
        }
      }
      if (s != Status::Ok) {
        *transferred = done;
        return s;
      }
      done += wrote;
      if (offset + done > size_) size_ = offset + done;
    }
    *transferred = done;
    return Status::Ok;
  }
  Status resize(uint64_t new_size) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (!writable_) return Status::ReadOnly;
    ByteBuffer args(fs_.allocator_), reply(fs_.allocator_);
    XdrWriter w(&args);
    put_handle(w, handle_.bytes, handle_.length);
    write_sattr(w, false, 0, true, new_size);
    w.boolean(false);   // no guard
    if (!w.ok()) return Status::OutOfMemory;
    Status s = fs_.nfs_call(*server_, kSetattr, args.view(), &reply);
    if (s != Status::Ok) return s;
    XdrReader r(reply.view());
    const uint32_t status = r.u32();
    if (status != 0) return map_status(status);
    size_ = new_size;
    return Status::Ok;
  }
  Status flush() override { return Status::Ok; }   // (writes are synchronous)

 private:
  NfsFileSystem& fs_;
  SharedPointer<Server> server_;
  FileHandle handle_;
  uint64_t size_;
  bool writable_;
};

// A folder read from the server in batches (READDIRPLUS).
class NfsFileSystem::Directory final : public IDirectory {
 public:
  Directory(NfsFileSystem& fs, SharedPointer<Server> server, const FileHandle& handle) : fs_(fs), server_(util::move(server)), handle_(handle), batch_(fs.allocator_) {}
  Status next(DirectoryEntry* entry) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    for (;;) {
      if (index_ < batch_.count()) {
        *entry = batch_[index_++];
        return Status::Ok;
      }
      if (eof_) return Status::EndOfDirectory;
      const Status s = fetch();
      if (s != Status::Ok) return s;
    }
  }
  void rewind() override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    batch_.clear();
    index_ = 0;
    eof_ = false;
    cookie_ = 0;
    for (uint8_t& b : verifier_) b = 0;
  }

 private:
  Status fetch() {
    batch_.clear();
    index_ = 0;
    ByteBuffer args(fs_.allocator_), reply(fs_.allocator_);
    XdrWriter w(&args);
    put_handle(w, handle_.bytes, handle_.length);
    w.u64(cookie_);
    w.fixed(ConstByteView(verifier_, 8));
    w.u32(8192);    // how many bytes of names
    w.u32(32768);   // and of everything
    if (!w.ok()) return Status::OutOfMemory;
    Status s = fs_.nfs_call(*server_, kReaddirplus, args.view(), &reply);
    if (s != Status::Ok) return s;
    XdrReader r(reply.view());
    const uint32_t status = r.u32();
    Fattr dir;
    read_post_op_attr(r, &dir);
    if (status != 0) return map_status(status);
    const ConstByteView verifier = r.fixed(8);
    if (verifier.size() == 8) __builtin_memcpy(verifier_, verifier.data(), 8);
    while (r.ok() && r.boolean()) {
      r.u64();   // file id
      const StringView name = r.string(1024);
      cookie_ = r.u64();
      Fattr a;
      const bool has_attributes = read_post_op_attr(r, &a);
      if (r.boolean()) r.opaque(kMaxFileHandle);   // the handle (not needed: lookups get their own)
      if (!r.ok()) break;
      if (name.equals(".") || name.equals("..")) continue;
      DirectoryEntry e;
      if (!e.name.assign(name)) continue;   // (too long for a name here)
      e.type = has_attributes && a.type == 2 ? NodeType::Directory : NodeType::File;
      e.size = e.type == NodeType::File && has_attributes ? a.size : 0;
      e.read_only = has_attributes && (a.mode & 0222) == 0;
      if (!batch_.push_back(e)) return Status::OutOfMemory;
    }
    eof_ = r.boolean();
    if (!r.ok()) return Status::Network;
    if (batch_.count() == 0 && !eof_) return Status::Network;   // (no progress: the server would repeat itself forever)
    return Status::Ok;
  }

  NfsFileSystem& fs_;
  SharedPointer<Server> server_;
  FileHandle handle_;
  List<DirectoryEntry> batch_;
  uint64_t index_ = 0, cookie_ = 0;
  uint8_t verifier_[8] = {};
  bool eof_ = false;
};

class NfsFileSystem::ListDirectory final : public IDirectory {
 public:
  explicit ListDirectory(allocators::IAllocator& allocator) : entries_(allocator) {}
  bool add(StringView name) {
    for (const DirectoryEntry& e : entries_.view())
      if (e.name.view().equals(name)) return true;
    DirectoryEntry e;
    if (!e.name.assign(name)) return true;
    e.type = NodeType::Directory;
    return entries_.push_back(e);
  }
  Status next(DirectoryEntry* entry) override {
    if (index_ >= entries_.count()) return Status::EndOfDirectory;
    *entry = entries_[index_++];
    return Status::Ok;
  }
  void rewind() override { index_ = 0; }

 private:
  List<DirectoryEntry> entries_;
  uint64_t index_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------------------------- servers

NfsFileSystem::NfsFileSystem(IServerNetwork& network, allocators::IAllocator& allocator) : network_(network), allocator_(allocator), servers_(allocator) {}

NfsFileSystem::~NfsFileSystem() {
  kernel::ScopedRecursiveMutex lock(mutex_);
  for (SharedPointer<Server>& s : servers_.view()) drop_connection(*s);
}

FileSystemInfo NfsFileSystem::info() const {
  FileSystemInfo result;
  result.type_name.assign("nfs");
  return result;
}

SharedPointer<NfsFileSystem::Server> NfsFileSystem::find_server(StringView host, bool create) {
  for (SharedPointer<Server>& s : servers_.view())
    if (s->host.view().equals_ignore_case(host)) return s;
  if (!create) return SharedPointer<Server>();
  SharedPointer<Server> server = SharedPointer<Server>::make(allocator_, allocator_);
  if (!server || !server->host.assign(host)) return SharedPointer<Server>();
  StringView name = host;
  const uint64_t colon = host.rfind(':');   // "name:port": the port of the port mapper
  if (colon != StringView::npos && colon > 0 && colon + 1 < host.size() && host.size() - colon <= 6) {
    uint32_t value = 0;
    bool digits = true;
    for (char c : host.substr(colon + 1)) {
      digits = digits && c >= '0' && c <= '9';
      value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    if (digits && value > 0 && value < 65536) {
      name = host.first(colon);
      server->port = static_cast<uint16_t>(value);
    }
  }
  if (!server->name.assign(name) || !servers_.push_back(server)) return SharedPointer<Server>();
  return server;
}

void NfsFileSystem::drop_connection(Server& server) { server.rpc.reset(); }

void NfsFileSystem::set_credentials(StringView host, uint32_t uid, uint32_t gid) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  SharedPointer<Server> server = find_server(host, true);
  if (!server) return;
  server->uid = uid;
  server->gid = gid;
  drop_connection(*server);
  server->mounts.clear();   // (mounted again with the new ids)
}

void NfsFileSystem::forget(StringView host) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  for (uint64_t i = 0; i < servers_.count(); ++i)
    if (servers_[i]->host.view().equals_ignore_case(host)) {
      drop_connection(*servers_[i]);
      servers_.remove_at(i);
      return;
    }
}

// Asks the port mapper where `program` version `version` listens (TCP).
Status NfsFileSystem::port_of(Server& server, uint32_t program, uint32_t version, uint16_t* port) {
  SharedPointer<ITcpSocket> socket;
  INetworkStack* stack = nullptr;
  Status s = network_.connect(server.name.view(), server.port, 10000, &socket, &stack);
  if (s != Status::Ok) return s;
  RpcClient rpc(*stack, util::move(socket), allocator_);
  ByteBuffer args(allocator_), reply(allocator_);
  XdrWriter w(&args);
  w.u32(program);
  w.u32(version);
  w.u32(6);   // TCP
  w.u32(0);
  s = rpc.call(kPortmapper, 2, 3, args.view(), RpcAuth(), &reply);
  if (s != Status::Ok) return s;
  XdrReader r(reply.view());
  const uint32_t found = r.u32();
  if (!r.ok()) return Status::Network;
  if (found == 0 || found > 65535) return Status::Unsupported;   // not offered by this server
  *port = static_cast<uint16_t>(found);
  return Status::Ok;
}

Status NfsFileSystem::connect_service(Server& server, uint32_t program, uint32_t version, UniquePointer<RpcClient>* out) {
  uint16_t port = 0;
  Status s = port_of(server, program, version, &port);
  if (s == Status::Network && program == kNfs) {   // no port mapper (a firewall in the way): the usual port of the NFS service
    port = 2049;
    s = Status::Ok;
  }
  if (s != Status::Ok) return s;
  SharedPointer<ITcpSocket> socket;
  INetworkStack* stack = nullptr;
  s = network_.connect(server.name.view(), port, 10000, &socket, &stack);
  if (s != Status::Ok) return s;
  UniquePointer<RpcClient> rpc = UniquePointer<RpcClient>::make(allocator_, *stack, util::move(socket), allocator_);
  if (!rpc) return Status::OutOfMemory;
  *out = util::move(rpc);
  return Status::Ok;
}

Status NfsFileSystem::ensure_exports(Server& server) {
  if (server.exports_loaded) return Status::Ok;
  UniquePointer<RpcClient> mountd;
  Status s = connect_service(server, kMountd, 3, &mountd);
  if (s != Status::Ok) return s;
  ByteBuffer reply(allocator_);
  s = mountd->call(kMountd, 3, 5 /* EXPORT */, ConstByteView(), sys_auth(server.uid, server.gid), &reply);
  if (s != Status::Ok) return s;
  XdrReader r(reply.view());
  server.exports.clear();
  while (r.ok() && r.boolean()) {
    String path(allocator_);
    path.assign(r.string(1024));
    while (r.ok() && r.boolean()) r.string(256);   // the groups it is exported to
    if (r.ok() && !path.empty() && !server.exports.push_back(util::move(path))) return Status::OutOfMemory;
  }
  if (!r.ok()) {
    server.exports.clear();
    return Status::Network;
  }
  server.exports_loaded = true;
  return Status::Ok;
}

Status NfsFileSystem::list_exports(StringView host, List<String>* paths) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  SharedPointer<Server> server = find_server(host, true);
  if (!server) return Status::OutOfMemory;
  server->exports_loaded = false;   // (asked again: they change)
  const Status s = ensure_exports(*server);
  if (s != Status::Ok) return s;
  for (const String& e : server->exports.view()) {
    String copy(allocator_);
    if (!copy.assign(e.view()) || !paths->push_back(util::move(copy))) return Status::OutOfMemory;
  }
  return Status::Ok;
}

// Mounts an export: its root file handle, from the MOUNT service.
Status NfsFileSystem::mount(Server& server, StringView export_path, uint32_t* index) {
  for (uint64_t i = 0; i < server.mounts.count(); ++i)
    if (server.mounts[i].path.view().equals(export_path)) {
      *index = static_cast<uint32_t>(i);
      return Status::Ok;
    }
  UniquePointer<RpcClient> mountd;
  Status s = connect_service(server, kMountd, 3, &mountd);
  if (s != Status::Ok) return s;
  ByteBuffer args(allocator_), reply(allocator_);
  XdrWriter w(&args);
  w.string(export_path);
  s = mountd->call(kMountd, 3, 1 /* MNT */, args.view(), sys_auth(server.uid, server.gid), &reply);
  if (s != Status::Ok) return s;
  XdrReader r(reply.view());
  const uint32_t status = r.u32();
  if (!r.ok()) return Status::Network;
  if (status != 0) return status == 13 || status == 1 ? Status::AccessDenied : status == 2 ? Status::NotFound : status == 20 ? Status::NotADirectory : Status::IoError;
  const ConstByteView handle = r.opaque(kMaxFileHandle);
  if (!r.ok() || handle.empty()) return Status::Network;
  Mount m(allocator_);
  m.path.assign(export_path);
  m.root.length = static_cast<uint32_t>(handle.size());
  __builtin_memcpy(m.root.bytes, handle.data(), handle.size());
  if (!server.mounts.push_back(util::move(m))) return Status::OutOfMemory;
  *index = static_cast<uint32_t>(server.mounts.count() - 1);
  // How much the server likes to move at a time.
  ByteBuffer info_args(allocator_), info_reply(allocator_);
  XdrWriter iw(&info_args);
  put_handle(iw, server.mounts[*index].root.bytes, server.mounts[*index].root.length);
  if (iw.ok() && nfs_call(server, kFsinfo, info_args.view(), &info_reply) == Status::Ok) {
    XdrReader fr(info_reply.view());
    if (fr.u32() == 0) {
      Fattr ignored;
      read_post_op_attr(fr, &ignored);
      fr.u32();
      const uint32_t read_preferred = fr.u32();
      fr.u32();
      fr.u32();
      const uint32_t write_preferred = fr.u32();
      if (fr.ok()) {
        server.read_size = read_preferred >= 512 && read_preferred < 32768 ? read_preferred : 32768;
        server.write_size = write_preferred >= 512 && write_preferred < 32768 ? write_preferred : 32768;
      }
    }
  }
  return Status::Ok;
}

// A call to the NFS service; if the connection had died, once more over a new one.
Status NfsFileSystem::nfs_call(Server& server, uint32_t procedure, ConstByteView args, ByteBuffer* results) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!server.rpc || !server.rpc->alive()) {
      const Status s = connect_service(server, kNfs, 3, &server.rpc);
      if (s != Status::Ok) return s;
    }
    results->clear();
    const Status s = server.rpc->call(kNfs, 3, procedure, args, sys_auth(server.uid, server.gid), results);
    if (s == Status::Network && attempt == 0) {
      drop_connection(server);
      continue;
    }
    return s;
  }
  return Status::Network;
}

Status NfsFileSystem::lookup(Server& server, const FileHandle& dir, StringView name, FileHandle* out, Attributes* attributes) {
  ByteBuffer args(allocator_), reply(allocator_);
  XdrWriter w(&args);
  put_handle(w, dir.bytes, dir.length);
  w.string(name);
  if (!w.ok()) return Status::OutOfMemory;
  Status s = nfs_call(server, kLookup, args.view(), &reply);
  if (s != Status::Ok) return s;
  XdrReader r(reply.view());
  const uint32_t status = r.u32();
  if (status != 0) return map_status(status);
  const ConstByteView handle = r.opaque(kMaxFileHandle);
  Fattr a;
  const bool has = read_post_op_attr(r, &a);
  if (!r.ok() || handle.empty()) return Status::Network;
  out->length = static_cast<uint32_t>(handle.size());
  __builtin_memcpy(out->bytes, handle.data(), handle.size());
  if (attributes) {
    attributes->valid = has;
    attributes->type = has ? a.type : 0;
    attributes->mode = has ? a.mode : 0;
    attributes->size = has ? a.size : 0;
  }
  return Status::Ok;
}

Status NfsFileSystem::getattr(Server& server, const FileHandle& handle, Attributes* attributes) {
  ByteBuffer args(allocator_), reply(allocator_);
  XdrWriter w(&args);
  put_handle(w, handle.bytes, handle.length);
  if (!w.ok()) return Status::OutOfMemory;
  Status s = nfs_call(server, kGetattr, args.view(), &reply);
  if (s != Status::Ok) return s;
  XdrReader r(reply.view());
  const uint32_t status = r.u32();
  if (status != 0) return map_status(status);
  Fattr a;
  if (!read_fattr(r, &a)) return Status::Network;
  attributes->valid = true;
  attributes->type = a.type;
  attributes->mode = a.mode;
  attributes->size = a.size;
  return Status::Ok;
}

// Looks up parts[first .. last) one after the other from `from`.
Status NfsFileSystem::walk(Server& server, const FileHandle& from, const Parsed& where, uint32_t first, uint32_t last, FileHandle* out, Attributes* attributes) {
  FileHandle current = from;
  Attributes a;
  if (first == last) {
    Status s = getattr(server, current, &a);
    if (s != Status::Ok) return s;
  }
  for (uint32_t i = first; i < last; ++i) {
    FileHandle next;
    Status s = lookup(server, current, where.parts[i].view(), &next, &a);
    if (s != Status::Ok) return s;
    if (!a.valid) {
      s = getattr(server, next, &a);
      if (s != Status::Ok) return s;
    }
    current = next;
  }
  *out = current;
  if (attributes) *attributes = a;
  return Status::Ok;
}

bool NfsFileSystem::parse(StringView path, Parsed* out) const {
  StringView rest = path, component;
  uint32_t depth = 0;
  while (next_component(&rest, &component)) {
    if (is_dot(component)) continue;
    if (component.equals("..") || component.size() > 255) return false;
    if (depth == 0) {
      if (!out->server.assign(component)) return false;
    } else {
      String part(allocator_);
      if (!part.assign(component) || !out->parts.push_back(util::move(part))) return false;
    }
    ++depth;
  }
  return true;
}

// Works out what a path names: the root, a server, a folder made of export paths, or a node in an export (with its file handle).
Status NfsFileSystem::resolve(const Parsed& where, bool want_attributes, Located* out, uint32_t* tail_from) {
  (void)want_attributes;
  if (where.server.empty()) {
    out->depth = 0;
    return Status::Ok;
  }
  out->server = find_server(where.server.view(), true);
  if (!out->server) return Status::OutOfMemory;
  Server& server = *out->server;
  if (where.parts.count() == 0) {
    out->depth = 1;
    return Status::Ok;
  }
  out->depth = 2;
  Status s = ensure_exports(server);
  if (s != Status::Ok) return s;
  const uint32_t n = static_cast<uint32_t>(where.parts.count());
  // The longest prefix of the path that is an export.
  for (int64_t k = n; k >= 0; --k) {
    String prefix(allocator_);
    if (!prefix.push_back('/')) return Status::OutOfMemory;
    for (int64_t i = 0; i < k; ++i) {
      if (i > 0 && !prefix.push_back('/')) return Status::OutOfMemory;
      if (!prefix.append(where.parts[static_cast<uint64_t>(i)].view())) return Status::OutOfMemory;
    }
    bool is_export = false;
    for (const String& e : server.exports.view()) {
      StringView ev = e.view();
      while (ev.size() > 1 && ev[ev.size() - 1] == '/') ev = ev.first(ev.size() - 1);
      is_export = is_export || ev.equals(prefix.view());
    }
    if (!is_export) continue;
    uint32_t index = 0;
    s = mount(server, prefix.view(), &index);
    if (s != Status::Ok) return s;
    out->in_export = true;
    out->mount = index;
    if (tail_from) *tail_from = static_cast<uint32_t>(k);
    return walk(server, server.mounts[index].root, where, static_cast<uint32_t>(k), n, &out->handle, &out->attributes);
  }
  // Not in an export: a folder on the way to some.
  String prefix(allocator_);
  for (uint32_t i = 0; i < n; ++i)
    if (!prefix.push_back('/') || !prefix.append(where.parts[i].view())) return Status::OutOfMemory;
  for (const String& e : server.exports.view())
    if (e.view().starts_with(prefix.view()) && e.size() > prefix.size() && e.view()[prefix.size()] == '/') {
      out->in_export = false;
      out->exports_prefix = util::move(prefix);
      return Status::Ok;
    }
  return Status::NotFound;
}

// ---------------------------------------------------------------------------------------------------------------------------------------- operations

Status NfsFileSystem::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  Located at(allocator_);
  Status s = resolve(where, true, &at);
  if (s != Status::Ok) return s;
  if (at.depth == 0) {
    UniquePointer<ListDirectory> list = UniquePointer<ListDirectory>::make(allocator_, allocator_);
    if (!list) return Status::OutOfMemory;
    for (const SharedPointer<Server>& v : servers_.view())
      if (!list->add(v->host.view())) return Status::OutOfMemory;
    *directory = UniquePointer<IDirectory>(util::move(list));
    return Status::Ok;
  }
  if (at.depth == 1 || !at.in_export) {   // the next components of the export paths below here
    const StringView prefix = at.depth == 1 ? StringView() : at.exports_prefix.view();
    if (at.depth == 1) {
      s = ensure_exports(*at.server);
      if (s != Status::Ok) return s;
    }
    UniquePointer<ListDirectory> list = UniquePointer<ListDirectory>::make(allocator_, allocator_);
    if (!list) return Status::OutOfMemory;
    for (const String& e : at.server->exports.view()) {
      if (!e.view().starts_with(prefix) || e.size() <= prefix.size()) continue;
      StringView rest = e.view().substr(prefix.size());
      if (rest[0] != '/') continue;
      rest = rest.substr(1);
      const uint64_t slash = rest.find('/');
      const StringView name = slash == StringView::npos ? rest : rest.first(slash);
      if (!name.empty() && !list->add(name)) return Status::OutOfMemory;
    }
    *directory = UniquePointer<IDirectory>(util::move(list));
    return Status::Ok;
  }
  if (!at.attributes.directory()) return Status::NotADirectory;
  UniquePointer<IDirectory> handle = UniquePointer<IDirectory>::make_as<Directory>(allocator_, *this, at.server, at.handle);
  if (!handle) return Status::OutOfMemory;
  *directory = util::move(handle);
  return Status::Ok;
}

Status NfsFileSystem::stat(StringView path, DirectoryEntry* entry) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  Located at(allocator_);
  const Status s = resolve(where, true, &at);
  if (s != Status::Ok) return s;
  *entry = DirectoryEntry();
  entry->type = NodeType::Directory;
  if (at.depth == 0) return Status::Ok;
  if (at.depth == 1) {
    entry->name.assign(where.server.view());
    return Status::Ok;
  }
  entry->name.assign(where.parts[where.parts.count() - 1].view());
  if (!at.in_export) return Status::Ok;
  entry->type = at.attributes.directory() ? NodeType::Directory : NodeType::File;
  entry->size = at.attributes.directory() ? 0 : at.attributes.size;
  entry->read_only = (at.attributes.mode & 0222) == 0;
  return Status::Ok;
}

Status NfsFileSystem::create_in(Server& server, const FileHandle& dir, StringView name, bool directory, bool exclusive, FileHandle* out) {
  ByteBuffer args(allocator_), reply(allocator_);
  XdrWriter w(&args);
  put_handle(w, dir.bytes, dir.length);
  w.string(name);
  if (directory) {
    write_sattr(w, true, 0755, false, 0);
  } else {
    w.u32(exclusive ? 1 : 0);   // GUARDED fails if the name has appeared meanwhile; UNCHECKED does not
    write_sattr(w, true, 0644, false, 0);
  }
  if (!w.ok()) return Status::OutOfMemory;
  Status s = nfs_call(server, directory ? kMkdir : kCreate, args.view(), &reply);
  if (s != Status::Ok) return s;
  XdrReader r(reply.view());
  const uint32_t status = r.u32();
  if (status != 0) return map_status(status);
  FileHandle handle;
  if (r.boolean()) {
    const ConstByteView h = r.opaque(kMaxFileHandle);
    if (r.ok() && !h.empty()) {
      handle.length = static_cast<uint32_t>(h.size());
      __builtin_memcpy(handle.bytes, h.data(), h.size());
    }
  }
  if (handle.length == 0) return lookup(server, dir, name, out, nullptr);   // (the server did not return the handle)
  *out = handle;
  return Status::Ok;
}

Status NfsFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  Located at(allocator_);
  Status s = resolve(where, true, &at);
  const bool write = mode != OpenMode::Read;
  if (s == Status::NotFound && (mode == OpenMode::CreateOrOpen || mode == OpenMode::CreateTruncate) && where.parts.count() > 0) {
    // Make it in its folder.
    Parsed parent(allocator_);
    parent.server.assign(where.server.view());
    for (uint64_t i = 0; i + 1 < where.parts.count(); ++i) {
      String part(allocator_);
      part.assign(where.parts[i].view());
      parent.parts.push_back(util::move(part));
    }
    Located in(allocator_);
    s = resolve(parent, true, &in);
    if (s != Status::Ok) return s;
    if (!in.in_export) return Status::Unsupported;   // (no files among the export paths)
    if (!in.attributes.directory()) return Status::NotADirectory;
    FileHandle created;
    s = create_in(*in.server, in.handle, where.parts[where.parts.count() - 1].view(), false, true, &created);
    if (s != Status::Ok) return s;
    at.server = in.server;
    at.handle = created;
    at.in_export = true;
    at.depth = 2;
    at.attributes = Attributes();
    at.attributes.valid = true;
    at.attributes.type = 1;
    s = Status::Ok;
  } else if (s == Status::Ok) {
    if (at.depth < 2 || !at.in_export) return Status::IsADirectory;
    if (at.attributes.directory()) return Status::IsADirectory;
    if (mode == OpenMode::CreateTruncate && at.attributes.size != 0) {
      ByteBuffer args(allocator_), reply(allocator_);
      XdrWriter w(&args);
      put_handle(w, at.handle.bytes, at.handle.length);
      write_sattr(w, false, 0, true, 0);
      w.boolean(false);
      if (!w.ok()) return Status::OutOfMemory;
      s = nfs_call(*at.server, kSetattr, args.view(), &reply);
      if (s == Status::Ok) {
        XdrReader r(reply.view());
        const uint32_t status = r.u32();
        if (status != 0) s = map_status(status);
      }
      at.attributes.size = 0;
    }
  }
  if (s != Status::Ok) return s;
  UniquePointer<IFile> handle = UniquePointer<IFile>::make_as<File>(allocator_, *this, at.server, at.handle, at.attributes.size, write);
  if (!handle) return Status::OutOfMemory;
  *file = util::move(handle);
  return Status::Ok;
}

Status NfsFileSystem::create_directory(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  if (where.parts.count() == 0) return where.server.empty() ? Status::AlreadyExists : Status::Unsupported;
  Located existing(allocator_);
  Status s = resolve(where, true, &existing);
  if (s == Status::Ok) return Status::AlreadyExists;
  if (s != Status::NotFound) return s;
  Parsed parent(allocator_);
  parent.server.assign(where.server.view());
  for (uint64_t i = 0; i + 1 < where.parts.count(); ++i) {
    String part(allocator_);
    part.assign(where.parts[i].view());
    parent.parts.push_back(util::move(part));
  }
  Located in(allocator_);
  s = resolve(parent, true, &in);
  if (s != Status::Ok) return s;
  if (!in.in_export) return Status::Unsupported;
  FileHandle created;
  return create_in(*in.server, in.handle, where.parts[where.parts.count() - 1].view(), true, true, &created);
}

Status NfsFileSystem::remove(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  if (where.parts.count() == 0) return Status::Unsupported;
  Located target(allocator_);
  Status s = resolve(where, true, &target);
  if (s != Status::Ok) return s;
  if (!target.in_export) return Status::Unsupported;
  Parsed parent(allocator_);
  parent.server.assign(where.server.view());
  for (uint64_t i = 0; i + 1 < where.parts.count(); ++i) {
    String part(allocator_);
    part.assign(where.parts[i].view());
    parent.parts.push_back(util::move(part));
  }
  Located in(allocator_);
  s = resolve(parent, true, &in);
  if (s != Status::Ok) return s;
  if (!in.in_export) return Status::Unsupported;   // (the root of an export is not removed from here)
  ByteBuffer args(allocator_), reply(allocator_);
  XdrWriter w(&args);
  put_handle(w, in.handle.bytes, in.handle.length);
  w.string(where.parts[where.parts.count() - 1].view());
  if (!w.ok()) return Status::OutOfMemory;
  s = nfs_call(*in.server, target.attributes.directory() ? kRmdir : kRemove, args.view(), &reply);
  if (s != Status::Ok) return s;
  XdrReader r(reply.view());
  return map_status(r.u32());
}

Status NfsFileSystem::rename(StringView from, StringView to) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed source(allocator_), target(allocator_);
  if (!parse(from, &source) || !parse(to, &target)) return Status::InvalidPath;
  if (source.parts.count() == 0 || target.parts.count() == 0) return Status::Unsupported;
  if (!source.server.view().equals_ignore_case(target.server.view())) return Status::Unsupported;
  Located existing(allocator_);
  Status s = resolve(target, true, &existing);
  if (s == Status::Ok) return Status::AlreadyExists;   // (NFS would replace it)
  if (s != Status::NotFound) return s;
  Located what(allocator_);
  s = resolve(source, true, &what);
  if (s != Status::Ok) return s;
  auto parent_of = [&](const Parsed& p, Parsed* out) {
    out->server.assign(p.server.view());
    for (uint64_t i = 0; i + 1 < p.parts.count(); ++i) {
      String part(allocator_);
      part.assign(p.parts[i].view());
      out->parts.push_back(util::move(part));
    }
  };
  Parsed source_parent(allocator_), target_parent(allocator_);
  parent_of(source, &source_parent);
  parent_of(target, &target_parent);
  Located from_dir(allocator_), to_dir(allocator_);
  s = resolve(source_parent, true, &from_dir);
  if (s != Status::Ok) return s;
  s = resolve(target_parent, true, &to_dir);
  if (s != Status::Ok) return s;
  if (!from_dir.in_export || !to_dir.in_export || from_dir.mount != to_dir.mount) return Status::Unsupported;   // between exports is a copy
  ByteBuffer args(allocator_), reply(allocator_);
  XdrWriter w(&args);
  put_handle(w, from_dir.handle.bytes, from_dir.handle.length);
  w.string(source.parts[source.parts.count() - 1].view());
  put_handle(w, to_dir.handle.bytes, to_dir.handle.length);
  w.string(target.parts[target.parts.count() - 1].view());
  if (!w.ok()) return Status::OutOfMemory;
  s = nfs_call(*from_dir.server, kRename, args.view(), &reply);
  if (s != Status::Ok) return s;
  XdrReader r(reply.view());
  return map_status(r.u32());
}

}  // namespace network
