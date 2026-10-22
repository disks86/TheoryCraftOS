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

#include "Network/Sftp/SftpFileSystem.hpp"
#include "FileSystem/Path.hpp"
#include "Network/HostPort.hpp"
#include "Utility/Move.hpp"

namespace network {

using namespace filesystem;

namespace {

constexpr uint8_t kInit = 1, kVersion = 2, kOpen = 3, kClose = 4, kRead = 5, kWrite = 6, kLstat = 7, kFstat = 8, kFsetstat = 10, kOpendir = 11, kReaddir = 12,
                  kRemove = 13, kMkdir = 14, kRmdir = 15, kStat = 17, kRename = 18, kStatus = 101, kHandle = 102, kData = 103, kName = 104, kAttrs = 105;
constexpr uint32_t kFlagRead = 1, kFlagWrite = 2, kCreate = 8, kTruncate = 0x10;
constexpr uint32_t kChunk = 32768, kDepth = 8, kMaxReply = 1u << 20;
constexpr uint32_t kUnixTypeMask = 0170000, kUnixLink = 0120000;

// Writes an ATTRS structure that sets only the permissions.
void write_permissions(SshWriter& w, uint32_t permissions) {
  w.u32(4);
  w.u32(permissions);
}

}  // namespace

Status SftpFileSystem::map_status(uint32_t code) {
  switch (code) {
    case 0: return Status::Ok;
    case 1: return Status::EndOfDirectory;
    case 2: return Status::NotFound;
    case 3: return Status::AccessDenied;
    case 5: return Status::InvalidArgument;
    case 6: case 7: return Status::Network;
    case 8: return Status::Unsupported;
    default: return Status::IoError;
  }
}

Status SftpFileSystem::status_of(uint8_t reply_type, ConstByteView reply) {
  if (reply_type != kStatus) return Status::IoError;
  SshReader r(reply);
  const uint32_t code = r.u32();
  return r.ok() ? map_status(code) : Status::Network;
}

bool SftpFileSystem::read_attributes(SshReader& r, Attributes* out) {
  out->flags = r.u32();
  if (out->flags & 1) out->size = r.u64();
  if (out->flags & 2) {
    r.u32();
    r.u32();
  }
  if (out->flags & 4) out->permissions = r.u32();
  if (out->flags & 8) {
    r.u32();
    r.u32();
  }
  if (out->flags & 0x80000000u) {
    const uint32_t count = r.u32();
    for (uint32_t i = 0; i < count && r.ok(); ++i) {
      r.string(1u << 16);
      r.string(1u << 16);
    }
  }
  return r.ok();
}

// ---------------------------------------------------------------------------------------------------------------------------------------- handles

class SftpFileSystem::File final : public IFile {
 public:
  File(SftpFileSystem& fs, SharedPointer<Server> server, ConstByteView handle, uint64_t size, bool writable)
      : fs_(fs), server_(util::move(server)), generation_(server_->generation), handle_(fs.allocator_), size_(size), writable_(writable) {
    handle_.append(handle);
  }
  ~File() {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (usable()) fs_.close_handle(*server_, handle_.view());
  }
  uint64_t size() const override { return size_; }

  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    if (!usable()) return Status::Network;
    uint64_t done = 0;
    while (done < buffer.size()) {
      // Up to kDepth requests go out before the first answer is read.
      uint32_t ids[kDepth], lengths[kDepth];
      uint32_t count = 0;
      uint64_t position = done;
      while (count < kDepth && position < buffer.size()) {
        const uint32_t length = static_cast<uint32_t>(buffer.size() - position < kChunk ? buffer.size() - position : kChunk);
        ByteBuffer body(fs_.allocator_);
        SshWriter w(&body);
        w.string(handle_.view());
        w.u64(offset + position);
        w.u32(length);
        if (!w.ok()) return Status::OutOfMemory;
        ids[count] = server_->next_id++;
        lengths[count] = length;
        const Status s = fs_.send_request(*server_, kRead, ids[count], body.view());
        if (s != Status::Ok) {
          *transferred = done;
          return s;
        }
        position += length;
        ++count;
      }
      uint64_t valid = done;    // the buffer is filled up to here
      bool stop = false;        // after a short answer or the end the rest of the answers are only read off
      bool end = false;
      Status result = Status::Ok;
      uint64_t chunk_start = done;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t id = 0;
        uint8_t type = 0;
        ByteBuffer reply(fs_.allocator_);
        const Status s = fs_.read_reply(*server_, &id, &type, &reply);
        if (s != Status::Ok) {
          *transferred = valid;
          return s;
        }
        if (id != ids[i]) {
          fs_.drop_connection(*server_);
          *transferred = valid;
          return Status::Network;
        }
        if (!stop) {
          if (type == kData) {
            SshReader r(reply.view());
            const ConstByteView data = r.string(kChunk);
            if (!r.ok() || data.size() > lengths[i]) {
              fs_.drop_connection(*server_);
              *transferred = valid;
              return Status::Network;
            }
            buffer.subview(chunk_start, data.size()).copy_from(data);
            valid = chunk_start + data.size();
            if (data.size() < lengths[i]) stop = true;
            if (data.empty()) end = true;
          } else {
            const Status st = status_of(type, reply.view());
            stop = true;
            if (st == Status::EndOfDirectory) end = true;
            else result = st == Status::Ok ? Status::IoError : st;
          }
        }
        chunk_start += lengths[i];
      }
      done = valid;
      if (result != Status::Ok) {
        *transferred = done;
        return result;
      }
      if (end) break;
    }
    *transferred = done;
    return Status::Ok;
  }

  Status write(uint64_t offset, ConstByteView data, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    if (!writable_) return Status::ReadOnly;
    if (!usable()) return Status::Network;
    uint64_t done = 0;
    while (done < data.size()) {
      uint32_t ids[kDepth], lengths[kDepth];
      uint32_t count = 0;
      uint64_t position = done;
      while (count < kDepth && position < data.size()) {
        const uint32_t length = static_cast<uint32_t>(data.size() - position < kChunk ? data.size() - position : kChunk);
        ByteBuffer body(fs_.allocator_);
        SshWriter w(&body);
        w.string(handle_.view());
        w.u64(offset + position);
        w.string(data.subview(position, length));
        if (!w.ok()) return Status::OutOfMemory;
        ids[count] = server_->next_id++;
        lengths[count] = length;
        const Status s = fs_.send_request(*server_, kWrite, ids[count], body.view());
        if (s != Status::Ok) {
          *transferred = done;
          return s;
        }
        position += length;
        ++count;
      }
      Status result = Status::Ok;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t id = 0;
        uint8_t type = 0;
        ByteBuffer reply(fs_.allocator_);
        const Status s = fs_.read_reply(*server_, &id, &type, &reply);
        if (s != Status::Ok) {
          *transferred = done;
          return s;
        }
        if (id != ids[i]) {
          fs_.drop_connection(*server_);
          *transferred = done;
          return Status::Network;
        }
        if (result == Status::Ok) {
          const Status st = status_of(type, reply.view());
          if (st != Status::Ok) result = st;
          else done += lengths[i];
        }
      }
      if (offset + done > size_) size_ = offset + done;
      if (result != Status::Ok) {
        *transferred = done;
        return result;
      }
    }
    *transferred = done;
    return Status::Ok;
  }

  Status resize(uint64_t new_size) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (!writable_) return Status::ReadOnly;
    if (!usable()) return Status::Network;
    ByteBuffer body(fs_.allocator_), reply(fs_.allocator_);
    SshWriter w(&body);
    w.string(handle_.view());
    w.u32(1);   // attributes: the size
    w.u64(new_size);
    if (!w.ok()) return Status::OutOfMemory;
    uint8_t type = 0;
    Status s = fs_.call(*server_, kFsetstat, body.view(), &type, &reply, false);
    if (s != Status::Ok) return s;
    s = status_of(type, reply.view());
    if (s == Status::Ok) size_ = new_size;
    return s;
  }
  Status flush() override { return Status::Ok; }

 private:
  bool usable() const { return server_->ssh && server_->ssh->alive() && server_->generation == generation_; }

  SftpFileSystem& fs_;
  SharedPointer<Server> server_;
  uint32_t generation_;
  ByteBuffer handle_;
  uint64_t size_;
  bool writable_;
};

