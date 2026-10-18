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

#include "Network/Smb/SmbFileSystem.hpp"
#include "FileSystem/Path.hpp"
#include "Utility/Move.hpp"
#include "Utility/Utf16.hpp"

namespace network {

using namespace filesystem;

namespace {

constexpr uint32_t kReadAttributes = 0x00100080, kListDirectory = 0x00100081, kReadFile = 0x00120089, kReadWriteFile = 0x0012019F, kDeleteAccess = 0x00110080;
constexpr uint32_t kOpen = 1, kCreateNew = 2, kOpenIf = 3, kOverwriteIf = 5;
constexpr uint32_t kDirectoryFile = 0x1, kNonDirectoryFile = 0x40;

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24); }
uint32_t le16(const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); }
uint64_t le64(const uint8_t* p) { return uint64_t{le32(p)} | (uint64_t{le32(p + 4)} << 32); }

// ---- srvsvc over DCE/RPC (connection oriented, NDR 2.0): bind, then NetrShareEnumAll at level 1.
const uint8_t kSrvsvcUuid[16] = {0xC8, 0x4F, 0x32, 0x4B, 0x70, 0x16, 0xD3, 0x01, 0x12, 0x78, 0x5A, 0x47, 0xBF, 0x6E, 0xE1, 0x88};
const uint8_t kNdrUuid[16] = {0x04, 0x5D, 0x88, 0x8A, 0xEB, 0x1C, 0xC9, 0x11, 0x9F, 0xE8, 0x08, 0x00, 0x2B, 0x10, 0x48, 0x60};

bool rpc_header(ByteBuffer* b, uint8_t type, uint32_t call) {
  return b->put8(5) && b->put8(0) && b->put8(type) && b->put8(3) /* first and last fragment */ && b->put32(0x00000010) /* little endian, ASCII, IEEE */ && b->put16(0) /* length: patched */ &&
         b->put16(0) && b->put32(call);
}

void patch16(ByteBuffer* b, uint64_t at, uint32_t v) {
  b->patch8(at, static_cast<uint8_t>(v));
  b->patch8(at + 1, static_cast<uint8_t>(v >> 8));
}

bool build_bind(ByteBuffer* out) {
  bool ok = rpc_header(out, 11, 1) && out->put16(4280) && out->put16(4280) && out->put32(0) && out->put8(1) && out->put_zeros(3) && out->put16(0) && out->put8(1) && out->put8(0) &&
            out->append(ConstByteView(kSrvsvcUuid, 16)) && out->put16(3) && out->put16(0) && out->append(ConstByteView(kNdrUuid, 16)) && out->put32(2);
  if (ok) patch16(out, 8, static_cast<uint32_t>(out->size()));
  return ok;
}

// NDR: a conformant varying string of UTF-16 units including the terminator, padded to a multiple of four bytes.
bool ndr_string(allocators::IAllocator& allocator, ByteBuffer* b, StringView text) {
  ByteBuffer units(allocator);
  if (!util::append_utf16le(&units, text) || !units.put16(0)) return false;
  const uint32_t count = static_cast<uint32_t>(units.size() / 2);
  if (!b->put32(count) || !b->put32(0) || !b->put32(count) || !b->append(units.view())) return false;
  return units.size() % 4 == 0 || b->put_zeros(4 - units.size() % 4);
}

bool build_enum_request(allocators::IAllocator& allocator, StringView server, ByteBuffer* out) {
  String unc(allocator);
  if (!unc.append("\\\\") || !unc.append(server)) return false;
  bool ok = rpc_header(out, 0, 2) && out->put32(0) /* allocation hint: patched */ && out->put16(0) && out->put16(15) /* NetrShareEnumAll */;
  ok = ok && out->put32(0x00020000) && ndr_string(allocator, out, unc.view());                    // ServerName: a pointer to the string
  ok = ok && out->put32(1) && out->put32(1) && out->put32(0x00020004) && out->put32(0) && out->put32(0);   // InfoStruct: level 1, SHARE_INFO_1_CONTAINER { 0 entries, no array }
  ok = ok && out->put32(0xFFFFFFFF) && out->put32(0x00020008) && out->put32(0);                // PreferedMaximumLength, ResumeHandle
  if (!ok) return false;
  patch16(out, 8, static_cast<uint32_t>(out->size()));
  out->patch32(16, static_cast<uint32_t>(out->size() - 24));
  return true;
}

