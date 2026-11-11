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
#include "Process/WindowsUnixSockets.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kNoResources = 0xC000009Au,
                   kInfoLengthMismatch = 0xC0000004u, kNotImplemented = 0xC0000002u, kCancelled = 0xC0000120u, kInvalidInfoClass = 0xC0000003u,
                   kIoTimeout = 0xC00000B5u;      // STATUS_IO_TIMEOUT (WSAETIMEDOUT: SO_RCVTIMEO / SO_SNDTIMEO passed)
constexpr uint32_t kStatusWouldBlock = 0xC0000103u;      // (WindowsSockets.cpp's: WSAEWOULDBLOCK on a socket handle)
constexpr uint64_t kChunk = 64 * 1024;

struct Hold {
  ISystemSync& sync;
  explicit Hold(UnixSocketManager& m) : sync(m.sync()) { sync.lock(); }
  ~Hold() { sync.unlock(); }
};

bool read_u32(Process& p, uint64_t at, uint32_t* out) {
  uint8_t b[4];
  if (!p.read_user(at, ByteView(b, 4))) return false;
  *out = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
  return true;
}

void release_dropped(UnixSocketManager& m) {
  Queue<UnixInFlight> dropped(m.allocator());
  {
    Hold hold(m);
    if (!m.has_dropped_locked()) return;
    m.take_dropped_locked(&dropped);
  }
  m.release_dropped(dropped);
}

// Runs `op` until it is not WouldBlock, waiting between tries unless the socket is non-blocking (then the would-block status).
template <typename Op>
uint32_t run_blocking(Process& p, UnixSocketManager& m, uint32_t id, bool sending, Op&& op) {
  uint64_t timeout = ~uint64_t{0};
  bool non_blocking = false;
  {
    Hold hold(m);
    timeout = m.timeout_locked(id, sending);
    non_blocking = m.non_blocking_locked(id);
  }
  UnixStatus status = UnixStatus::Ok;
  const uint32_t waited = unix_wait(p, m, timeout, [&] {
    status = op();
    return status != UnixStatus::WouldBlock || non_blocking;
  });
  release_dropped(m);
  if (waited == 1) return kIoTimeout;
  if (waited == 2) return kCancelled;
  return unix_ntstatus(status);
}

// Reads the program's sockaddr_un. NTSTATUS.
uint32_t read_name(Process& p, uint64_t at, uint64_t length, UnixName* name) {
  if (at == 0) return kAccessViolation;
  if (length < 2 || length > kSockaddrUnSize) return kInvalidParameter;
  uint8_t bytes[kSockaddrUnSize];
  if (!p.read_user(at, ByteView(bytes, length))) return kAccessViolation;
  const UnixAddressResult r = parse_unix_address(ConstByteView(bytes, length), name);
  if (r == UnixAddressResult::WrongFamily) return 0xC0000141u;      // STATUS_INVALID_ADDRESS (WSAEAFNOSUPPORT)
  return r == UnixAddressResult::Ok ? kSuccess : kInvalidParameter;
}

// The OS file a path name means for this program (its Windows path, made absolute against its current directory). NTSTATUS.
uint32_t host_of(Process& p, const UnixName& name, String* host) {
  host->clear();
  if (name.kind != UnixName::Kind::Path) return kSuccess;
  ProcessFiles& files = p.files();
  if (!files.attached()) return 0xC0000236u;
  String virtual_path(files.allocator());
  if (files.resolve(name.text(), &virtual_path) != PathResult::Ok) return kInvalidParameter;
  return files.host_path_of(virtual_path.view(), host) ? kSuccess : kNoResources;
}

uint32_t file_status(filesystem::Status status) {
  switch (status) {
    case filesystem::Status::Ok: return kSuccess;
    case filesystem::Status::NotFound: return 0xC000003Au;      // STATUS_OBJECT_PATH_NOT_FOUND (the folder is not there)
    case filesystem::Status::AccessDenied: case filesystem::Status::ReadOnly: return 0xC0000022u;
    case filesystem::Status::AlreadyExists: return 0xC000020Au;      // (WSAEADDRINUSE)
    case filesystem::Status::OutOfMemory: return kNoResources;
    default: return kInvalidParameter;
  }
}