// A folder read from the server in batches (READDIR).
class SftpFileSystem::Directory final : public IDirectory {
 public:
  Directory(SftpFileSystem& fs, SharedPointer<Server> server, ConstByteView handle, StringView path)
      : fs_(fs), server_(util::move(server)), generation_(server_->generation), handle_(fs.allocator_), path_(fs.allocator_), batch_(fs.allocator_) {
    handle_.append(handle);
    path_.assign(path);
  }
  ~Directory() {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (usable()) fs_.close_handle(*server_, handle_.view());
  }
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
    // (the handle cannot be wound back: a new one is opened)
    if (usable()) fs_.close_handle(*server_, handle_.view());
    batch_.clear();
    index_ = 0;
    eof_ = true;
    ByteBuffer body(fs_.allocator_), reply(fs_.allocator_);
    SshWriter w(&body);
    w.string(path_.view());
    uint8_t type = 0;
    if (!w.ok() || fs_.call(*server_, kOpendir, body.view(), &type, &reply) != Status::Ok || type != kHandle) return;
    SshReader r(reply.view());
    const ConstByteView handle = r.string(256);
    if (!r.ok()) return;
    handle_.clear();
    handle_.append(handle);
    generation_ = server_->generation;
    eof_ = false;
  }

 private:
  bool usable() const { return server_->ssh && server_->ssh->alive() && server_->generation == generation_; }

  Status fetch() {
    batch_.clear();
    index_ = 0;
    if (!usable()) return Status::Network;
    ByteBuffer body(fs_.allocator_), reply(fs_.allocator_);
    SshWriter w(&body);
    w.string(handle_.view());
    if (!w.ok()) return Status::OutOfMemory;
    uint8_t type = 0;
    Status s = fs_.call(*server_, kReaddir, body.view(), &type, &reply, false);
    if (s != Status::Ok) return s;
    if (type == kStatus) {
      s = status_of(type, reply.view());
      if (s == Status::EndOfDirectory) {
        eof_ = true;
        return Status::Ok;
      }
      return s == Status::Ok ? Status::IoError : s;
    }
    if (type != kName) return Status::Network;
    SshReader r(reply.view());
    const uint32_t count = r.u32();
    for (uint32_t i = 0; i < count && r.ok(); ++i) {
      const StringView name = r.text(1024);
      r.text(4096);   // (the "ls -l" line)
      Attributes a;
      if (!read_attributes(r, &a)) break;
      if (name.equals(".") || name.equals("..")) continue;
      DirectoryEntry e;
      if (!e.name.assign(name)) continue;   // (too long for a name here)
      bool directory = a.directory();
      if (a.link()) {   // a link is what it points to
        String target(fs_.allocator_);
        Attributes t;
        if (target.assign(path_.view()) && (path_.view().ends_with('/') || target.push_back('/')) && target.append(name) &&
            fs_.stat_remote(*server_, target.view(), true, &t) == Status::Ok) {
          directory = t.directory();
          a = t;
        }
      }
      e.type = directory ? NodeType::Directory : NodeType::File;
      e.size = directory || !a.has_size() ? 0 : a.size;
      e.read_only = a.read_only();
      if (!batch_.push_back(e)) return Status::OutOfMemory;
    }
    if (!r.ok()) return Status::Network;
    if (batch_.count() == 0 && count == 0) eof_ = true;
    return Status::Ok;
  }

  SftpFileSystem& fs_;
  SharedPointer<Server> server_;
  uint32_t generation_;
  ByteBuffer handle_;
  String path_;
  List<DirectoryEntry> batch_;
  uint64_t index_ = 0;
  bool eof_ = false;
};