// A string in the stub: max count, offset, actual count, units. *at moves past it (and its padding). False if it does not fit.
bool read_ndr_string(ConstByteView stub, uint64_t* at, String* out) {
  if (*at + 12 > stub.size()) return false;
  const uint32_t offset = le32(stub.data() + *at + 4), actual = le32(stub.data() + *at + 8);
  *at += 12;
  if (offset != 0 || actual > 4096 || *at + uint64_t{actual} * 2 > stub.size()) return false;
  uint32_t length = actual;
  if (length && stub[*at + 2 * (length - 1)] == 0 && stub[*at + 2 * (length - 1) + 1] == 0) --length;   // (the terminator)
  const bool ok = util::append_utf8_from_utf16le(out, stub.subview(*at, uint64_t{length} * 2));
  *at += uint64_t{actual} * 2;
  *at = (*at + 3) & ~uint64_t{3};
  return ok;
}

// The share names in the stub of a NetrShareEnumAll answer: disk shares only, without the administrative ones ("C$", "ADMIN$").
bool parse_enum_response(allocators::IAllocator& allocator, ConstByteView stub, List<String>* names) {
  if (stub.size() < 24) return false;
  uint64_t at = 12;   // level, union tag, pointer to the container
  const uint32_t count = le32(stub.data() + at);
  const uint32_t array = le32(stub.data() + at + 4);
  at += 8;
  if (count == 0 || array == 0) return true;
  if (count > 4096 || at + 4 > stub.size()) return false;
  at += 4;   // the array's maximum count
  struct Entry { uint32_t name_pointer, type, remark_pointer; };
  List<Entry> entries(allocator);
  for (uint32_t i = 0; i < count; ++i) {
    if (at + 12 > stub.size()) return false;
    if (!entries.push_back({le32(stub.data() + at), le32(stub.data() + at + 4), le32(stub.data() + at + 8)})) return false;
    at += 12;
  }
  for (const Entry& e : entries.view()) {
    String name(allocator), remark(allocator);
    if (e.name_pointer && !read_ndr_string(stub, &at, &name)) return false;
    if (e.remark_pointer && !read_ndr_string(stub, &at, &remark)) return false;
    const bool disk = (e.type & 0xFF) == 0, hidden = (e.type & 0x80000000u) != 0 || (!name.empty() && name[name.size() - 1] == '$');
    if (disk && !hidden && !name.empty() && !names->push_back(util::move(name))) return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------- handles

class SmbFileSystem::File final : public IFile {
 public:
  File(SmbFileSystem& fs, SharedPointer<Server> server, uint32_t tree, const SmbFileId& id, uint64_t size, bool writable)
      : fs_(fs), server_(util::move(server)), generation_(server_->generation), tree_(tree), id_(id), size_(size), writable_(writable) {}
  ~File() {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (usable()) server_->connection->close(tree_, id_);
  }

  uint64_t size() const override { return size_; }
  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    if (!usable()) return Status::Network;
    uint64_t done = 0;
    while (done < buffer.size()) {
      uint32_t got = 0;
      const Status s = server_->connection->read(tree_, id_, offset + done, buffer.subview(done, buffer.size() - done), &got);
      if (s != Status::Ok) {
        *transferred = done;
        return s;
      }
      if (got == 0) break;   // the end of the file
      done += got;
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
      uint32_t wrote = 0;
      const Status s = server_->connection->write(tree_, id_, offset + done, data.subview(done, data.size() - done), &wrote);
      if (s != Status::Ok || wrote == 0) {
        *transferred = done;
        return s != Status::Ok ? s : Status::IoError;
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
    if (!usable()) return Status::Network;
    const Status s = server_->connection->set_end_of_file(tree_, id_, new_size);
    if (s == Status::Ok) size_ = new_size;
    return s;
  }
  Status flush() override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (!writable_) return Status::Ok;
    return usable() ? server_->connection->flush(tree_, id_) : Status::Network;
  }

 private:
  bool usable() const { return server_->connection && server_->connection->alive() && server_->generation == generation_; }
  SmbFileSystem& fs_;
  SharedPointer<Server> server_;
  uint32_t generation_, tree_;
  SmbFileId id_;
  uint64_t size_;
  bool writable_;
};

// A folder on a share, read from the server in batches.
class SmbFileSystem::Directory final : public IDirectory {
 public:
  Directory(SmbFileSystem& fs, SharedPointer<Server> server, uint32_t tree, const SmbFileId& id)
      : fs_(fs), server_(util::move(server)), generation_(server_->generation), tree_(tree), id_(id), batch_(fs.allocator_) {}
  ~Directory() {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (usable()) server_->connection->close(tree_, id_);
  }
  Status next(DirectoryEntry* entry) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    for (;;) {
      if (at_ + 64 > batch_.size()) {   // used up: ask for more
        if (done_) return Status::EndOfDirectory;
        if (!usable()) return Status::Network;
        batch_.clear();
        at_ = 0;
        const Status s = server_->connection->query_directory(tree_, id_, restart_, &batch_);
        restart_ = false;
        if (s == Status::EndOfDirectory) {
          done_ = true;
          return s;
        }
        if (s != Status::Ok) return s;
        continue;
      }
      const uint8_t* r = batch_.view().data() + at_;
      const uint32_t next_offset = le32(r), name_length = le32(r + 60);
      if (at_ + 64 + name_length > batch_.size()) return Status::Corrupt;
      at_ = next_offset ? at_ + next_offset : batch_.size();
      String name(fs_.allocator_);
      if (!util::append_utf8_from_utf16le(&name, batch_.view().subview(static_cast<uint64_t>(r - batch_.view().data()) + 64, name_length))) return Status::OutOfMemory;
      if (name.view().equals(".") || name.view().equals("..")) continue;
      *entry = DirectoryEntry();
      if (!entry->name.assign(name.view())) continue;   // (too long for a name here: left out)
      const uint32_t attributes = le32(r + 56);
      entry->type = (attributes & 0x10) ? NodeType::Directory : NodeType::File;
      entry->size = entry->type == NodeType::File ? le64(r + 40) : 0;
      entry->read_only = (attributes & 0x01) != 0;
      return Status::Ok;
    }
  }
  void rewind() override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    batch_.clear();
    at_ = 0;
    done_ = false;
    restart_ = true;
  }

 private:
  bool usable() const { return server_->connection && server_->connection->alive() && server_->generation == generation_; }
  SmbFileSystem& fs_;
  SharedPointer<Server> server_;
  uint32_t generation_, tree_;
  SmbFileId id_;
  ByteBuffer batch_;
  uint64_t at_ = 0;
  bool done_ = false, restart_ = true;
};