// Writes a name for the program into `capacity` bytes at `at` (cut short to it); its full size to *length_at when that is given.
uint32_t write_name(Process& p, uint64_t at, uint32_t capacity, uint64_t length_at, const UnixName& name) {
  uint8_t bytes[kSockaddrUnSize + 1];
  const uint32_t n = write_unix_address(ByteView(bytes, capacity < sizeof(bytes) ? capacity : sizeof(bytes)), name);
  if (n != 0 && !p.write_user(at, ConstByteView(bytes, n))) return kAccessViolation;
  if (length_at != 0 && !p.write_user_u32(length_at, unix_address_size(name))) return kAccessViolation;
  return kSuccess;
}

uint32_t give_handle(Process& p, UnixSocketManager& m, uint32_t id, uint64_t handle_at) {
  const int64_t slot = p.files().open_token(DescriptorKind::UnixSocket, id);
  if (slot < 0) { m.release(id); return kNoResources; }
  p.files().set_handle_flags(uint64_t(slot), false, false);
  if (!p.write_user_u64(handle_at, (uint64_t(slot) + 1) * 4)) { p.close_descriptor(uint64_t(slot)); return kAccessViolation; }
  return kSuccess;
}

}  // namespace

uint32_t unix_ntstatus(UnixStatus s) {
  switch (s) {
    case UnixStatus::Ok: return kSuccess;
    case UnixStatus::WouldBlock: return kStatusWouldBlock;
    case UnixStatus::Invalid: return kInvalidParameter;
    case UnixStatus::NoMemory:
    case UnixStatus::NoSockets: return kNoResources;
    case UnixStatus::BadSocket: return kInvalidHandle;
    case UnixStatus::AddressInUse: return 0xC000020Au;          // STATUS_ADDRESS_ALREADY_EXISTS -> WSAEADDRINUSE
    case UnixStatus::NotFound:
    case UnixStatus::Refused: return 0xC0000236u;               // STATUS_CONNECTION_REFUSED
    case UnixStatus::WrongType: return 0xC0000030u;             // STATUS_INVALID_PARAMETER_MIX -> WSAEPROTOTYPE
    case UnixStatus::AlreadyConnected: return 0xC000023Bu;      // STATUS_CONNECTION_ACTIVE -> WSAEISCONN
    case UnixStatus::NotConnected:
    case UnixStatus::DestinationRequired: return 0xC0000238u;   // STATUS_NOT_CONNECTED
    case UnixStatus::Broken: return 0xC00000B1u;                // STATUS_PIPE_CLOSING -> WSAESHUTDOWN (the other end does not take more)
    case UnixStatus::Reset: return 0xC0000050u;                 // (WSAECONNRESET)
    case UnixStatus::MessageTooBig: return 0xC0000206u;         // STATUS_INVALID_BUFFER_SIZE -> WSAEMSGSIZE
    case UnixStatus::NotSupported: return kNotImplemented;      // (WSAEOPNOTSUPP)
    case UnixStatus::NotPermitted: return 0xC0000022u;          // STATUS_ACCESS_DENIED -> WSAEACCES
  }
  return kInvalidParameter;
}

uint32_t unix_create_socket(Process& p, uint32_t type, uint64_t handle_at) {
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return kNotImplemented;
  if (handle_at == 0) return kAccessViolation;
  UnixType t;
  if (type == 1) t = UnixType::Stream;
  else if (type == 2) t = UnixType::Datagram;
  else if (type == 5) t = UnixType::SeqPacket;
  else return kInvalidParameter;
  uint32_t id = 0;
  if (const UnixStatus st = m->create(t, p.id(), &id); st != UnixStatus::Ok) return unix_ntstatus(st);
  return give_handle(p, *m, id, handle_at);
}

