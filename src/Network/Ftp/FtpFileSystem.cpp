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

#include "Network/Ftp/FtpFileSystem.hpp"
#include "Crypto/Crypto.hpp"
#include "FileSystem/Path.hpp"
#include "Kernel/Thread.hpp"
#include "Network/Dns/IDnsResolver.hpp"
#include "Network/HostPort.hpp"
#include "Utility/Move.hpp"

namespace network {

using namespace filesystem;

namespace {

constexpr uint32_t kStallMs = 30000;
constexpr uint64_t kMaxListing = 32u << 20;

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool has_control_characters(StringView text) {
  for (char c : text)
    if (c == '\r' || c == '\n' || c == 0) return true;
  return false;
}

// Splits `text` at whitespace into at most `max` tokens; *rest is what follows the last token taken (leading blanks dropped).
uint32_t tokenize(StringView text, StringView* tokens, uint32_t max, StringView* rest) {
  uint64_t i = 0;
  uint32_t count = 0;
  while (count < max) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i >= text.size()) break;
    const uint64_t start = i;
    while (i < text.size() && text[i] != ' ' && text[i] != '\t') ++i;
    tokens[count++] = text.substr(start, i - start);
  }
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
  *rest = text.substr(i);
  return count;
}

bool all_digits(StringView text) {
  if (text.empty()) return false;
  for (char c : text)
    if (!is_digit(c)) return false;
  return true;
}