class SftpFileSystem::ListDirectory final : public IDirectory {
 public:
  explicit ListDirectory(allocators::IAllocator& allocator) : entries_(allocator) {}
  bool add(StringView name) {
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

SftpFileSystem::SftpFileSystem(IServerNetwork& network, allocators::IAllocator& allocator, crypto::IRandom& random, HostTrust& trust)
    : network_(network), allocator_(allocator), random_(random), trust_(trust), servers_(allocator) {}

SftpFileSystem::~SftpFileSystem() {
  kernel::ScopedRecursiveMutex lock(mutex_);
  for (SharedPointer<Server>& s : servers_.view()) drop_connection(*s);
}

FileSystemInfo SftpFileSystem::info() const {
  FileSystemInfo result;
  result.type_name.assign("sftp");
  return result;
}

bool SftpFileSystem::trust_key(StringView host, String* key) {
  StringView name;
  uint16_t port = 22;
  split_host_port(host, 22, &name, &port);
  key->clear();
  if (!key->append("sftp:")) return false;
  for (char c : name) {
    if (!key->push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c)) return false;
  }
  return key->push_back(':') && key->append_uint(port);
}

SharedPointer<SftpFileSystem::Server> SftpFileSystem::find_server(StringView host, bool create) {
  for (SharedPointer<Server>& s : servers_.view())
    if (s->host.view().equals_ignore_case(host)) return s;
  if (!create) return SharedPointer<Server>();
  SharedPointer<Server> server = SharedPointer<Server>::make(allocator_, allocator_);
  if (!server || !server->host.assign(host)) return SharedPointer<Server>();
  StringView name;
  split_host_port(host, 22, &name, &server->port);
  if (!server->name.assign(name) || !servers_.push_back(server)) return SharedPointer<Server>();
  return server;
}

void SftpFileSystem::drop_connection(Server& server) { server.ssh.reset(); }

void SftpFileSystem::set_credentials(StringView host, StringView user, StringView password) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  SharedPointer<Server> server = find_server(host, true);
  if (!server) return;
  server->user.assign(user);
  server->password.assign(password);
  server->has_login = true;
  drop_connection(*server);
}