uint32_t unix_bind_socket(Process& p, uint32_t id, uint64_t address, uint64_t length) {
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return kInvalidHandle;
  UnixName name;
  if (const uint32_t s = read_name(p, address, length, &name); s != kSuccess) return s;
  String host(m->allocator());
  if (const uint32_t s = host_of(p, name, &host); s != kSuccess) return s;
  filesystem::IFileSystem* fs = p.files().file_system();
  if (name.kind == UnixName::Kind::Path) {
    {
      Hold hold(*m);
      UnixName current;
      if (!m->local_name_locked(id, &current)) return kInvalidHandle;
      if (current.kind != UnixName::Kind::Unnamed) return kInvalidParameter;      // (already bound: WSAEINVAL)
    }
    if (!fs) return 0xC000003Au;
    filesystem::DirectoryEntry entry;
    const filesystem::Status there = fs->stat(host.view(), &entry);
    if (there == filesystem::Status::Ok) return 0xC000020Au;      // the file is there: WSAEADDRINUSE until it is deleted, as on Windows
    if (there != filesystem::Status::NotFound) return file_status(there);
    UniquePointer<filesystem::IFile> file;
    if (const filesystem::Status made = fs->open_file(host.view(), filesystem::OpenMode::CreateOrOpen, &file); made != filesystem::Status::Ok) return file_status(made);
  }
  UnixStatus st;
  {
    Hold hold(*m);
    st = m->bind_locked(id, name, host.view());
  }
  if (st != UnixStatus::Ok && name.kind == UnixName::Kind::Path && fs) fs->remove(host.view());
  return unix_ntstatus(st);
}

uint32_t unix_connect_socket(Process& p, uint32_t id, uint64_t address, uint64_t length) {
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return kInvalidHandle;
  UnixName name;
  if (const uint32_t s = read_name(p, address, length, &name); s != kSuccess) return s;
  if (name.kind == UnixName::Kind::Unnamed) return kInvalidParameter;
  String host(m->allocator());
  if (const uint32_t s = host_of(p, name, &host); s != kSuccess) return s;
  if (name.kind == UnixName::Kind::Path) {
    filesystem::IFileSystem* fs = p.files().file_system();
    filesystem::DirectoryEntry entry;
    if (!fs || fs->stat(host.view(), &entry) != filesystem::Status::Ok) return 0xC0000236u;      // no such socket file: WSAECONNREFUSED
  }
  const uint64_t pid = p.id();
  return run_blocking(p, *m, id, true, [&] { return m->connect_locked(id, name, host.view(), pid); });
}

uint32_t unix_listen_socket(Process& p, uint32_t id, uint32_t backlog) {
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return kInvalidHandle;
  (void)p;
  Hold hold(*m);
  const int32_t asked = int32_t(backlog);
  return unix_ntstatus(m->listen_locked(id, asked < 0 || backlog == 0x7FFFFFFFu ? UnixSocketManager::kMaxBacklog : backlog));      // (SOMAXCONN)
}

uint32_t unix_accept_socket(Process& p, uint32_t id, uint64_t handle_at, uint64_t peer_at, uint64_t peer_length_at) {
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return kInvalidHandle;
  if (handle_at == 0) return kAccessViolation;
  uint32_t peer_capacity = 0;
  if (peer_at != 0 && (peer_length_at == 0 || !read_u32(p, peer_length_at, &peer_capacity))) return kAccessViolation;
  if (peer_at != 0 && peer_capacity < 2) return 0xC0000023u;      // STATUS_BUFFER_TOO_SMALL (WSAEFAULT), before a connection is taken
  uint32_t accepted = 0;
  const uint32_t status = run_blocking(p, *m, id, false, [&] { return m->accept_locked(id, &accepted); });
  if (status != kSuccess) return status;
  UnixName peer;
  {
    Hold hold(*m);
    m->peer_name_locked(accepted, &peer);
  }
  if (const uint32_t given = give_handle(p, *m, accepted, handle_at); given != kSuccess) return given;
  if (peer_at != 0) return write_name(p, peer_at, peer_capacity, peer_length_at, peer);
  return kSuccess;
}