uint64_t to_number(StringView text) {
  uint64_t v = 0;
  for (char c : text) v = v * 10 + static_cast<uint64_t>(c - '0');
  return v;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------- parsing

Status FtpFileSystem::map_reply(uint32_t code) {
  switch (code) {
    case 421: case 425: case 426: return Status::Network;
    case 530: case 532: return Status::LoginRequired;
    case 450: return Status::Busy;   // (file unavailable; also what some servers say for an empty folder)
    case 550: return Status::NotFound;
    case 452: case 552: return Status::NoSpace;
    case 553: return Status::AccessDenied;
    case 551: return Status::InvalidPath;
    case 500: case 501: case 502: case 504: return Status::Unsupported;
    default: return Status::IoError;
  }
}

bool FtpFileSystem::parse_passive_reply(uint32_t code, StringView text, uint16_t* port) {
  const uint64_t open = text.find('(');
  if (open == StringView::npos) return false;
  if (code == 229) {   // (|||port|)
    StringView rest = text.substr(open + 1);
    if (rest.size() < 5) return false;
    const char d = rest[0];
    if (rest[1] != d || rest[2] != d) return false;
    rest = rest.substr(3);
    const uint64_t end = rest.find(d);
    if (end == StringView::npos || end == 0 || end > 5 || !all_digits(rest.first(end))) return false;
    const uint64_t value = to_number(rest.first(end));
    if (value == 0 || value > 65535) return false;
    *port = static_cast<uint16_t>(value);
    return true;
  }
  if (code == 227) {   // (h1,h2,h3,h4,p1,p2)
    uint32_t numbers[6];
    uint32_t count = 0;
    uint64_t i = open + 1;
    while (i < text.size() && count < 6) {
      if (!is_digit(text[i])) return false;
      uint32_t v = 0;
      while (i < text.size() && is_digit(text[i])) {
        v = v * 10 + static_cast<uint32_t>(text[i] - '0');
        if (v > 255) return false;
        ++i;
      }
      numbers[count++] = v;
      if (i < text.size() && text[i] == ',') ++i;
      else break;
    }
    if (count != 6) return false;
    const uint32_t value = numbers[4] * 256 + numbers[5];
    if (value == 0) return false;
    *port = static_cast<uint16_t>(value);
    return true;
  }
  return false;
}

// "type=file;size=123;modify=2024...;perm=adfrw; name"
bool FtpFileSystem::parse_mlsx_line(StringView line, DirectoryEntry* entry) {
  const uint64_t space = line.find(' ');
  if (space == StringView::npos) return false;
  const StringView facts = line.first(space), name = line.substr(space + 1);
  if (name.empty() || name.equals(".") || name.equals("..")) return false;
  bool directory = false, have_type = false, read_only = false;
  uint64_t size = 0;
  uint64_t start = 0;
  while (start < facts.size()) {
    uint64_t end = start;
    while (end < facts.size() && facts[end] != ';') ++end;
    const StringView fact = facts.substr(start, end - start);
    const uint64_t eq = fact.find('=');
    if (eq != StringView::npos) {
      const StringView key = fact.first(eq), value = fact.substr(eq + 1);
      if (key.equals_ignore_case("type")) {
        have_type = true;
        if (value.equals_ignore_case("dir")) directory = true;
        else if (value.equals_ignore_case("cdir") || value.equals_ignore_case("pdir")) return false;
      } else if (key.equals_ignore_case("size") && all_digits(value)) {
        size = to_number(value);
      } else if (key.equals_ignore_case("perm")) {
        bool writable = false;
        for (char c : value)
          if (c == 'w' || c == 'a' || c == 'c' || c == 'm') writable = true;
        read_only = !writable;
      }
    }
    start = end + 1;
  }
  if (!have_type) return false;
  *entry = DirectoryEntry();
  if (!entry->name.assign(name)) return false;
  entry->type = directory ? NodeType::Directory : NodeType::File;
  entry->size = directory ? 0 : size;
  entry->read_only = !directory && read_only;
  return true;
}

bool FtpFileSystem::parse_list_line(StringView line, DirectoryEntry* entry) {
  StringView tokens[9], rest;
  const uint32_t count = tokenize(line, tokens, 9, &rest);
  if (count < 4) return false;
  // Windows (IIS): "10-02-24  09:15AM       <DIR>          name" or "...AM      1234 name".
  if (tokens[0].size() >= 8 && is_digit(tokens[0][0]) && tokens[0][2] == '-' && count >= 4 && (tokens[2].equals("<DIR>") || all_digits(tokens[2]))) {
    StringView t[3], name;
    tokenize(line, t, 3, &name);
    if (name.empty() || name.equals(".") || name.equals("..")) return false;
    *entry = DirectoryEntry();
    if (!entry->name.assign(name)) return false;
    const bool directory = t[2].equals("<DIR>");
    entry->type = directory ? NodeType::Directory : NodeType::File;
    entry->size = directory ? 0 : to_number(t[2]);
    return true;
  }
  // Unix: "drwxr-xr-x 2 owner group 4096 Jan  1 12:00 name" (the group is missing on some servers).
  const char kind = tokens[0].empty() ? 0 : tokens[0][0];
  if (tokens[0].size() < 10 || (kind != '-' && kind != 'd' && kind != 'l')) return false;
  uint32_t size_index;
  uint32_t name_after;
  if (count >= 5 && all_digits(tokens[4])) {
    size_index = 4;
    name_after = 8;
  } else if (count >= 4 && all_digits(tokens[3])) {
    size_index = 3;
    name_after = 7;
  } else {
    return false;
  }
  StringView t[9], name;
  if (tokenize(line, t, name_after, &name) < name_after) return false;
  if (kind == 'l') {
    const uint64_t arrow = name.find(StringView(" -> "));
    if (arrow != StringView::npos) name = name.first(arrow);
  }
  if (name.empty() || name.equals(".") || name.equals("..")) return false;
  *entry = DirectoryEntry();
  if (!entry->name.assign(name)) return false;
  entry->type = kind == 'd' ? NodeType::Directory : NodeType::File;
  entry->size = kind == 'd' ? 0 : to_number(t[size_index]);
  entry->read_only = kind != 'd' && tokens[0][2] != 'w';
  return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------- files

class FtpFileSystem::File final : public IFile, public Transfer {
 public:
  File(FtpFileSystem& fs, SharedPointer<Server> server, StringView path, uint64_t size, bool writable, bool create_pending)
      : fs_(fs), server_(util::move(server)), generation_(server_->generation), path_(fs.allocator_), size_(size), writable_(writable), create_pending_(create_pending) {
    path_.assign(path);
  }
  ~File() {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    stop();
    if (create_pending_ && usable()) end_upload_with_empty();
  }
  uint64_t size() const override { return size_; }

  void stop() override {   // another command needs the server
    if (server_->active == this) server_->active = nullptr;
    if (!usable()) {
      release();
      return;
    }
    if (uploading_) end_upload();
    if (reading_) end_download(true);
  }

  Status read(uint64_t offset, ByteView buffer, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    if (!usable()) return Status::Network;
    if (uploading_) {
      const Status s = end_upload();
      if (s != Status::Ok) return s;
    }
    if (offset >= size_ || buffer.empty()) return Status::Ok;
    if (!reading_ || offset != data_position_) {
      if (reading_) end_download(true);
      const Status s = begin_download(offset);
      if (s != Status::Ok) return s;
    }
    uint64_t done = 0;
    while (done < buffer.size()) {
      uint32_t got = 0;
      const Status s = fs_.data_read(*server_, data_, buffer.subview(done, buffer.size() - done), &got);
      if (s != Status::Ok) {
        *transferred = done;
        release();
        return s;
      }
      if (got == 0) {   // the end of the file
        end_download(false);
        break;
      }
      done += got;
      data_position_ += got;
    }
    *transferred = done;
    return Status::Ok;
  }

  Status write(uint64_t offset, ConstByteView bytes, uint64_t* transferred) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    *transferred = 0;
    if (!writable_) return Status::ReadOnly;
    if (!usable()) return Status::Network;
    if (bytes.empty()) return Status::Ok;
    if (reading_) end_download(true);
    if (!uploading_) {
      if (offset != 0 && offset != size_) return Status::Unsupported;   // (FTP writes whole files from the start, or append)
      const Status s = begin_upload(offset == 0 ? "STOR" : "APPE");
      if (s != Status::Ok) return s;
      if (offset == 0) size_ = 0;
      upload_position_ = offset;
    } else if (offset != upload_position_) {
      return Status::Unsupported;
    }
    const Status s = fs_.data_write(*server_, data_, bytes);
    if (s != Status::Ok) {
      release();
      return s;
    }
    upload_position_ += bytes.size();
    if (upload_position_ > size_) size_ = upload_position_;
    create_pending_ = false;
    *transferred = bytes.size();
    return Status::Ok;
  }

  Status resize(uint64_t new_size) override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (!writable_) return Status::ReadOnly;
    if (!usable()) return Status::Network;
    if (new_size == size_) return Status::Ok;
    if (new_size != 0) return Status::Unsupported;
    if (reading_) end_download(true);
    if (uploading_) {
      const Status e = end_upload();
      if (e != Status::Ok) return e;
    }
    return end_upload_with_empty();
  }

  Status flush() override {
    kernel::ScopedRecursiveMutex lock(fs_.mutex_);
    if (!usable()) return uploading_ ? Status::Network : Status::Ok;
    if (uploading_) return end_upload();
    if (create_pending_ && writable_) return end_upload_with_empty();
    return Status::Ok;
  }

 private:
  bool usable() const { return server_->control && server_->generation == generation_; }
  void release() {   // the connection is gone or broken: forget the transfer without a word to the server
    if (data_.open()) data_.socket->abort();
    data_ = Data();
    reading_ = false;
    uploading_ = false;
    if (server_->active == this) server_->active = nullptr;
  }

  Status begin_download(uint64_t offset) {
    fs_.stop_active(*server_);
    Status s = fs_.start_transfer(*server_, "RETR", path_.view(), offset, &data_);
    if (s != Status::Ok) return s;
    reading_ = true;
    data_position_ = offset;
    server_->active = this;
    return Status::Ok;
  }

  Status end_download(bool abort) {
    if (!reading_) return Status::Ok;
    reading_ = false;
    if (server_->active == this) server_->active = nullptr;
    return fs_.finish_transfer(*server_, data_, abort);
  }

  Status begin_upload(const char* verb) {
    fs_.stop_active(*server_);
    const Status s = fs_.start_transfer(*server_, verb, path_.view(), 0, &data_);
    if (s != Status::Ok) return s == Status::NotFound ? Status::AccessDenied : s;
    uploading_ = true;
    server_->active = this;
    return Status::Ok;
  }

  Status end_upload() {
    if (!uploading_) return Status::Ok;
    uploading_ = false;
    if (server_->active == this) server_->active = nullptr;
    const Status s = fs_.finish_transfer(*server_, data_, false);
    return s == Status::NotFound ? Status::AccessDenied : s;
  }

  Status end_upload_with_empty() {
    create_pending_ = false;
    fs_.stop_active(*server_);
    const Status s = begin_upload("STOR");
    if (s != Status::Ok) return s;
    size_ = 0;
    return end_upload();
  }

  FtpFileSystem& fs_;
  SharedPointer<Server> server_;
  uint32_t generation_;
  String path_;
  uint64_t size_;
  bool writable_;
  bool create_pending_;       // made empty on the server when closed or flushed, if nothing was written
  Data data_;
  bool reading_ = false, uploading_ = false;
  uint64_t data_position_ = 0, upload_position_ = 0;
};