void SftpFileSystem::forget(StringView host) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  for (uint64_t i = 0; i < servers_.count(); ++i)
    if (servers_[i]->host.view().equals_ignore_case(host)) {
      drop_connection(*servers_[i]);
      servers_.remove_at(i);
      return;
    }
}

Status SftpFileSystem::ensure_connected(Server& server) {
  if (server.ssh && server.ssh->alive()) return Status::Ok;
  server.ssh.reset();
  if (!server.has_login) return Status::LoginRequired;
  SharedPointer<ITcpSocket> socket;
  INetworkStack* stack = nullptr;
  Status s = network_.connect(server.name.view(), server.port, 10000, &socket, &stack);
  if (s != Status::Ok) return s;
  UniquePointer<SshSession> ssh = UniquePointer<SshSession>::make(allocator_, *stack, util::move(socket), allocator_, random_);
  if (!ssh) return Status::OutOfMemory;
  String key(allocator_);
  if (!trust_key(server.host.view(), &key)) return Status::OutOfMemory;
  s = ssh->handshake(trust_, key.view());
  if (s != Status::Ok) return s;
  s = ssh->login_password(server.user.view(), server.password.view());
  if (s != Status::Ok) return s;
  s = ssh->open_subsystem("sftp");
  if (s != Status::Ok) return s;
  // SSH_FXP_INIT with version 3; the answer is the server's version (the lower of the two).
  {
    ByteBuffer init(allocator_);
    SshWriter w(&init);
    w.u32(5);
    w.u8(kInit);
    w.u32(3);
    if (!w.ok()) return Status::OutOfMemory;
    s = ssh->send(init.view());
    if (s != Status::Ok) return s;
    uint8_t head[5];
    s = ssh->receive(head, 5);
    if (s != Status::Ok) return s;
    const uint32_t length = (uint32_t{head[0]} << 24) | (uint32_t{head[1]} << 16) | (uint32_t{head[2]} << 8) | head[3];
    if (length < 5 || length > kMaxReply || head[4] != kVersion) return Status::Network;
    ByteBuffer rest(allocator_);
    if (!rest.put_zeros(length - 1)) return Status::OutOfMemory;
    s = ssh->receive(rest.writable_view().data(), static_cast<uint32_t>(length - 1));
    if (s != Status::Ok) return s;
    SshReader r(rest.view());
    if (r.u32() < 3) return Status::Unsupported;
  }
  server.ssh = util::move(ssh);
  server.next_id = 1;
  ++server.generation;
  return Status::Ok;
}