uint32_t unix_send_socket(Process& p, uint32_t id, uint64_t buffer, uint32_t length, uint64_t sent_at, uint64_t to_at, uint64_t to_length) {
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return kInvalidHandle;
  if (length != 0 && buffer == 0) return kAccessViolation;
  UnixType type = UnixType::Stream;
  {
    Hold hold(*m);
    if (!m->type_locked(id, &type)) return kInvalidHandle;
  }
  UnixName to;
  String host(m->allocator());
  UnixSocketManager::Destination destination;
  if (to_at != 0) {
    if (const uint32_t s = read_name(p, to_at, to_length, &to); s != kSuccess) return s;
    if (const uint32_t s = host_of(p, to, &host); s != kSuccess) return s;
    destination.name = &to;
    destination.host_path = host.view();
  }
  const uint64_t pid = p.id();
  ByteBuffer staging(m->allocator());
  uint64_t done = 0;
  uint32_t status = kSuccess;
  if (type != UnixType::Stream) {
    if (length > UnixSocketManager::kBufferBytes) return unix_ntstatus(UnixStatus::MessageTooBig);
    if (!staging.put_zeros(length)) return kNoResources;
    if (length && !p.read_user(buffer, staging.writable_view())) return kAccessViolation;
    uint32_t sent = 0;
    status = run_blocking(p, *m, id, true, [&] { return m->send_locked(id, staging.view(), to_at ? &destination : nullptr, pid, nullptr, &sent); });
    done = sent;
  } else if (to_at != 0 || length == 0) {
    uint32_t sent = 0;
    Hold hold(*m);
    status = unix_ntstatus(m->send_locked(id, ConstByteView(), to_at ? &destination : nullptr, pid, nullptr, &sent));
  } else {      // a stream: in pieces, all of it (a blocking socket) or what fits (a non-blocking one)
    while (done < length && status == kSuccess) {
      const uint64_t chunk = length - done < kChunk ? length - done : kChunk;
      staging.clear();
      if (!staging.put_zeros(chunk)) { status = kNoResources; break; }
      if (!p.read_user(buffer + done, staging.writable_view())) { status = kAccessViolation; break; }
      uint64_t in_chunk = 0;
      while (in_chunk < chunk) {
        uint32_t sent = 0;
        status = run_blocking(p, *m, id, true, [&] { return m->send_locked(id, staging.view().subview(in_chunk, chunk - in_chunk), nullptr, pid, nullptr, &sent); });
        if (status != kSuccess) break;
        in_chunk += sent;
      }
      done += in_chunk;
    }
    if (done != 0) status = kSuccess;      // (part of it went: that is the answer)
  }
  if (status != kSuccess) return status;
  if (sent_at != 0 && !p.write_user_u32(sent_at, uint32_t(done))) return kAccessViolation;
  return kSuccess;
}

uint32_t unix_receive_socket(Process& p, uint32_t id, uint64_t buffer, uint32_t capacity, uint64_t got_at, uint64_t from_at, uint64_t from_length_at,
                             uint64_t peer_closed_at) {
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return kInvalidHandle;
  if (capacity != 0 && buffer == 0) return kAccessViolation;
  uint32_t from_capacity = 0;
  if (from_at != 0 && (from_length_at == 0 || !read_u32(p, from_length_at, &from_capacity))) return kAccessViolation;
  UnixType type = UnixType::Stream;
  {
    Hold hold(*m);
    if (!m->type_locked(id, &type)) return kInvalidHandle;
  }
  ByteBuffer staging(m->allocator());
  const uint32_t size = capacity < UnixSocketManager::kBufferBytes ? capacity : UnixSocketManager::kBufferBytes;
  if (!staging.put_zeros(size)) return kNoResources;
  UnixSocketManager::Received received;
  const uint32_t status = run_blocking(p, *m, id, false, [&] { return m->receive_locked(id, staging.writable_view(), false, &received, nullptr); });
  if (status != kSuccess) return status;
  if (received.got && !p.write_user(buffer, staging.view().first(received.got))) return kAccessViolation;
  if (got_at != 0 && !p.write_user_u32(got_at, received.got)) return kAccessViolation;
  if (from_at != 0 && !received.end_of_file) {
    UnixName from = received.from;
    if (type == UnixType::Stream || type == UnixType::SeqPacket) { Hold hold(*m); m->peer_name_locked(id, &from); }      // (a connection: its one peer)
    if (const uint32_t w = write_name(p, from_at, from_capacity, from_length_at, from); w != kSuccess) return w;
  }
  if (peer_closed_at != 0) {
    const uint8_t flag = received.end_of_file ? 1 : 0;
    if (!p.write_user(peer_closed_at, ConstByteView(&flag, 1))) return kAccessViolation;
  }
  if (type != UnixType::Stream && received.message_size > received.got) return unix_ntstatus(UnixStatus::MessageTooBig);      // (cut short: WSAEMSGSIZE, the data given)
  return kSuccess;
}