// A list of entries held in memory (the servers, the shares of a server).
class SmbFileSystem::ListDirectory final : public IDirectory {
 public:
  explicit ListDirectory(allocators::IAllocator& allocator) : entries_(allocator) {}
  bool add(StringView name) {
    DirectoryEntry e;
    if (!e.name.assign(name)) return true;   // (a name too long to show is left out)
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

// ---------------------------------------------------------------------------------------------------------------------------------------- the filesystem

SmbFileSystem::SmbFileSystem(ISmbNetwork& network, allocators::IAllocator& allocator, const SmbEnvironment& environment)
    : network_(network), allocator_(allocator), environment_(environment), servers_(allocator) {}

SmbFileSystem::~SmbFileSystem() {
  kernel::ScopedRecursiveMutex lock(mutex_);
  for (SharedPointer<Server>& s : servers_.view()) drop(*s);
}

FileSystemInfo SmbFileSystem::info() const {
  FileSystemInfo result;
  result.type_name.assign("smb");
  return result;
}

SharedPointer<SmbFileSystem::Server> SmbFileSystem::find_server(StringView host, bool create) {
  for (SharedPointer<Server>& s : servers_.view())
    if (s->host.view().equals_ignore_case(host)) return s;
  if (!create) return SharedPointer<Server>();
  SharedPointer<Server> server = SharedPointer<Server>::make(allocator_, allocator_);
  if (!server || !server->host.assign(host)) return SharedPointer<Server>();
  const uint64_t colon = host.rfind(':');   // "name:port"
  StringView name = host;
  uint32_t port = 445;
  if (colon != StringView::npos && colon > 0 && colon + 1 < host.size() && host.size() - colon <= 6) {
    uint32_t value = 0;
    bool digits = true;
    for (char c : host.substr(colon + 1)) {
      digits = digits && c >= '0' && c <= '9';
      value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    if (digits && value > 0 && value < 65536) {
      name = host.first(colon);
      port = value;
    }
  }
  server->port = static_cast<uint16_t>(port);
  if (!server->name.assign(name) || !servers_.push_back(server)) return SharedPointer<Server>();
  return server;
}

void SmbFileSystem::drop(Server& server) {
  server.connection.reset();
  server.shares.clear();
  ++server.generation;
}

void SmbFileSystem::set_credentials(StringView host, StringView user, StringView password) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  SharedPointer<Server> server = find_server(host, true);
  if (!server) return;
  StringView domain;
  const uint64_t slash = user.find('\\');
  const uint64_t at = user.find('@');
  if (slash != StringView::npos) {
    domain = user.first(slash);
    user = user.substr(slash + 1);
  } else if (at != StringView::npos) {
    domain = user.substr(at + 1);
    user = user.first(at);
  }
  server->user.assign(user);
  server->domain.assign(domain);
  server->password.assign(password);
  server->have_credentials = true;
  drop(*server);
}

void SmbFileSystem::forget(StringView host) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  for (uint64_t i = 0; i < servers_.count(); ++i)
    if (servers_[i]->host.view().equals_ignore_case(host)) {
      drop(*servers_[i]);
      servers_.remove_at(i);
      return;
    }
}

Status SmbFileSystem::connect(Server& server) {
  drop(server);
  SharedPointer<ITcpSocket> socket;
  INetworkStack* stack = nullptr;
  Status s = network_.connect(server.name.view(), server.port, 10000, &socket, &stack);
  if (s != Status::Ok) return s;
  UniquePointer<SmbConnection> connection = UniquePointer<SmbConnection>::make(allocator_, *stack, util::move(socket), allocator_, environment_);
  if (!connection) return Status::OutOfMemory;
  NtlmCredentials credentials;
  if (server.have_credentials) {
    credentials.user = server.user.view();
    credentials.domain = server.domain.view();
    credentials.password = server.password.view();
  }
  s = connection->login(credentials);
  if (s != Status::Ok) return s;
  server.connection = util::move(connection);
  ++server.generation;
  return Status::Ok;
}

Status SmbFileSystem::tree_for(Server& server, StringView share, uint32_t* tree) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!server.connection || !server.connection->alive()) {
      const Status s = connect(server);
      if (s != Status::Ok) return s;
    }
    for (const Share& known : server.shares.view())
      if (known.name.view().equals_ignore_case(share)) {
        *tree = known.tree;
        return Status::Ok;
      }
    uint32_t id = 0;
    const Status s = server.connection->tree_connect(server.name.view(), share, &id);
    if (s == Status::Ok) {
      Share entry{String(allocator_), id};
      if (!entry.name.assign(share) || !server.shares.push_back(util::move(entry))) return Status::OutOfMemory;
      *tree = id;
      return Status::Ok;
    }
    if (s != Status::Network || attempt == 1) return s;
    drop(server);   // the connection had died: once more with a fresh one
  }
  return Status::Network;
}