class FtpFileSystem::ListDirectory final : public IDirectory {
 public:
  explicit ListDirectory(allocators::IAllocator& allocator) : entries_(allocator) {}
  List<DirectoryEntry>& entries() { return entries_; }
  bool add_name(StringView name) {
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

FtpFileSystem::FtpFileSystem(IServerNetwork& network, allocators::IAllocator& allocator, const FtpEnvironment& environment)
    : network_(network), allocator_(allocator), environment_(environment), servers_(allocator) {}

FtpFileSystem::~FtpFileSystem() {
  kernel::ScopedRecursiveMutex lock(mutex_);
  for (SharedPointer<Server>& s : servers_.view()) drop_connection(*s);
}

FileSystemInfo FtpFileSystem::info() const {
  FileSystemInfo result;
  result.type_name.assign("ftps");
  return result;
}

bool FtpFileSystem::trust_key(StringView host, String* key) {
  StringView name;
  uint16_t port = 21;
  split_host_port(host, 21, &name, &port);
  key->clear();
  if (!key->append("ftps:")) return false;
  for (char c : name) {
    if (!key->push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c)) return false;
  }
  return key->push_back(':') && key->append_uint(port);
}

SharedPointer<FtpFileSystem::Server> FtpFileSystem::find_server(StringView host, bool create) {
  for (SharedPointer<Server>& s : servers_.view())
    if (s->host.view().equals_ignore_case(host)) return s;
  if (!create) return SharedPointer<Server>();
  SharedPointer<Server> server = SharedPointer<Server>::make(allocator_, allocator_);
  if (!server || !server->host.assign(host)) return SharedPointer<Server>();
  StringView name;
  split_host_port(host, 21, &name, &server->port);
  if (!server->name.assign(name) || !servers_.push_back(server)) return SharedPointer<Server>();
  return server;
}

void FtpFileSystem::drop_connection(Server& server) {
  server.active = nullptr;
  if (server.control) server.control->abort();
  server.control = SharedPointer<ITcpSocket>();
  server.control_tls = nullptr;
}

void FtpFileSystem::stop_active(Server& server) {
  if (!server.active) return;
  Transfer* t = server.active;
  server.active = nullptr;
  t->stop();
}

void FtpFileSystem::set_credentials(StringView host, StringView user, StringView password) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  SharedPointer<Server> server = find_server(host, true);
  if (!server) return;
  stop_active(*server);
  server->user.assign(user);
  server->password.assign(password);
  server->has_login = true;
  drop_connection(*server);
}