Status SftpFileSystem::send_request(Server& server, uint8_t type, uint32_t id, ConstByteView body) {
  ByteBuffer packet(allocator_);
  SshWriter w(&packet);
  w.u32(static_cast<uint32_t>(5 + body.size()));
  w.u8(type);
  w.u32(id);
  w.raw(body);
  if (!w.ok()) return Status::OutOfMemory;
  if (!server.ssh) return Status::Network;
  return server.ssh->send(packet.view());
}

Status SftpFileSystem::read_reply(Server& server, uint32_t* id, uint8_t* type, ByteBuffer* reply) {
  if (!server.ssh) return Status::Network;
  uint8_t head[9];
  Status s = server.ssh->receive(head, 9);
  if (s != Status::Ok) return s;
  const uint32_t length = (uint32_t{head[0]} << 24) | (uint32_t{head[1]} << 16) | (uint32_t{head[2]} << 8) | head[3];
  if (length < 5 || length > kMaxReply) {
    drop_connection(server);
    return Status::Network;
  }
  *type = head[4];
  *id = (uint32_t{head[5]} << 24) | (uint32_t{head[6]} << 16) | (uint32_t{head[7]} << 8) | head[8];
  reply->clear();
  if (length == 5) return Status::Ok;
  if (!reply->put_zeros(length - 5)) return Status::OutOfMemory;
  return server.ssh->receive(reply->writable_view().data(), length - 5);
}

Status SftpFileSystem::call(Server& server, uint8_t type, ConstByteView body, uint8_t* reply_type, ByteBuffer* reply, bool retry) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    Status s = ensure_connected(server);
    if (s != Status::Ok) return s;
    const uint32_t id = server.next_id++;
    s = send_request(server, type, id, body);
    uint32_t reply_id = 0;
    if (s == Status::Ok) s = read_reply(server, &reply_id, reply_type, reply);
    if (s == Status::Ok && reply_id != id) {
      drop_connection(server);
      s = Status::Network;
    }
    if (s == Status::Network && retry && attempt == 0) {
      drop_connection(server);
      continue;
    }
    return s;
  }
  return Status::Network;
}

Status SftpFileSystem::stat_remote(Server& server, StringView remote, bool follow, Attributes* out) {
  ByteBuffer body(allocator_), reply(allocator_);
  SshWriter w(&body);
  w.string(remote);
  if (!w.ok()) return Status::OutOfMemory;
  uint8_t type = 0;
  Status s = call(server, follow ? kStat : kLstat, body.view(), &type, &reply);
  if (s != Status::Ok) return s;
  if (type != kAttrs) {
    s = status_of(type, reply.view());
    return s == Status::Ok ? Status::IoError : s;
  }
  SshReader r(reply.view());
  return read_attributes(r, out) ? Status::Ok : Status::Network;
}

Status SftpFileSystem::close_handle(Server& server, ConstByteView handle) {
  ByteBuffer body(allocator_), reply(allocator_);
  SshWriter w(&body);
  w.string(handle);
  if (!w.ok()) return Status::OutOfMemory;
  uint8_t type = 0;
  const Status s = call(server, kClose, body.view(), &type, &reply, false);
  return s != Status::Ok ? s : status_of(type, reply.view());
}

// ---------------------------------------------------------------------------------------------------------------------------------------- the filesystem

bool SftpFileSystem::parse(StringView path, Parsed* out) const {
  StringView rest = path, component;
  uint32_t depth = 0;
  out->remote.clear();
  while (next_component(&rest, &component)) {
    if (is_dot(component)) continue;
    if (component.equals("..") || component.size() > 255) return false;
    if (depth == 0) {
      if (!out->server.assign(component)) return false;
    } else {
      if (!out->remote.push_back('/') || !out->remote.append(component)) return false;
      out->last.assign(component);
    }
    ++depth;
  }
  if (out->remote.empty() && !out->server.empty() && !out->remote.push_back('/')) return false;
  return true;
}