bool SmbFileSystem::parse(StringView path, Parsed* out) const {
  StringView rest = path, component;
  uint32_t depth = 0;
  while (next_component(&rest, &component)) {
    if (is_dot(component)) continue;
    if (component.equals("..") || component.contains('\\') || (depth != 0 && component.contains(':'))) return false;   // (a port goes with the server name)
    if (depth == 0) {
      if (!out->server.assign(component)) return false;
    } else if (depth == 1) {
      if (!out->share.assign(component)) return false;
    } else {
      if (!out->rest.empty() && !out->rest.push_back('/')) return false;
      if (!out->rest.append(component)) return false;
    }
    ++depth;
  }
  out->depth = depth > 2 ? 2 : depth;
  return true;
}

// Opens (or creates) the node a path names on its share.
Status SmbFileSystem::open_node(const Parsed& where, uint32_t access, uint32_t disposition, uint32_t options, SharedPointer<Server>* server_out, uint32_t* tree_out, SmbFileId* id, SmbFileInfo* info) {
  SharedPointer<Server> server = find_server(where.server.view(), true);
  if (!server) return Status::OutOfMemory;
  for (int attempt = 0; attempt < 2; ++attempt) {
    uint32_t tree = 0;
    Status s = tree_for(*server, where.share.view(), &tree);
    if (s != Status::Ok) return s;
    s = server->connection->create(tree, where.rest.view(), access, disposition, options, 0, id, info);
    if (s == Status::Network && attempt == 0) {
      drop(*server);
      continue;
    }
    if (s == Status::Ok) {
      *server_out = server;
      *tree_out = tree;
    }
    return s;
  }
  return Status::Network;
}