void FtpFileSystem::forget(StringView host) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  for (uint64_t i = 0; i < servers_.count(); ++i)
    if (servers_[i]->host.view().equals_ignore_case(host)) {
      drop_connection(*servers_[i]);
      servers_.remove_at(i);
      return;
    }
}

Status FtpFileSystem::secure(Server& server, SharedPointer<ITcpSocket> inner, SharedPointer<ITcpSocket>* out, TlsSocket** tls) {
  TlsConfig config;
  Ipv4Address literal;
  const bool is_address = parse_ipv4(server.name.view(), &literal);
  config.server_name = is_address ? nullptr : server.name.c_str();
  config.server_ipv4 = is_address ? literal.value() : 0;
  config.trust = environment_.roots;
  config.verify_certificates = environment_.roots != nullptr;
  config.clock = environment_.clock;
  config.clock_context = environment_.clock_context;
  config.random = environment_.random;
  SharedPointer<ITcpSocket> socket = SharedPointer<ITcpSocket>::make_as<TlsSocket>(allocator_, *server.stack, allocator_, config, util::move(inner));
  if (!socket) return Status::OutOfMemory;
  TlsSocket* t = static_cast<TlsSocket*>(socket.get());
  const uint64_t started = server.stack->now_ms();
  while (!t->handshake_complete()) {
    if (t->failed() || t->error() != TlsError::None) {
      socket->abort();
      return t->error() == TlsError::Certificate ? Status::UntrustedHost : Status::Network;
    }
    if (server.stack->now_ms() - started > kStallMs) {
      socket->abort();
      return Status::Network;
    }
    server.stack->poll();
    kernel::sleep_us(100);
  }
  if (!environment_.roots) {   // no certificate authorities: the first certificate seen is the one to expect
    if (!environment_.trust) {
      socket->abort();
      return Status::UntrustedHost;
    }
    uint8_t fingerprint[32];
    crypto::hash(crypto::HashKind::Sha256, t->server_certificate(), fingerprint);
    String key(allocator_);
    if (!trust_key(server.host.view(), &key)) return Status::OutOfMemory;
    const HostTrust::Result trusted = environment_.trust->check(key.view(), fingerprint);
    if (trusted == HostTrust::Result::Changed) {
      socket->abort();
      return Status::UntrustedHost;
    }
    if (trusted == HostTrust::Result::OutOfMemory) {
      socket->abort();
      return Status::OutOfMemory;
    }
  }
  *out = util::move(socket);
  *tls = t;
  return Status::Ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------------- the control connection

Status FtpFileSystem::send_line(Server& server, StringView line) {
  if (!server.control) return Status::Network;
  ByteBuffer bytes(allocator_);
  if (!bytes.append(ConstByteView(reinterpret_cast<const uint8_t*>(line.data()), line.size())) || !bytes.put8('\r') || !bytes.put8('\n')) return Status::OutOfMemory;
  ConstByteView rest = bytes.view();
  server.last_progress_ms = server.stack->now_ms();
  while (!rest.empty()) {
    if (server.control->failed()) {
      drop_connection(server);
      return Status::Network;
    }
    const uint32_t n = server.control->send(rest);
    if (n > 0) {
      rest = rest.subview(n);
      server.last_progress_ms = server.stack->now_ms();
      continue;
    }
    if (server.stack->now_ms() - server.last_progress_ms > kStallMs) {
      drop_connection(server);
      return Status::Network;
    }
    server.stack->poll();
    kernel::sleep_us(100);
  }
  return Status::Ok;
}

Status FtpFileSystem::read_reply(Server& server, uint32_t* code, String* text) {
  text->clear();
  String line(allocator_);
  bool first = true, multiline = false;
  uint32_t reply_code = 0;
  server.last_progress_ms = server.stack->now_ms();
  for (;;) {
    // one line
    line.clear();
    for (;;) {
      if (!server.control) return Status::Network;
      uint8_t b = 0;
      if (server.control->receive(ByteView(&b, 1)) == 1) {
        server.last_progress_ms = server.stack->now_ms();
        if (b == '\n') break;
        if (b != '\r' && (line.size() > 8192 || !line.push_back(static_cast<char>(b)))) {
          drop_connection(server);
          return Status::Network;
        }
        continue;
      }
      if (server.control->failed() || (server.control->peer_closed() && server.control->available() == 0) ||
          server.stack->now_ms() - server.last_progress_ms > kStallMs) {
        drop_connection(server);
        return Status::Network;
      }
      server.stack->poll();
      kernel::sleep_us(100);
    }
    if (first) {
      if (line.size() < 4 || !is_digit(line[0]) || !is_digit(line[1]) || !is_digit(line[2])) {
        drop_connection(server);
        return Status::Network;
      }
      reply_code = static_cast<uint32_t>((line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0'));
      multiline = line[3] == '-';
      first = false;
      if (!text->append(line.view().substr(4))) return Status::OutOfMemory;
      if (!multiline) break;
      continue;
    }
    // A multi-line reply ends with "code<space>".
    const bool last = line.size() >= 4 && is_digit(line[0]) && is_digit(line[1]) && is_digit(line[2]) && line[3] == ' ' &&
                      static_cast<uint32_t>((line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0')) == reply_code;
    if (!text->push_back('\n') || !text->append(last ? line.view().substr(4) : line.view())) return Status::OutOfMemory;
    if (last) break;
  }
  *code = reply_code;
  return Status::Ok;
}

Status FtpFileSystem::raw_command(Server& server, StringView text, uint32_t* code, String* reply) {
  if (has_control_characters(text)) return Status::InvalidPath;
  Status s = send_line(server, text);
  if (s != Status::Ok) return s;
  for (;;) {
    s = read_reply(server, code, reply);
    if (s != Status::Ok) return s;
    if (*code >= 100 && *code < 200 && *code != 125 && *code != 150) continue;   // (a progress remark)
    break;
  }
  if (*code == 421) {   // the server is closing the connection
    drop_connection(server);
    return Status::Network;
  }
  return Status::Ok;
}

Status FtpFileSystem::ensure_connected(Server& server) {
  if (server.control && !server.control->failed()) return Status::Ok;
  drop_connection(server);
  SharedPointer<ITcpSocket> socket;
  INetworkStack* stack = nullptr;
  Status s = network_.connect(server.name.view(), server.port, 10000, &socket, &stack);
  if (s != Status::Ok) return s;
  server.stack = stack;
  const bool implicit = server.port == 990;
  if (implicit) {
    s = secure(server, util::move(socket), &server.control, &server.control_tls);
    if (s != Status::Ok) return s;
  } else {
    server.control = util::move(socket);
    server.control_tls = nullptr;
  }
  uint32_t code = 0;
  String text(allocator_);
  s = read_reply(server, &code, &text);
  if (s != Status::Ok) return s;
  if (code != 220) {
    drop_connection(server);
    return code == 421 ? Status::Network : Status::Unsupported;
  }
  if (!implicit) {   // AUTH TLS, then everything else is encrypted. A server that cannot is not used: the password would go in the clear.
    s = raw_command(server, "AUTH TLS", &code, &text);
    if (s != Status::Ok) return s;
    if (code != 234) {
      drop_connection(server);
      return Status::Unsupported;
    }
    SharedPointer<ITcpSocket> secured;
    TlsSocket* tls = nullptr;
    s = secure(server, util::move(server.control), &secured, &tls);
    server.control = SharedPointer<ITcpSocket>();
    if (s != Status::Ok) return s;
    server.control = util::move(secured);
    server.control_tls = tls;
  }
  // Protected data connections.
  s = raw_command(server, "PBSZ 0", &code, &text);
  if (s != Status::Ok) return s;
  if (code != 200) {
    drop_connection(server);
    return Status::Unsupported;
  }
  s = raw_command(server, "PROT P", &code, &text);
  if (s != Status::Ok) return s;
  if (code != 200) {
    drop_connection(server);
    return Status::Unsupported;
  }
  // The login: the given one, else anonymous.
  String user(allocator_), password(allocator_);
  if (server.has_login) {
    user.assign(server.user.view());
    password.assign(server.password.view());
  } else {
    user.assign("anonymous");
    password.assign("tcos@");
  }
  String command(allocator_);
  command.assign("USER ");
  command.append(user.view());
  s = raw_command(server, command.view(), &code, &text);
  if (s != Status::Ok) return s;
  if (code == 331 || code == 332) {
    command.assign("PASS ");
    command.append(password.view());
    s = raw_command(server, command.view(), &code, &text);
    if (s != Status::Ok) return s;
  }
  if (code != 230) {
    drop_connection(server);
    return server.has_login ? Status::LoginFailed : Status::LoginRequired;
  }
  raw_command(server, "TYPE I", &code, &text);
  server.mlst = false;
  server.rest = true;
  if (raw_command(server, "FEAT", &code, &text) == Status::Ok && code == 211) {
    StringView rest = text.view();
    while (!rest.empty()) {
      const uint64_t nl = rest.find('\n');
      const StringView feature = (nl == StringView::npos ? rest : rest.first(nl)).trimmed();
      if (feature.size() >= 4 && feature.first(4).equals_ignore_case("MLST")) server.mlst = true;
      rest = nl == StringView::npos ? StringView() : rest.substr(nl + 1);
    }
  }
  raw_command(server, "OPTS UTF8 ON", &code, &text);
  ++server.generation;
  return Status::Ok;
}

Status FtpFileSystem::command(Server& server, StringView text, uint32_t* code, String* reply) {
  stop_active(server);
  for (int attempt = 0; attempt < 2; ++attempt) {
    Status s = ensure_connected(server);
    if (s != Status::Ok) return s;
    s = raw_command(server, text, code, reply);
    if (s == Status::Network && attempt == 0) {   // (an idle connection the server closed)
      drop_connection(server);
      continue;
    }
    return s;
  }
  return Status::Network;
}

// ---------------------------------------------------------------------------------------------------------------------------------------- transfers

Status FtpFileSystem::open_data(Server& server, Data* data) {
  String text(allocator_);
  uint32_t code = 0;
  uint16_t port = 0;
  Status s = raw_command(server, "EPSV", &code, &text);
  if (s != Status::Ok) return s;
  if (!(code == 229 && parse_passive_reply(229, text.view(), &port))) {
    s = raw_command(server, "PASV", &code, &text);
    if (s != Status::Ok) return s;
    if (code != 227 || !parse_passive_reply(227, text.view(), &port)) return code == 227 ? Status::Network : map_reply(code);
  }
  // The data connection goes to the address the control connection goes to (the address in a PASV reply is often a private one behind a NAT).
  const Ipv4Address peer = server.control->remote_address();
  String address(allocator_);
  for (int i = 0; i < 4; ++i) {
    if ((i && !address.push_back('.')) || !address.append_uint(peer.octets[i])) return Status::OutOfMemory;
  }
  INetworkStack* stack = nullptr;
  s = network_.connect(address.view(), port, 10000, &data->socket, &stack);
  data->tls = nullptr;
  return s;
}

Status FtpFileSystem::start_transfer(Server& server, StringView verb, StringView path, uint64_t rest, Data* data) {
  if (has_control_characters(path)) return Status::InvalidPath;
  uint32_t code = 0;
  String text(allocator_);
  Status s = ensure_connected(server);
  if (s != Status::Ok) return s;
  s = open_data(server, data);
  if (s != Status::Ok) return s;
  if (rest > 0) {
    String command(allocator_);
    command.assign("REST ");
    command.append_uint(rest);
    s = raw_command(server, command.view(), &code, &text);
    if (s == Status::Ok && code != 350) s = Status::Unsupported;
    if (s != Status::Ok) {
      data->socket->abort();
      *data = Data();
      return s;
    }
  }
  String command(allocator_);
  command.assign(verb);
  if (!path.empty()) {
    command.push_back(' ');
    command.append(path);
  }
  s = raw_command(server, command.view(), &code, &text);
  if (s == Status::Ok && code != 125 && code != 150) s = code == 501 ? Status::NotFound : map_reply(code);   // (501: a path the server cannot make sense of)
  if (s != Status::Ok) {
    data->socket->abort();
    *data = Data();
    return s;
  }
  SharedPointer<ITcpSocket> secured;
  TlsSocket* tls = nullptr;
  s = secure(server, util::move(data->socket), &secured, &tls);
  *data = Data();
  if (s != Status::Ok) {   // (the server is left with a transfer nobody takes: start over)
    drop_connection(server);
    return s;
  }
  data->socket = util::move(secured);
  data->tls = tls;
  return Status::Ok;
}

Status FtpFileSystem::data_read(Server& server, Data& data, ByteView out, uint32_t* got) {
  *got = 0;
  uint64_t last = server.stack->now_ms();
  for (;;) {
    const uint32_t n = data.socket->receive(out);
    if (n > 0) {
      *got = n;
      return Status::Ok;
    }
    if (data.socket->peer_closed() && data.socket->available() == 0) return Status::Ok;
    if (data.socket->failed()) {
      if (data.tls && data.tls->error() == TlsError::Truncated) return Status::Ok;   // (a server that ends the data without close_notify)
      return Status::Network;
    }
    if (server.stack->now_ms() - last > kStallMs) return Status::Network;
    server.stack->poll();
    kernel::sleep_us(100);
  }
}

Status FtpFileSystem::data_write(Server& server, Data& data, ConstByteView bytes) {
  uint64_t last = server.stack->now_ms();
  while (!bytes.empty()) {
    if (data.socket->failed()) return Status::Network;
    const uint32_t n = data.socket->send(bytes);
    if (n > 0) {
      bytes = bytes.subview(n);
      last = server.stack->now_ms();
      continue;
    }
    if (server.stack->now_ms() - last > kStallMs) return Status::Network;
    server.stack->poll();
    kernel::sleep_us(100);
  }
  return Status::Ok;
}

Status FtpFileSystem::finish_transfer(Server& server, Data& data, bool abort) {
  if (data.open()) {
    if (abort) {
      data.socket->abort();
    } else {
      const uint64_t started = server.stack->now_ms();
      while (!data.socket->peer_closed() && data.socket->unsent_or_unacknowledged() > 0 && !data.socket->failed() && server.stack->now_ms() - started < kStallMs) {
        server.stack->poll();
        kernel::sleep_us(100);
      }
      data.socket->close();
    }
    data = Data();
  }
  uint32_t code = 0;
  String text(allocator_);
  Status s = read_reply(server, &code, &text);
  if (s != Status::Ok) return s;
  if (abort) return Status::Ok;
  if (code == 226 || code == 250 || code == 200) return Status::Ok;
  return map_reply(code);
}

Status FtpFileSystem::list_remote(Server& server, StringView path, List<DirectoryEntry>* entries) {
  stop_active(server);
  Status s = ensure_connected(server);
  if (s != Status::Ok) return s;
  Data data;
  s = start_transfer(server, server.mlst ? "MLSD" : "LIST", path, 0, &data);
  if (s == Status::Busy && !server.mlst) return Status::Ok;   // (some servers answer an empty folder with "no files found")
  if (s != Status::Ok) return s;
  ByteBuffer listing(allocator_);
  uint8_t chunk[4096];
  for (;;) {
    uint32_t got = 0;
    s = data_read(server, data, ByteView(chunk, sizeof chunk), &got);
    if (s != Status::Ok) {
      finish_transfer(server, data, true);
      return s;
    }
    if (got == 0) break;
    if (listing.size() + got > kMaxListing || !listing.append(ConstByteView(chunk, got))) {
      finish_transfer(server, data, true);
      return Status::OutOfMemory;
    }
  }
  s = finish_transfer(server, data, false);
  if (s != Status::Ok) return s;
  StringView rest(reinterpret_cast<const char*>(listing.view().data()), listing.size());
  while (!rest.empty()) {
    const uint64_t nl = rest.find('\n');
    StringView line = nl == StringView::npos ? rest : rest.first(nl);
    rest = nl == StringView::npos ? StringView() : rest.substr(nl + 1);
    if (!line.empty() && line[line.size() - 1] == '\r') line = line.first(line.size() - 1);
    if (line.empty()) continue;
    DirectoryEntry e;
    if (server.mlst ? parse_mlsx_line(line, &e) : parse_list_line(line, &e))
      if (!entries->push_back(e)) return Status::OutOfMemory;
  }
  return Status::Ok;
}

Status FtpFileSystem::stat_remote(Server& server, const Parsed& where, DirectoryEntry* entry) {
  *entry = DirectoryEntry();
  if (where.at_root()) {
    entry->type = NodeType::Directory;
    entry->name.assign(where.server.view());
    return Status::Ok;
  }
  String text(allocator_);
  uint32_t code = 0;
  {
    // MLST asks for the one entry. (Servers that cannot say so fall back to listing the folder.)
    String command(allocator_);
    command.assign("MLST ");
    command.append(where.remote.view());
    Status s = this->command(server, command.view(), &code, &text);
    if (s != Status::Ok) return s;
    if (code == 250) {
      StringView rest = text.view();
      while (!rest.empty()) {
        const uint64_t nl = rest.find('\n');
        const StringView line = nl == StringView::npos ? rest : rest.first(nl);
        rest = nl == StringView::npos ? StringView() : rest.substr(nl + 1);
        if (line.empty() || line[0] != ' ') continue;
        DirectoryEntry e;
        // (the name in an MLST line is the path as given; the entry's name is its last component)
        if (parse_mlsx_line(line.substr(1), &e)) {
          *entry = e;
          entry->name.assign(where.last.view());
          return Status::Ok;
        }
      }
      return Status::NotFound;
    }
    if (code == 550 || code == 450) return Status::NotFound;
    if (code != 500 && code != 501 && code != 502 && code != 504) return map_reply(code);
  }
  // List the parent.
  String parent(allocator_);
  const uint64_t slash = where.remote.view().rfind('/');
  parent.assign(slash == 0 || slash == StringView::npos ? StringView("/") : where.remote.view().first(slash));
  List<DirectoryEntry> entries(allocator_);
  const Status s = list_remote(server, parent.view(), &entries);
  if (s != Status::Ok) return s;
  for (const DirectoryEntry& e : entries.view())
    if (e.name.view().equals(where.last.view())) {
      *entry = e;
      return Status::Ok;
    }
  return Status::NotFound;
}

// ---------------------------------------------------------------------------------------------------------------------------------------- the filesystem

bool FtpFileSystem::parse(StringView path, Parsed* out) const {
  StringView rest = path, component;
  uint32_t depth = 0;
  out->remote.clear();
  while (next_component(&rest, &component)) {
    if (is_dot(component)) continue;
    if (component.equals("..") || component.size() > 255 || has_control_characters(component)) return false;
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

Status FtpFileSystem::open_directory(StringView path, UniquePointer<IDirectory>* directory) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  UniquePointer<ListDirectory> list = UniquePointer<ListDirectory>::make(allocator_, allocator_);
  if (!list) return Status::OutOfMemory;
  if (where.server.empty()) {
    for (const SharedPointer<Server>& v : servers_.view())
      if (!list->add_name(v->host.view())) return Status::OutOfMemory;
    *directory = UniquePointer<IDirectory>(util::move(list));
    return Status::Ok;
  }
  SharedPointer<Server> server = find_server(where.server.view(), true);
  if (!server) return Status::OutOfMemory;
  for (int attempt = 0; attempt < 2; ++attempt) {
    list->entries().clear();
    const Status s = list_remote(*server, where.remote.view(), &list->entries());
    if (s == Status::Network && attempt == 0 && !server->control) continue;   // (a connection that had been dropped)
    if (s != Status::Ok) return s;
    break;
  }
  *directory = UniquePointer<IDirectory>(util::move(list));
  return Status::Ok;
}

Status FtpFileSystem::stat(StringView path, DirectoryEntry* entry) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  if (where.server.empty()) {
    *entry = DirectoryEntry();
    entry->type = NodeType::Directory;
    return Status::Ok;
  }
  SharedPointer<Server> server = find_server(where.server.view(), true);
  if (!server) return Status::OutOfMemory;
  stop_active(*server);
  Status s = ensure_connected(*server);
  if (s != Status::Ok) return s;
  return stat_remote(*server, where, entry);
}

Status FtpFileSystem::open_file(StringView path, OpenMode mode, UniquePointer<IFile>* file) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  if (where.server.empty() || where.at_root()) return Status::IsADirectory;
  SharedPointer<Server> server = find_server(where.server.view(), true);
  if (!server) return Status::OutOfMemory;
  stop_active(*server);
  Status s = ensure_connected(*server);
  if (s != Status::Ok) return s;
  DirectoryEntry found;
  s = stat_remote(*server, where, &found);
  bool create = false;
  if (s == Status::NotFound && (mode == OpenMode::CreateOrOpen || mode == OpenMode::CreateTruncate)) {
    create = true;
    found = DirectoryEntry();
  } else if (s != Status::Ok) {
    return s;
  } else if (found.type == NodeType::Directory) {
    return Status::IsADirectory;
  } else if (mode == OpenMode::CreateTruncate) {
    create = true;   // (made empty: when it is written to, or flushed)
    found.size = 0;
  }
  UniquePointer<IFile> opened = UniquePointer<IFile>::make_as<File>(allocator_, *this, server, where.remote.view(), found.size, mode != OpenMode::Read, create);
  if (!opened) return Status::OutOfMemory;
  *file = util::move(opened);
  return Status::Ok;
}

Status FtpFileSystem::create_directory(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  if (where.server.empty() || where.at_root()) return Status::AlreadyExists;
  SharedPointer<Server> server = find_server(where.server.view(), true);
  if (!server) return Status::OutOfMemory;
  String command(allocator_), text(allocator_);
  command.assign("MKD ");
  command.append(where.remote.view());
  uint32_t code = 0;
  Status s = this->command(*server, command.view(), &code, &text);
  if (s != Status::Ok) return s;
  if (code == 257 || code == 250) return Status::Ok;
  DirectoryEntry existing;
  if (code == 550 && stat_remote(*server, where, &existing) == Status::Ok) return Status::AlreadyExists;
  s = map_reply(code);
  return s == Status::NotFound ? Status::AccessDenied : s;
}

Status FtpFileSystem::remove(StringView path) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed where(allocator_);
  if (!parse(path, &where)) return Status::InvalidPath;
  if (where.server.empty() || where.at_root()) return Status::AccessDenied;
  SharedPointer<Server> server = find_server(where.server.view(), true);
  if (!server) return Status::OutOfMemory;
  stop_active(*server);
  Status s = ensure_connected(*server);
  if (s != Status::Ok) return s;
  DirectoryEntry entry;
  s = stat_remote(*server, where, &entry);
  if (s != Status::Ok) return s;
  String command(allocator_), text(allocator_);
  command.assign(entry.type == NodeType::Directory ? "RMD " : "DELE ");
  command.append(where.remote.view());
  uint32_t code = 0;
  s = this->command(*server, command.view(), &code, &text);
  if (s != Status::Ok) return s;
  if (code == 250 || code == 200) return Status::Ok;
  if (entry.type == NodeType::Directory && (code == 550 || code == 450)) {   // not empty?
    List<DirectoryEntry> entries(allocator_);
    if (list_remote(*server, where.remote.view(), &entries) == Status::Ok && entries.count() > 0) return Status::NotEmpty;
  }
  s = map_reply(code);
  return s == Status::NotFound ? Status::AccessDenied : s;
}

Status FtpFileSystem::rename(StringView from, StringView to) {
  kernel::ScopedRecursiveMutex lock(mutex_);
  Parsed a(allocator_), b(allocator_);
  if (!parse(from, &a) || !parse(to, &b)) return Status::InvalidPath;
  if (a.server.empty() || b.server.empty() || a.at_root() || b.at_root()) return Status::AccessDenied;
  if (!a.server.view().equals_ignore_case(b.server.view())) return Status::Unsupported;
  SharedPointer<Server> server = find_server(a.server.view(), true);
  if (!server) return Status::OutOfMemory;
  stop_active(*server);
  Status s = ensure_connected(*server);
  if (s != Status::Ok) return s;
  DirectoryEntry existing;
  const Status found = stat_remote(*server, b, &existing);
  if (found == Status::Ok) return Status::AlreadyExists;
  if (found != Status::NotFound) return found;
  String command(allocator_), text(allocator_);
  command.assign("RNFR ");
  command.append(a.remote.view());
  uint32_t code = 0;
  s = this->command(*server, command.view(), &code, &text);
  if (s != Status::Ok) return s;
  if (code != 350) return map_reply(code);
  command.assign("RNTO ");
  command.append(b.remote.view());
  s = this->command(*server, command.view(), &code, &text);
  if (s != Status::Ok) return s;
  if (code == 250 || code == 200) return Status::Ok;
  s = map_reply(code);
  return s == Status::NotFound ? Status::AccessDenied : s;
}

}  // namespace network