Status SftpFileSystem::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  if (where.server.empty()) {
    UniquePointer<ListDirectory> list = UniquePointer<ListDirectory>::make(allocator_, allocator_);
    if (!list) return Status::OutOfMemory;
    for (const SharedPointer<Server>& v : servers_.view())
      if (!list->add(v->host.view())) return Status::OutOfMemory;
    *directory = UniquePointer<IDirectory>(util::move(list));
    return Status::Ok;
  }
  SharedPointer<Server> server = find_server(where.server.view(), true);
  if (!server) return Status::OutOfMemory;
  ByteBuffer body(allocator_), reply(allocator_);
  SshWriter w(&body);
  w.string(where.remote.view());
  if (!w.ok()) return Status::OutOfMemory;
  uint8_t type = 0;
  Status s = call(*server, kOpendir, body.view(), &type, &reply);
  if (s != Status::Ok) return s;
  if (type != kHandle) {
    s = status_of(type, reply.view());
    return s == Status::Ok ? Status::IoError : s;
  }
  SshReader r(reply.view());
  const ConstByteView handle = r.string(256);
  if (!r.ok()) return Status::Network;
  UniquePointer<IDirectory> dir = UniquePointer<IDirectory>::make_as<Directory>(allocator_, *this, server, handle, where.remote.view());
  if (!dir) return Status::OutOfMemory;
  *directory = util::move(dir);
  return Status::Ok;
}

Status SftpFileSystem::stat(StringView path, DirectoryEntry* entry) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  *entry = DirectoryEntry();
  entry->type = NodeType::Directory;
  if (where.server.empty()) return Status::Ok;
  SharedPointer<Server> server = find_server(where.server.view(), true);
  if (!server) return Status::OutOfMemory;
  Attributes a;
  const Status s = stat_remote(*server, where.remote.view(), true, &a);
  if (s != Status::Ok) return s;
  entry->name.assign(where.at_root() ? where.server.view() : where.last.view());
  entry->type = a.directory() ? NodeType::Directory : NodeType::File;
  entry->size = a.directory() || !a.has_size() ? 0 : a.size;
  entry->read_only = a.read_only();
  return Status::Ok;
}

Status SftpFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  if (where.server.empty() || where.at_root()) return Status::IsADirectory;
  SharedPointer<Server> server = find_server(where.server.view(), true);
  if (!server) return Status::OutOfMemory;
  uint32_t flags = kFlagRead;
  bool create = false;
  switch (mode) {
    case OpenMode::Read: break;
    case OpenMode::ReadWrite: flags |= kFlagWrite; break;
    case OpenMode::CreateOrOpen: flags |= kFlagWrite | kCreate; create = true; break;
    case OpenMode::CreateTruncate: flags |= kFlagWrite | kCreate | kTruncate; create = true; break;
  }
  ByteBuffer body(allocator_), reply(allocator_);
  SshWriter w(&body);
  w.string(where.remote.view());
  w.u32(flags);
  if (create) write_permissions(w, 0644);
  else w.u32(0);
  if (!w.ok()) return Status::OutOfMemory;
  uint8_t type = 0;
  Status s = call(*server, kOpen, body.view(), &type, &reply);
  if (s != Status::Ok) return s;
  if (type != kHandle) {
    s = status_of(type, reply.view());
    if (s == Status::IoError || s == Status::Ok) {   // (the generic failure of a server that will not open a folder, or a name that is taken)
      Attributes a;
      if (stat_remote(*server, where.remote.view(), true, &a) == Status::Ok && a.directory()) return Status::IsADirectory;
    }
    return s == Status::Ok ? Status::IoError : s;
  }
  SshReader r(reply.view());
  ByteBuffer handle(allocator_);
  handle.append(r.string(256));
  if (!r.ok()) return Status::Network;
  // The size, and whether it is a folder after all.
  ByteBuffer fbody(allocator_), freply(allocator_);
  SshWriter fw(&fbody);
  fw.string(handle.view());
  if (!fw.ok()) return Status::OutOfMemory;
  s = call(*server, kFstat, fbody.view(), &type, &freply, false);
  Attributes a;
  if (s == Status::Ok && type == kAttrs) {
    SshReader ar(freply.view());
    if (!read_attributes(ar, &a)) s = Status::Network;
  } else if (s == Status::Ok) {
    s = status_of(type, freply.view());
    if (s == Status::Ok) s = Status::IoError;
  }
  if (s != Status::Ok || a.directory()) {
    close_handle(*server, handle.view());
    return s != Status::Ok ? s : Status::IsADirectory;
  }
  UniquePointer<IFile> opened = UniquePointer<IFile>::make_as<File>(allocator_, *this, server, handle.view(), a.has_size() ? a.size : 0, mode != OpenMode::Read);
  if (!opened) return Status::OutOfMemory;
  *file = util::move(opened);
  return Status::Ok;
}