Status SmbFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  if (where.depth < 2) return Status::IsADirectory;
  const bool write = mode != OpenMode::Read;
  const uint32_t disposition = mode == OpenMode::CreateOrOpen ? kOpenIf : mode == OpenMode::CreateTruncate ? kOverwriteIf : kOpen;
  SharedPointer<Server> server;
  uint32_t tree = 0;
  SmbFileId id;
  SmbFileInfo info;
  if (where.rest.empty()) return Status::IsADirectory;   // (the share itself)
  Status s = open_node(where, write ? kReadWriteFile : kReadFile, disposition, kNonDirectoryFile, &server, &tree, &id, &info);
  if (s != Status::Ok) return s;
  UniquePointer<IFile> handle = UniquePointer<IFile>::make_as<File>(allocator_, *this, server, tree, id, info.size, write);
  if (!handle) {
    server->connection->close(tree, id);
    return Status::OutOfMemory;
  }
  *file = util::move(handle);
  return Status::Ok;
}

Status SmbFileSystem::list_shares_locked(Server& server, List<String>* names) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!server.connection || !server.connection->alive()) {
      const Status s = connect(server);
      if (s != Status::Ok) return s;
    }
    SmbConnection& c = *server.connection;
    uint32_t tree = 0;
    Status s = c.tree_connect(server.name.view(), "IPC$", &tree);
    if (s == Status::Network && attempt == 0) {
      drop(server);
      continue;
    }
    if (s != Status::Ok) return s;
    SmbFileId pipe;
    s = c.create(tree, "srvsvc", kReadWriteFile, kOpen, 0, 0, &pipe, nullptr);
    if (s != Status::Ok) {
      c.tree_disconnect(tree);
      return s == Status::NotFound ? Status::Unsupported : s;
    }
    ByteBuffer request(allocator_), answer(allocator_), stub(allocator_);
    s = Status::Ok;
    if (!build_bind(&request)) s = Status::OutOfMemory;
    if (s == Status::Ok) s = c.pipe_transceive(tree, pipe, request.view(), &answer);
    if (s == Status::Ok && (answer.size() < 16 || answer.view()[2] != 12)) s = Status::Unsupported;   // not a bind acknowledgement
    if (s == Status::Ok) {
      request.clear();
      answer.clear();
      if (!build_enum_request(allocator_, server.name.view(), &request)) s = Status::OutOfMemory;
    }
    if (s == Status::Ok) s = c.pipe_transceive(tree, pipe, request.view(), &answer);
    // The answer may come in several fragments: the first from the transaction, the rest read from the pipe.
    for (int fragments = 0; s == Status::Ok && fragments < 64; ++fragments) {
      if (answer.size() < 24 || answer.view()[2] != 2) {   // 2: response
        s = Status::Unsupported;
        break;
      }
      const uint8_t* h = answer.view().data();
      const uint32_t length = le16(h + 8);
      if (length < 24 || length > answer.size()) {
        s = Status::Corrupt;
        break;
      }
      if (!stub.append(answer.view().subview(24, length - 24))) {
        s = Status::OutOfMemory;
        break;
      }
      if (h[3] & 2) break;   // the last fragment
      answer.clear();
      uint8_t buffer[SmbConnection::kMaxIo];
      uint32_t got = 0;
      s = c.read(tree, pipe, 0, ByteView(buffer, sizeof buffer), &got);
      if (s == Status::Ok && (got == 0 || !answer.append(ConstByteView(buffer, got)))) s = Status::Corrupt;
    }
    if (s == Status::Ok && !parse_enum_response(allocator_, stub.view(), names)) s = Status::Corrupt;
    c.close(tree, pipe);
    c.tree_disconnect(tree);
    if (s == Status::Network && attempt == 0) {
      drop(server);
      continue;
    }
    return s;
  }
  return Status::Network;
}

Status SmbFileSystem::list_shares(StringView host, List<String>* names) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  SharedPointer<Server> server = find_server(host, true);
  if (!server) return Status::OutOfMemory;
  return list_shares_locked(*server, names);
}

Status SmbFileSystem::connection_info(StringView host, ConnectionInfo* info) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  SharedPointer<Server> server = find_server(host, true);
  if (!server) return Status::OutOfMemory;
  if (!server->connection || !server->connection->alive()) {
    const Status s = connect(*server);
    if (s != Status::Ok) return s;
  }
  info->dialect = server->connection->dialect();
  info->signing = server->connection->signing();
  info->guest = server->connection->guest();
  return Status::Ok;
}