uint32_t unix_socket_control(Process& p, uint32_t id, uint32_t operation, uint64_t in, uint32_t in_length, uint64_t out, uint32_t out_length) {
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return kInvalidHandle;
  switch (operation) {
    case 0: {      // Shutdown
      uint32_t how = 0;
      if (in_length != 4) return kInfoLengthMismatch;
      if (!read_u32(p, in, &how)) return kAccessViolation;
      if (how > 2) return kInvalidParameter;
      Hold hold(*m);
      return unix_ntstatus(m->shutdown_locked(id, how != 1, how != 0));
    }
    case 1: {      // SetNonBlocking
      uint8_t on = 0;
      if (in_length != 1) return kInfoLengthMismatch;
      if (!p.read_user(in, ByteView(&on, 1))) return kAccessViolation;
      Hold hold(*m);
      m->set_non_blocking_locked(id, on != 0);
      return kSuccess;
    }
    case 2:        // GetSockName
    case 3: {      // GetPeerName
      UnixName name;
      {
        Hold hold(*m);
        const bool ok = operation == 2 ? m->local_name_locked(id, &name) : m->peer_name_locked(id, &name);
        if (!ok) return operation == 2 ? kInvalidHandle : 0xC0000238u;      // STATUS_NOT_CONNECTED
      }
      if (out == 0 || out_length < 2) return kInfoLengthMismatch;
      if (const uint32_t w = write_name(p, out, out_length, 0, name); w != kSuccess) return w;
      if (in != 0 && in_length == 4 && !p.write_user_u32(in, unix_address_size(name))) return kAccessViolation;
      return kSuccess;
    }
    case 4: {      // BytesReadable
      if (out_length < 4) return kInfoLengthMismatch;
      uint32_t n = 0;
      {
        Hold hold(*m);
        n = m->readable_bytes_locked(id);
      }
      return p.write_user_u32(out, n) ? kSuccess : kAccessViolation;
    }
    case 7: {      // GetSocketInfo: ULONG family, ULONG type, BOOLEAN v6only, 3 bytes padding
      if (out_length < 12) return kInfoLengthMismatch;
      UnixType type = UnixType::Stream;
      {
        Hold hold(*m);
        if (!m->type_locked(id, &type)) return kInvalidHandle;
      }
      uint8_t bytes[12] = {};
      ByteWriter w{ByteView(bytes)};
      w.le32(kAfUnix);
      w.le32(uint32_t(type));
      return p.write_user(out, ConstByteView(bytes)) ? kSuccess : kAccessViolation;
    }
    case 11: {     // GetPeerPid
      if (out_length < 4) return kInfoLengthMismatch;
      uint64_t pid = 0;
      {
        Hold hold(*m);
        if (!m->peer_pid_locked(id, &pid)) return 0xC0000238u;
      }
      return p.write_user_u32(out, uint32_t(pid)) ? kSuccess : kAccessViolation;
    }
    case 12: {     // SetTimeout
      if (in_length != 8) return kInfoLengthMismatch;
      uint32_t which = 0, ms = 0;
      if (!read_u32(p, in, &which) || !read_u32(p, in + 4, &ms)) return kAccessViolation;
      if (which > 1) return kInvalidParameter;
      Hold hold(*m);
      m->set_timeout_locked(id, which == 1, uint64_t(ms) * 1000000u);
      return kSuccess;
    }
    case 13: {     // TakeError
      if (out_length < 4) return kInfoLengthMismatch;
      UnixStatus error;
      {
        Hold hold(*m);
        error = m->take_error_locked(id);
      }
      return p.write_user_u32(out, unix_ntstatus(error)) ? kSuccess : kAccessViolation;
    }
    case 5: case 6: case 8: case 9: case 10: return kNotImplemented;      // (the network's: an AF_UNIX socket has no interface, IPv6 or names to look up)
    default: return kInvalidInfoClass;
  }
}

bool unix_select_ready_locked(UnixSocketManager& sockets, uint32_t id, uint32_t which) {
  const UnixReadiness r = sockets.readiness_locked(id);
  if (which == 0) return r.readable || r.hangup;
  if (which == 1) return r.writable;
  return r.error;
}

}  // namespace process::windowsabi