Status SftpFileSystem::create_directory(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  if (where.server.empty() || where.at_root()) return Status::AlreadyExists;
  SharedPointer<Server> server = find_server(where.server.view(), true);
  if (!server) return Status::OutOfMemory;
  ByteBuffer body(allocator_), reply(allocator_);
  SshWriter w(&body);
  w.string(where.remote.view());
  write_permissions(w, 0755);
  if (!w.ok()) return Status::OutOfMemory;
  uint8_t type = 0;
  Status s = call(*server, kMkdir, body.view(), &type, &reply);
  if (s != Status::Ok) return s;
  s = status_of(type, reply.view());
  if (s == Status::IoError) {   // (servers say "failure" for a name that is taken)
    Attributes a;
    if (stat_remote(*server, where.remote.view(), false, &a) == Status::Ok) return Status::AlreadyExists;
  }
  return s;
}

Status SftpFileSystem::remove(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  if (where.server.empty() || where.at_root()) return Status::AccessDenied;
  SharedPointer<Server> server = find_server(where.server.view(), true);
  if (!server) return Status::OutOfMemory;
  Attributes a;
  Status s = stat_remote(*server, where.remote.view(), false, &a);   // (a link is removed, not what it points to)
  if (s != Status::Ok) return s;
  ByteBuffer body(allocator_), reply(allocator_);
  SshWriter w(&body);
  w.string(where.remote.view());
  if (!w.ok()) return Status::OutOfMemory;
  uint8_t type = 0;
  s = call(*server, a.directory() ? kRmdir : kRemove, body.view(), &type, &reply);
  if (s != Status::Ok) return s;
  s = status_of(type, reply.view());
  if (s == Status::IoError && a.directory()) {   // not empty?
    UniquePointer<IDirectory> dir;
    if (open_directory(path, &dir) == Status::Ok) {
      DirectoryEntry e;
      if (dir->next(&e) == Status::Ok) return Status::NotEmpty;
    }
  }
  return s;
}

Status SftpFileSystem::rename(StringView from, StringView to) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed a(allocator_), b(allocator_);
  if (!parse(from, &a) || !parse(to, &b)) return Status::InvalidPath;
  if (a.server.empty() || b.server.empty() || a.at_root() || b.at_root()) return Status::AccessDenied;
  if (!a.server.view().equals_ignore_case(b.server.view())) return Status::Unsupported;   // (not between servers)
  SharedPointer<Server> server = find_server(a.server.view(), true);
  if (!server) return Status::OutOfMemory;
  Attributes existing;
  const Status found = stat_remote(*server, b.remote.view(), false, &existing);
  if (found == Status::Ok) return Status::AlreadyExists;
  if (found != Status::NotFound) return found;
  ByteBuffer body(allocator_), reply(allocator_);
  SshWriter w(&body);
  w.string(a.remote.view());
  w.string(b.remote.view());
  if (!w.ok()) return Status::OutOfMemory;
  uint8_t type = 0;
  Status s = call(*server, kRename, body.view(), &type, &reply);
  if (s != Status::Ok) return s;
  return status_of(type, reply.view());
}

}  // namespace network