Status SmbFileSystem::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  if (where.depth == 0) {   // the servers visited
    UniquePointer<ListDirectory> list = UniquePointer<ListDirectory>::make(allocator_, allocator_);
    if (!list) return Status::OutOfMemory;
    for (const SharedPointer<Server>& s : servers_.view())
      if (!list->add(s->host.view())) return Status::OutOfMemory;
    *directory = UniquePointer<IDirectory>(util::move(list));
    return Status::Ok;
  }
  if (where.depth == 1) {   // the shares of a server
    List<String> names(allocator_);
    const Status s = list_shares(where.server.view(), &names);
    if (s != Status::Ok) return s;
    UniquePointer<ListDirectory> list = UniquePointer<ListDirectory>::make(allocator_, allocator_);
    if (!list) return Status::OutOfMemory;
    for (const String& n : names.view())
      if (!list->add(n.view())) return Status::OutOfMemory;
    *directory = UniquePointer<IDirectory>(util::move(list));
    return Status::Ok;
  }
  SharedPointer<Server> server;
  uint32_t tree = 0;
  SmbFileId id;
  SmbFileInfo info;
  const Status s = open_node(where, kListDirectory, kOpen, kDirectoryFile, &server, &tree, &id, &info);
  if (s != Status::Ok) return s;
  UniquePointer<IDirectory> handle = UniquePointer<IDirectory>::make_as<Directory>(allocator_, *this, server, tree, id);
  if (!handle) {
    server->connection->close(tree, id);
    return Status::OutOfMemory;
  }
  *directory = util::move(handle);
  return Status::Ok;
}

Status SmbFileSystem::stat(StringView path, DirectoryEntry* entry) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  *entry = DirectoryEntry();
  entry->type = NodeType::Directory;
  if (where.depth == 0) return Status::Ok;
  if (where.depth == 1) {
    entry->name.assign(where.server.view());
    return Status::Ok;
  }
  SharedPointer<Server> server;
  uint32_t tree = 0;
  SmbFileId id;
  SmbFileInfo info;
  const Status s = open_node(where, kReadAttributes, kOpen, 0, &server, &tree, &id, &info);
  if (s != Status::Ok) return s;
  server->connection->close(tree, id);
  const StringView rest = where.rest.view();
  const uint64_t slash = rest.rfind('/');
  entry->name.assign(rest.empty() ? where.share.view() : slash == StringView::npos ? rest : rest.substr(slash + 1));
  entry->type = info.directory() ? NodeType::Directory : NodeType::File;
  entry->size = info.directory() ? 0 : info.size;
  entry->read_only = info.read_only();
  return Status::Ok;
}

Status SmbFileSystem::create_directory(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  if (where.depth < 2) return where.depth == 0 ? Status::AlreadyExists : Status::Unsupported;
  if (where.rest.empty()) return Status::AlreadyExists;
  SharedPointer<Server> server;
  uint32_t tree = 0;
  SmbFileId id;
  const Status s = open_node(where, kListDirectory, kCreateNew, kDirectoryFile, &server, &tree, &id, nullptr);
  if (s != Status::Ok) return s;
  server->connection->close(tree, id);
  return Status::Ok;
}

Status SmbFileSystem::remove(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  if (where.depth < 2 || where.rest.empty()) return Status::Unsupported;   // servers and shares are not removed from here
  SharedPointer<Server> server;
  uint32_t tree = 0;
  SmbFileId id;
  Status s = open_node(where, kDeleteAccess, kOpen, 0, &server, &tree, &id, nullptr);
  if (s != Status::Ok) return s;
  s = server->connection->delete_on_close(tree, id);
  const Status closed = server->connection->close(tree, id);   // (the deletion happens here)
  return s != Status::Ok ? s : closed;
}

Status SmbFileSystem::rename(StringView from, StringView to) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed source(allocator_), target(allocator_);
  if (!parse(from, &source) || !parse(to, &target)) return Status::InvalidPath;
  if (source.depth < 2 || target.depth < 2 || source.rest.empty() || target.rest.empty()) return Status::Unsupported;
  if (!source.server.view().equals_ignore_case(target.server.view()) || !source.share.view().equals_ignore_case(target.share.view())) return Status::Unsupported;   // between shares is a copy
  SharedPointer<Server> server;
  uint32_t tree = 0;
  SmbFileId id;
  Status s = open_node(source, kDeleteAccess, kOpen, 0, &server, &tree, &id, nullptr);
  if (s != Status::Ok) return s;
  s = server->connection->rename(tree, id, target.rest.view());
  server->connection->close(tree, id);
  return s;
}

}  // namespace network
