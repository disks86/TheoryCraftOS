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
#include "Process/LinuxSockets.hpp"
#include "Process/LinuxFiles.hpp"
#include "Process/UnixSockets.hpp"
#include "ObjectContainer/List.hpp"

namespace process::linuxabi {

namespace {

constexpr int64_t kEnoent = 2, kEintr = 4, kEbadf = 9, kEagain = 11, kEnomem = 12, kEfault = 14, kEinval = 22, kEnfile = 23, kEmfile = 24, kEnotty = 25,
                  kEpipe = 32, kEnametoolong = 36, kEnotsock = 88, kEdestaddrreq = 89, kEmsgsize = 90, kEprototype = 91, kEnoprotoopt = 92,
                  kEprotonosupport = 93, kEsocktnosupport = 94, kEopnotsupp = 95, kEafnosupport = 97, kEaddrinuse = 98, kEconnreset = 104, kEisconn = 106,
                  kEnotconn = 107, kEconnrefused = 111, kEperm = 1, kEtoomanyrefs = 109;
constexpr uint64_t kSockNonBlock = 0x800, kSockCloexec = 0x80000;
constexpr uint64_t kMsgPeek = 0x2, kMsgCtrunc = 0x8, kMsgTrunc = 0x20, kMsgDontwait = 0x40, kMsgWaitall = 0x100, kMsgWaitforone = 0x10000,
                   kMsgCmsgCloexec = 0x40000000;
constexpr uint64_t kSolSocket = 1;
constexpr uint32_t kScmRights = 1, kScmCredentials = 2;
constexpr uint64_t kIovMax = 1024, kMaxIoBytes = 0x7FFFF000u, kStreamChunk = 64 * 1024, kMaxPollFds = 4096;
constexpr uint64_t kMsghdrSize = 56, kMmsghdrSize = 64, kCmsgHeader = 16;
constexpr uint16_t kPollIn = 0x1, kPollOut = 0x4, kPollErr = 0x8, kPollHup = 0x10, kPollNval = 0x20, kPollRdNorm = 0x40, kPollWrNorm = 0x100, kPollRdHup = 0x2000;

uint64_t error(int64_t value) { return uint64_t(-value); }
uint64_t align8(uint64_t n) { return (n + 7) & ~uint64_t{7}; }

int64_t errno_of(UnixStatus s) {
  switch (s) {
    case UnixStatus::Ok: return 0;
    case UnixStatus::WouldBlock: return -kEagain;
    case UnixStatus::Invalid: return -kEinval;
    case UnixStatus::NoMemory: return -kEnomem;
    case UnixStatus::NoSockets: return -kEnfile;
    case UnixStatus::BadSocket: return -kEbadf;
    case UnixStatus::AddressInUse: return -kEaddrinuse;
    case UnixStatus::NotFound: return -kEnoent;
    case UnixStatus::Refused: return -kEconnrefused;
    case UnixStatus::WrongType: return -kEprototype;
    case UnixStatus::AlreadyConnected: return -kEisconn;
    case UnixStatus::NotConnected: return -kEnotconn;
    case UnixStatus::DestinationRequired: return -kEdestaddrreq;
    case UnixStatus::Broken: return -kEpipe;
    case UnixStatus::Reset: return -kEconnreset;
    case UnixStatus::MessageTooBig: return -kEmsgsize;
    case UnixStatus::NotSupported: return -kEopnotsupp;
    case UnixStatus::NotPermitted: return -kEperm;
  }
  return -kEinval;
}
uint64_t result_of(UnixStatus s) { return s == UnixStatus::Ok ? 0 : uint64_t(errno_of(s)); }

struct Hold {
  ISystemSync& sync;
  explicit Hold(UnixSocketManager& m) : sync(m.sync()) { sync.lock(); }
  ~Hold() { sync.unlock(); }
};

struct Sock {
  UnixSocketManager* m = nullptr;
  uint32_t id = 0;
};

// The socket behind a descriptor. 0, or -errno.
int64_t socket_of(Process& p, uint64_t fd, Sock* out) {
  const DescriptorKind kind = p.files().kind(fd);
  if (kind == DescriptorKind::Closed) return -kEbadf;
  if (kind != DescriptorKind::UnixSocket) return -kEnotsock;
  out->m = system_unix_sockets();
  if (!out->m) return -kEbadf;
  out->id = uint32_t(p.files().token(fd));
  return 0;
}

bool read_u32(Process& p, uint64_t at, uint32_t* out) {
  uint8_t b[4];
  if (!p.read_user(at, ByteView(b, 4))) return false;
  *out = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
  return true;
}

// Descriptors a socket's queue let go of under the lock: released now, with the lock let go.
void release_dropped(UnixSocketManager& m) {
  Queue<UnixInFlight> dropped(m.allocator());
  {
    Hold hold(m);
    if (!m.has_dropped_locked()) return;
    m.take_dropped_locked(&dropped);
  }
  m.release_dropped(dropped);
}

// Runs `op` (a UnixStatus with the lock held) until it is not WouldBlock, waiting between tries unless the socket is non-blocking or `dont_wait`. 0, or -errno
// (EAGAIN when it would block or the socket's timeout passed, EINTR when the program is told to stop).
template <typename Op>
int64_t run_blocking(Process& p, const Sock& s, bool sending, bool dont_wait, Op&& op) {
  uint64_t timeout = ~uint64_t{0};
  bool non_blocking = dont_wait;
  {
    Hold hold(*s.m);
    timeout = s.m->timeout_locked(s.id, sending);
    non_blocking = non_blocking || s.m->non_blocking_locked(s.id);
  }
  UnixStatus status = UnixStatus::Ok;
  const uint32_t waited = unix_wait(p, *s.m, timeout, [&] {
    status = op();
    return status != UnixStatus::WouldBlock || non_blocking;
  });
  release_dropped(*s.m);
  if (waited == 1) return -kEagain;
  if (waited == 2) return -kEintr;
  return errno_of(status);
}

// --- names ----------------------------------------------------------------------------------------------------------------------------------------------------

// Reads the program's sockaddr_un (`length` bytes at `address`). *unspecified (when given) says it was AF_UNSPEC (connect(): dissolve a datagram association).
int64_t read_name(Process& p, uint64_t address, uint64_t length, UnixName* name, bool* unspecified = nullptr) {
  if (length > kSockaddrUnSize || length < 2) return -kEinval;
  if (address == 0) return -kEfault;
  uint8_t bytes[kSockaddrUnSize];
  if (!p.read_user(address, ByteView(bytes, length))) return -kEfault;
  const UnixAddressResult r = parse_unix_address(ConstByteView(bytes, length), name);
  if (r == UnixAddressResult::WrongFamily && unspecified && bytes[0] == 0 && bytes[1] == 0) { *unspecified = true; return 0; }
  return r == UnixAddressResult::Ok ? 0 : -kEinval;
}

// The file of the OS's file system that a path name means for this program (cleared for the other kinds). 0, or -errno.
int64_t host_of(Process& p, const UnixName& name, String* host) {
  host->clear();
  if (name.kind != UnixName::Kind::Path) return 0;
  ProcessFiles& files = p.files();
  if (!files.attached()) return -kEnoent;
  String virtual_path(files.allocator());
  const PathResult r = files.resolve(name.text(), &virtual_path);
  if (r == PathResult::TooLong) return -kEnametoolong;
  if (r != PathResult::Ok) return -kEnoent;
  return files.host_path_of(virtual_path.view(), host) ? 0 : -kEnomem;
}

// A destination or a listener named by a path must have its file. 0, or -errno (ENOENT if it is not there).
int64_t require_file(Process& p, StringView host) {
  filesystem::IFileSystem* fs = p.files().file_system();
  if (!fs) return -kEnoent;
  filesystem::DirectoryEntry entry;
  const filesystem::Status status = fs->stat(host, &entry);
  return status == filesystem::Status::Ok ? 0 : int64_t(errno_of_status(status));
}

// Writes `name` for the program: into the buffer at `address` whose size is the int at `length_at` (cut short to it), then the full size into that int.
int64_t write_name(Process& p, uint64_t address, uint64_t length_at, const UnixName& name) {
  if (address == 0 || length_at == 0) return 0;
  uint32_t capacity = 0;
  if (!read_u32(p, length_at, &capacity)) return -kEfault;
  if (int32_t(capacity) < 0) return -kEinval;
  uint8_t bytes[kSockaddrUnSize + 1];
  const uint32_t n = write_unix_address(ByteView(bytes, capacity < sizeof(bytes) ? capacity : sizeof(bytes)), name);
  if (n != 0 && !p.write_user(address, ConstByteView(bytes, n))) return -kEfault;
  return p.write_user_u32(length_at, unix_address_size(name)) ? 0 : -kEfault;
}

uint64_t give_fd(Process& p, UnixSocketManager& m, uint32_t id, bool close_on_exec, bool non_blocking) {
  const int64_t slot = p.files().open_token(DescriptorKind::UnixSocket, id);
  if (slot < 0) { m.release(id); return error(kEmfile); }
  p.files().set_close_on_exec(uint64_t(slot), close_on_exec);
  if (non_blocking) { Hold hold(m); m.set_non_blocking_locked(id, true); }
  return uint64_t(slot);
}

// --- moving data ------------------------------------------------------------------------------------------------------------------------------------------------

// The program's iovec array (16 bytes each: base, length), read whole. *total is the sum of the lengths. 0, or -errno.
int64_t read_iovecs(Process& p, uint64_t iov, uint64_t count, ByteBuffer* raw, uint64_t* total) {
  *total = 0;
  if (count > kIovMax) return -kEmsgsize;
  if (count == 0) return 0;
  if (iov == 0) return -kEfault;
  if (!raw->put_zeros(count * 16)) return -kEnomem;
  if (!p.read_user(iov, raw->writable_view())) return -kEfault;
  ByteReader in(raw->view());
  for (uint64_t i = 0; i < count; ++i) {
    in.le64();
    const uint64_t length = in.le64();
    if (length > kMaxIoBytes - *total) return -kEinval;
    *total += length;
  }
  return 0;
}

// Copies `out.size()` bytes of the iovecs, starting `from` bytes in, from (gather) or to (scatter) the program. False on a bad address.
bool walk_iovecs(Process& p, ConstByteView raw, uint64_t from, ByteView data, bool gather) {
  ByteReader in(raw);
  uint64_t done = 0;
  while (done < data.size() && in.remaining() >= 16) {
    const uint64_t base = in.le64(), length = in.le64();
    if (from >= length) { from -= length; continue; }
    const uint64_t take = length - from < data.size() - done ? length - from : data.size() - done;
    if (base > ~uint64_t{0} - from) return false;
    const bool ok = gather ? p.read_user(base + from, data.subview(done, take)) : p.write_user(base + from, ConstByteView(data.subview(done, take)));
    if (!ok) return false;
    done += take;
    from = 0;
  }
  return done == data.size();
}

// One iovec for a plain buffer.
void single_iovec(uint8_t (&raw)[16], uint64_t buffer, uint64_t length) {
  ByteWriter w{ByteView(raw)};
  w.le64(buffer);
  w.le64(length);
}

// Sends what the iovecs hold (to `to` when given). The bytes sent, or -errno. `rights` go with the first byte (and stay the caller's if not sent).
int64_t send_core(Process& p, const Sock& s, ConstByteView iovs, uint64_t total, const UnixName* to, uint64_t flags, Queue<UnixInFlight>* rights) {
  UnixType type = UnixType::Stream;
  {
    Hold hold(*s.m);
    if (!s.m->type_locked(s.id, &type)) return -kEbadf;
  }
  String host(s.m->allocator());
  UnixSocketManager::Destination destination;
  if (to) {
    if (const int64_t r = host_of(p, *to, &host); r < 0) return r;
    if (to->kind == UnixName::Kind::Path && type == UnixType::Datagram)
      if (const int64_t r = require_file(p, host.view()); r < 0) return r;
    if (to->kind == UnixName::Kind::Unnamed) return -kEinval;
    destination.name = to;
    destination.host_path = host.view();
  }
  const bool dont_wait = (flags & kMsgDontwait) != 0;
  const uint64_t pid = p.id();
  ByteBuffer staging(s.m->allocator());
  if (type != UnixType::Stream) {
    if (total > UnixSocketManager::kBufferBytes) return -kEmsgsize;
    if (!staging.put_zeros(total)) return -kEnomem;
    if (!walk_iovecs(p, iovs, 0, staging.writable_view(), true)) return -kEfault;
    uint32_t sent = 0;
    const int64_t r = run_blocking(p, s, true, dont_wait, [&] {
      return s.m->send_locked(s.id, staging.view(), to ? &destination : nullptr, pid, rights, &sent);
    });
    return r < 0 ? r : int64_t(sent);
  }
  if (to) {      // (a stream socket cannot be given an address: EISCONN if connected, EOPNOTSUPP if not)
    uint32_t sent = 0;
    Hold hold(*s.m);
    return errno_of(s.m->send_locked(s.id, ConstByteView(), &destination, pid, rights, &sent));
  }
  if (total == 0) {
    uint32_t sent = 0;
    Hold hold(*s.m);
    const UnixStatus st = s.m->send_locked(s.id, ConstByteView(), nullptr, pid, rights, &sent);
    return st == UnixStatus::Ok ? 0 : errno_of(st);
  }
  uint64_t done = 0;
  while (done < total) {
    const uint64_t chunk = total - done < kStreamChunk ? total - done : kStreamChunk;
    staging.clear();
    if (!staging.put_zeros(chunk)) return done ? int64_t(done) : -kEnomem;
    if (!walk_iovecs(p, iovs, done, staging.writable_view(), true)) return done ? int64_t(done) : -kEfault;
    uint64_t in_chunk = 0;
    while (in_chunk < chunk) {
      uint32_t sent = 0;
      const int64_t r = run_blocking(p, s, true, dont_wait, [&] {
        return s.m->send_locked(s.id, staging.view().subview(in_chunk, chunk - in_chunk), nullptr, pid, rights, &sent);
      });
      if (r < 0) return done + in_chunk ? int64_t(done + in_chunk) : r;
      in_chunk += sent;
    }
    done += chunk;
  }
  return int64_t(done);
}

struct ReceiveResult {
  uint64_t got = 0;
  uint32_t message_size = 0;      // a message's full size (more than `got` if it was cut short)
  UnixName from;
  uint64_t sender_pid = 0;
  bool message = false;
};

// Receives into the iovecs. 0 and *out, or -errno. `rights` collects the descriptors that came with what was taken.
int64_t receive_core(Process& p, const Sock& s, ConstByteView iovs, uint64_t total, uint64_t flags, Queue<UnixInFlight>* rights, ReceiveResult* out) {
  *out = ReceiveResult();
  UnixType type = UnixType::Stream;
  {
    Hold hold(*s.m);
    if (!s.m->type_locked(s.id, &type)) return -kEbadf;
  }
  out->message = type != UnixType::Stream;
  const bool peek = (flags & kMsgPeek) != 0, dont_wait = (flags & kMsgDontwait) != 0;
  const bool wait_all = (flags & kMsgWaitall) && !peek && type == UnixType::Stream;
  ByteBuffer staging(s.m->allocator());
  const uint64_t first_size = total < UnixSocketManager::kBufferBytes ? total : UnixSocketManager::kBufferBytes;
  if (!staging.put_zeros(first_size)) return -kEnomem;
  UnixSocketManager::Received received;
  int64_t r = run_blocking(p, s, false, dont_wait, [&] { return s.m->receive_locked(s.id, staging.writable_view(), peek, &received, rights); });
  if (r < 0) return r;
  if (received.got && !walk_iovecs(p, iovs, 0, staging.writable_view().first(received.got), false)) return -kEfault;
  out->got = received.got;
  out->message_size = received.message_size;
  out->from = received.from;
  out->sender_pid = received.sender_pid;
  bool ended = received.end_of_file;
  while (wait_all && !ended && out->got < total) {
    const uint64_t want = total - out->got < UnixSocketManager::kBufferBytes ? total - out->got : UnixSocketManager::kBufferBytes;
    r = run_blocking(p, s, false, dont_wait, [&] { return s.m->receive_locked(s.id, staging.writable_view().first(want), false, &received, rights); });
    if (r < 0) break;      // (what came so far is the answer)
    if (received.got && !walk_iovecs(p, iovs, out->got, staging.writable_view().first(received.got), false)) return -kEfault;
    out->got += received.got;
    ended = received.end_of_file;
  }
  return 0;
}

// Puts received descriptors into the program's table. Those that do not fit in `room` (descriptors the control buffer can take) or the table are released.
// The descriptors made are written to `fds` (4 bytes each).
uint32_t install_rights(Process& p, UnixSocketManager& m, Queue<UnixInFlight>& rights, uint32_t room, bool close_on_exec, ByteBuffer* fds, bool* cut) {
  Queue<UnixInFlight> unused(m.allocator());
  uint32_t made = 0;
  UnixInFlight entry;
  while (rights.dequeue(&entry)) {
    int64_t slot = -1;
    if (made < room) slot = p.files().install(entry.kind, entry.object, entry.token);
    if (slot < 0) { *cut = true; unused.enqueue(static_cast<UnixInFlight&&>(entry)); continue; }
    p.files().set_close_on_exec(uint64_t(slot), close_on_exec);
    fds->put32(uint32_t(slot));
    ++made;
  }
  m.release_rights(unused);
  return made;
}

// The SCM_RIGHTS of a sendmsg() control buffer: each descriptor gets one more use, for the message (released again if it is not sent). SCM_CREDENTIALS is
// accepted and not needed (the receiver is told the sender's real process id anyway). 0, or -errno.
int64_t collect_rights(Process& p, UnixSocketManager& m, uint64_t control, uint64_t length, Queue<UnixInFlight>* rights) {
  if (length == 0) return 0;
  if (control == 0) return -kEfault;
  if (length > 64 * 1024) return -kEinval;
  ByteBuffer raw(m.allocator());
  if (!raw.put_zeros(length)) return -kEnomem;
  if (!p.read_user(control, raw.writable_view())) return -kEfault;
  uint64_t at = 0;
  while (length - at >= kCmsgHeader) {
    ByteReader header(raw.view().subview(at, kCmsgHeader));
    const uint64_t cmsg_length = header.le64();
    const uint32_t level = header.le32(), type = header.le32();
    if (cmsg_length < kCmsgHeader || cmsg_length > length - at) return -kEinval;
    if (level != kSolSocket) return -kEinval;
    const ConstByteView data = raw.view().subview(at + kCmsgHeader, cmsg_length - kCmsgHeader);
    if (type == kScmRights) {
      ByteReader fds(data);
      while (fds.remaining() >= 4) {
        const uint64_t fd = fds.le32();
        const DescriptorKind kind = p.files().kind(fd);
        if (kind == DescriptorKind::Closed || fd >= ProcessFiles::kMaxDescriptors) return -kEbadf;
        if (rights->count() >= UnixSocketManager::kMaxRights) return -kEtoomanyrefs;
        UnixInFlight entry;
        entry.kind = kind;
        entry.object = p.files().shared_object(fd);
        entry.token = p.files().token(fd);
        if (!rights->enqueue(static_cast<UnixInFlight&&>(entry))) return -kEnomem;
        descriptor_token_add(kind, rights->back()->token);
      }
    } else if (type != kScmCredentials) {
      return -kEinval;
    }
    const uint64_t step = align8(cmsg_length);
    if (step > length - at) break;
    at += step;
  }
  return 0;
}

struct Msghdr {
  uint64_t name = 0;
  uint32_t name_length = 0;
  uint64_t iov = 0, iov_count = 0, control = 0, control_length = 0;
  uint32_t flags = 0;
};

bool read_msghdr(Process& p, uint64_t at, Msghdr* m) {
  uint8_t raw[kMsghdrSize];
  if (at == 0 || !p.read_user(at, ByteView(raw, sizeof(raw)))) return false;
  ByteReader in{ConstByteView(raw, sizeof(raw))};
  m->name = in.le64();
  m->name_length = in.le32();
  in.skip(4);
  m->iov = in.le64();
  m->iov_count = in.le64();
  m->control = in.le64();
  m->control_length = in.le64();
  m->flags = in.le32();
  return true;
}

uint64_t do_sendmsg(Process& p, uint64_t fd, uint64_t message, uint64_t flags) {
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  Msghdr m;
  if (!read_msghdr(p, message, &m)) return error(kEfault);
  ByteBuffer raw(s.m->allocator());
  uint64_t total = 0;
  if (const int64_t r = read_iovecs(p, m.iov, m.iov_count, &raw, &total); r < 0) return error(-r);
  UnixName to;
  const bool has_to = m.name != 0 && m.name_length != 0;
  if (has_to)
    if (const int64_t r = read_name(p, m.name, m.name_length, &to); r < 0) return error(-r);
  Queue<UnixInFlight> rights(s.m->allocator());
  int64_t r = collect_rights(p, *s.m, m.control, m.control_length, &rights);
  if (r >= 0) r = send_core(p, s, raw.view(), total, has_to ? &to : nullptr, flags, &rights);
  s.m->release_rights(rights);      // (whatever was not sent)
  return r < 0 ? error(-r) : uint64_t(r);
}

uint64_t do_recvmsg(Process& p, uint64_t fd, uint64_t message, uint64_t flags, uint32_t* got_out = nullptr) {
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  Msghdr m;
  if (!read_msghdr(p, message, &m)) return error(kEfault);
  ByteBuffer raw(s.m->allocator());
  uint64_t total = 0;
  if (const int64_t r = read_iovecs(p, m.iov, m.iov_count, &raw, &total); r < 0) return error(-r);
  Queue<UnixInFlight> rights(s.m->allocator());
  ReceiveResult got;
  const int64_t r = receive_core(p, s, raw.view(), total, flags, &rights, &got);
  if (r < 0) { s.m->release_rights(rights); return error(-r); }
  uint32_t out_flags = 0;
  if (got.message && got.message_size > got.got) out_flags |= kMsgTrunc;
  // The control buffer: SCM_CREDENTIALS first (the receiver asked with SO_PASSCRED), then SCM_RIGHTS.
  bool pass_credentials = false;
  {
    Hold hold(*s.m);
    pass_credentials = s.m->pass_credentials_locked(s.id);
  }
  ByteBuffer control(s.m->allocator());
  bool cut = false;
  if (pass_credentials) {
    if (m.control_length >= control.size() + kCmsgHeader + 12) {
      control.put32(kCmsgHeader + 12); control.put32(0);      // cmsg_len (a size_t)
      control.put32(uint32_t(kSolSocket)); control.put32(kScmCredentials);
      control.put32(uint32_t(got.sender_pid)); control.put32(0); control.put32(0);      // pid, uid, gid
      control.put32(0);      // (padding to 8)
    } else {
      cut = true;
    }
  }
  if (rights.count() != 0) {
    const uint64_t left = m.control_length > control.size() ? m.control_length - control.size() : 0;
    const uint32_t room = left > kCmsgHeader ? uint32_t((left - kCmsgHeader) / 4) : 0;
    ByteBuffer fds(s.m->allocator());
    const uint32_t made = install_rights(p, *s.m, rights, room, (flags & kMsgCmsgCloexec) != 0, &fds, &cut);
    if (made != 0) {
      const uint64_t length = kCmsgHeader + uint64_t(made) * 4;
      control.put32(uint32_t(length)); control.put32(0);
      control.put32(uint32_t(kSolSocket)); control.put32(kScmRights);
      control.append(fds.view());
      if (align8(length) <= left) control.put_zeros(align8(length) - length);
    }
  }
  if (cut) out_flags |= kMsgCtrunc;
  if (control.size() != 0 && !p.write_user(m.control, control.view())) return error(kEfault);
  // msg_namelen, msg_controllen and msg_flags as they came out.
  uint32_t name_length = 0;
  if (m.name != 0) {
    uint8_t bytes[kSockaddrUnSize + 1];
    const uint32_t n = write_unix_address(ByteView(bytes, m.name_length < sizeof(bytes) ? m.name_length : sizeof(bytes)), got.from);
    if (n && !p.write_user(m.name, ConstByteView(bytes, n))) return error(kEfault);
    name_length = got.message ? unix_address_size(got.from) : 0;
  }
  if (!p.write_user_u32(message + 8, name_length) || !p.write_user_u64(message + 40, control.size()) || !p.write_user_u32(message + 48, out_flags))
    return error(kEfault);
  const uint64_t result = (flags & kMsgTrunc) && got.message ? got.message_size : got.got;
  if (got_out) *got_out = uint32_t(result);
  return result;
}

// --- readiness ---------------------------------------------------------------------------------------------------------------------------------------------------

struct Watched {
  DescriptorKind kind = DescriptorKind::Closed;
  uint32_t id = 0;
};

// What poll() reports for one descriptor, for the events asked for (the lock held). Sockets report their state; anything else is ready.
uint16_t poll_events_locked(UnixSocketManager* m, const Watched& w, uint16_t events) {
  if (w.kind == DescriptorKind::Closed) return kPollNval;
  if (w.kind != DescriptorKind::UnixSocket || !m) return events & (kPollIn | kPollOut | kPollRdNorm | kPollWrNorm);
  const UnixReadiness r = m->readiness_locked(w.id);
  uint16_t out = 0;
  if (r.readable) out |= events & (kPollIn | kPollRdNorm);
  if (r.writable) out |= events & (kPollOut | kPollWrNorm);
  if (r.read_hangup) out |= events & kPollRdHup;
  if (r.hangup) out |= kPollHup;
  if (r.error) out |= kPollErr;
  return out;
}

// Waits until one of the descriptors is ready or `timeout_ns` (~0: for ever) passes. `evaluate` (lock held) returns how many are ready.
template <typename Evaluate>
int64_t wait_ready(Process& p, uint64_t timeout_ns, Evaluate&& evaluate) {
  UnixSocketManager* m = system_unix_sockets();
  if (!m) {      // no sockets at all: everything is ready already (or nothing ever will be)
    const uint32_t n = evaluate(nullptr);
    if (n != 0 || timeout_ns == 0) return n;
    if (timeout_ns != ~uint64_t{0}) p.block_sleep(timeout_ns);
    return 0;
  }
  uint32_t ready = 0;
  const uint32_t waited = unix_wait(p, *m, timeout_ns, [&] {
    ready = evaluate(m);
    return ready != 0 || timeout_ns == 0;
  });
  if (waited == 2) return -kEintr;
  return waited == 1 ? 0 : int64_t(ready);
}

Watched watch(Process& p, uint64_t fd) {
  Watched w;
  w.kind = p.files().kind(fd);
  if (w.kind == DescriptorKind::UnixSocket) w.id = uint32_t(p.files().token(fd));
  return w;
}

// An allocator for a call's own bookkeeping.
allocators::IAllocator* call_allocator(Process& p) {
  if (p.files().attached()) return &p.files().allocator();
  if (UnixSocketManager* m = system_unix_sockets()) return &m->allocator();
  return nullptr;
}

uint64_t do_poll(Process& p, uint64_t fds, uint64_t count, uint64_t timeout_ns) {
  if (count > kMaxPollFds) return error(kEinval);
  allocators::IAllocator* allocator = call_allocator(p);
  if (!allocator) return error(kEnomem);
  ByteBuffer raw(*allocator);
  if (count && !raw.put_zeros(count * 8)) return error(kEnomem);
  if (count && !p.read_user(fds, raw.writable_view())) return error(kEfault);
  // Each pollfd: int fd, short events, short revents.
  List<Watched> watched(*allocator);
  if (!watched.reserve(count)) return error(kEnomem);
  for (uint64_t i = 0; i < count; ++i) {
    ByteReader in(raw.view().subview(i * 8, 8));
    const int32_t fd = int32_t(in.le32());
    watched.push_back(fd < 0 ? Watched{DescriptorKind::Closed, ~uint32_t{0}} : watch(p, uint64_t(fd)));
  }
  const int64_t r = wait_ready(p, timeout_ns, [&](UnixSocketManager* m) {
    uint32_t ready = 0;
    for (uint64_t i = 0; i < count; ++i) {
      ByteReader in(raw.view().subview(i * 8, 8));
      const int32_t fd = int32_t(in.le32());
      const uint16_t events = in.le16();
      const uint16_t revents = fd < 0 ? 0 : poll_events_locked(m, watched[i], events);
      ByteWriter w{raw.writable_view().subview(i * 8 + 6, 2)};
      w.le16(revents);
      if (revents) ++ready;
    }
    return ready;
  });
  if (r < 0) return error(-r);
  if (count && !p.write_user(fds, raw.view())) return error(kEfault);
  return uint64_t(r);
}

}  // namespace

// --- the system calls ----------------------------------------------------------------------------------------------------------------------------------------------

uint64_t sys_socket(Process& p, uint64_t domain, uint64_t type, uint64_t protocol) {
  if (domain != kAfUnix) return error(kEafnosupport);
  const uint64_t base = type & 0xF, extra = type & ~uint64_t{0xF};
  if (extra & ~(kSockNonBlock | kSockCloexec)) return error(kEinval);
  UnixType t;
  if (base == 1) t = UnixType::Stream;
  else if (base == 2 || base == 3) t = UnixType::Datagram;      // (SOCK_RAW is SOCK_DGRAM for AF_UNIX, as on Linux)
  else if (base == 5) t = UnixType::SeqPacket;
  else return error(kEsocktnosupport);
  if (protocol != 0 && protocol != kAfUnix) return error(kEprotonosupport);
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return error(kEnomem);
  uint32_t id = 0;
  if (const UnixStatus st = m->create(t, p.id(), &id); st != UnixStatus::Ok) return result_of(st);
  return give_fd(p, *m, id, extra & kSockCloexec, extra & kSockNonBlock);
}

uint64_t sys_socketpair(Process& p, uint64_t domain, uint64_t type, uint64_t protocol, uint64_t pair) {
  if (domain != kAfUnix) return error(kEafnosupport);
  const uint64_t base = type & 0xF, extra = type & ~uint64_t{0xF};
  if (extra & ~(kSockNonBlock | kSockCloexec)) return error(kEinval);
  UnixType t;
  if (base == 1) t = UnixType::Stream;
  else if (base == 2 || base == 3) t = UnixType::Datagram;
  else if (base == 5) t = UnixType::SeqPacket;
  else return error(kEsocktnosupport);
  if (protocol != 0 && protocol != kAfUnix) return error(kEprotonosupport);
  if (pair == 0) return error(kEfault);
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return error(kEnomem);
  uint32_t a = 0, b = 0;
  if (const UnixStatus st = m->create_pair(t, p.id(), &a, &b); st != UnixStatus::Ok) return result_of(st);
  const uint64_t fa = give_fd(p, *m, a, extra & kSockCloexec, extra & kSockNonBlock);
  if (int64_t(fa) < 0) { m->release(b); return fa; }
  const uint64_t fb = give_fd(p, *m, b, extra & kSockCloexec, extra & kSockNonBlock);
  if (int64_t(fb) < 0) { p.close_descriptor(fa); return fb; }
  if (!p.write_user_u32(pair, uint32_t(fa)) || !p.write_user_u32(pair + 4, uint32_t(fb))) {
    p.close_descriptor(fa);
    p.close_descriptor(fb);
    return error(kEfault);
  }
  return 0;
}

// pipe2(fds, flags) / pipe(fds): a one-way pipe, made as a connected AF_UNIX stream pair whose first end only reads and second only writes (each shut
// the other way), so that it can be passed with SCM_RIGHTS like any socket - what Wayland's clipboard and drag and drop hand around. O_CLOEXEC and
// O_NONBLOCK are SOCK_CLOEXEC and SOCK_NONBLOCK's values; O_DIRECT (packet mode) is refused. (fstat says a socket, not a FIFO.)
uint64_t sys_pipe2(Process& p, uint64_t fds, uint64_t flags) {
  if (flags & ~(kSockNonBlock | kSockCloexec)) return error(kEinval);
  if (fds == 0) return error(kEfault);
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return error(kEnomem);
  uint32_t a = 0, b = 0;
  if (const UnixStatus st = m->create_pair(UnixType::Stream, p.id(), &a, &b); st != UnixStatus::Ok) return result_of(st);
  {
    Hold hold(*m);
    m->shutdown_locked(a, false, true);      // (the read end sends nothing)
    m->shutdown_locked(b, true, false);      // (the write end receives nothing)
  }
  const uint64_t fr = give_fd(p, *m, a, flags & kSockCloexec, flags & kSockNonBlock);
  if (int64_t(fr) < 0) { m->release(b); return fr; }
  const uint64_t fw = give_fd(p, *m, b, flags & kSockCloexec, flags & kSockNonBlock);
  if (int64_t(fw) < 0) { p.close_descriptor(fr); return fw; }
  if (!p.write_user_u32(fds, uint32_t(fr)) || !p.write_user_u32(fds + 4, uint32_t(fw))) {
    p.close_descriptor(fr);
    p.close_descriptor(fw);
    return error(kEfault);
  }
  return 0;
}

uint64_t sys_bind(Process& p, uint64_t fd, uint64_t address, uint64_t length) {
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  UnixName name;
  if (const int64_t r = read_name(p, address, length, &name); r < 0) return error(-r);
  String host(s.m->allocator());
  if (const int64_t r = host_of(p, name, &host); r < 0) return error(-r);
  filesystem::IFileSystem* fs = p.files().file_system();
  if (name.kind == UnixName::Kind::Path) {
    {
      Hold hold(*s.m);
      UnixName current;
      if (s.m->local_name_locked(s.id, &current) && current.kind != UnixName::Kind::Unnamed) return error(kEinval);      // (already bound)
    }
    if (!fs) return error(kEnoent);
    filesystem::DirectoryEntry entry;
    const filesystem::Status there = fs->stat(host.view(), &entry);
    if (there == filesystem::Status::Ok) return error(kEaddrinuse);
    if (there != filesystem::Status::NotFound) return errno_of_status(there);
    UniquePointer<filesystem::IFile> file;      // the socket's file: made empty, kept after the socket is closed
    const filesystem::Status made = fs->open_file(host.view(), filesystem::OpenMode::CreateOrOpen, &file);
    if (made != filesystem::Status::Ok) return errno_of_status(made);
  }
  UnixStatus st;
  {
    Hold hold(*s.m);
    st = s.m->bind_locked(s.id, name, host.view());
  }
  if (st != UnixStatus::Ok && name.kind == UnixName::Kind::Path && fs) fs->remove(host.view());
  return result_of(st);
}

uint64_t sys_connect(Process& p, uint64_t fd, uint64_t address, uint64_t length) {
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  UnixName name;
  bool unspecified = false;
  if (const int64_t r = read_name(p, address, length, &name, &unspecified); r < 0) return error(-r);
  if (unspecified) {
    Hold hold(*s.m);
    return result_of(s.m->connect_locked(s.id, UnixName(), StringView(), p.id(), true));
  }
  if (name.kind == UnixName::Kind::Unnamed) return error(kEinval);
  String host(s.m->allocator());
  if (const int64_t r = host_of(p, name, &host); r < 0) return error(-r);
  if (name.kind == UnixName::Kind::Path)
    if (const int64_t r = require_file(p, host.view()); r < 0) return error(-r);
  const uint64_t pid = p.id();
  const int64_t r = run_blocking(p, s, true, false, [&] { return s.m->connect_locked(s.id, name, host.view(), pid); });
  return r < 0 ? error(-r) : 0;
}

uint64_t sys_listen(Process& p, uint64_t fd, uint64_t backlog) {
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  const int32_t asked = int32_t(backlog);
  Hold hold(*s.m);
  return result_of(s.m->listen_locked(s.id, asked < 0 ? UnixSocketManager::kMaxBacklog : uint32_t(asked)));
}

uint64_t sys_accept4(Process& p, uint64_t fd, uint64_t address, uint64_t length_at, uint64_t flags) {
  if (flags & ~(kSockNonBlock | kSockCloexec)) return error(kEinval);
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  if (address != 0) {
    uint32_t capacity = 0;
    if (length_at == 0 || !read_u32(p, length_at, &capacity)) return error(kEfault);
    if (int32_t(capacity) < 0) return error(kEinval);
  }
  uint32_t accepted = 0;
  const int64_t r = run_blocking(p, s, false, false, [&] { return s.m->accept_locked(s.id, &accepted); });
  if (r < 0) return error(-r);
  UnixName peer;
  {
    Hold hold(*s.m);
    s.m->peer_name_locked(accepted, &peer);
  }
  const uint64_t new_fd = give_fd(p, *s.m, accepted, flags & kSockCloexec, flags & kSockNonBlock);
  if (int64_t(new_fd) < 0) return new_fd;
  if (const int64_t w = write_name(p, address, length_at, peer); w < 0) { p.close_descriptor(new_fd); return error(-w); }
  return new_fd;
}

uint64_t sys_sendto(Process& p, uint64_t fd, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address, uint64_t address_length) {
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  if (length > kMaxIoBytes) length = kMaxIoBytes;
  UnixName to;
  const bool has_to = address != 0;
  if (has_to)
    if (const int64_t r = read_name(p, address, address_length, &to); r < 0) return error(-r);
  uint8_t iov[16];
  single_iovec(iov, buffer, length);
  const int64_t r = send_core(p, s, ConstByteView(iov), length, has_to ? &to : nullptr, flags, nullptr);
  return r < 0 ? error(-r) : uint64_t(r);
}

uint64_t sys_recvfrom(Process& p, uint64_t fd, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address, uint64_t length_at) {
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  if (length > kMaxIoBytes) length = kMaxIoBytes;
  if (address != 0) {
    uint32_t capacity = 0;
    if (length_at == 0 || !read_u32(p, length_at, &capacity)) return error(kEfault);
    if (int32_t(capacity) < 0) return error(kEinval);
  }
  uint8_t iov[16];
  single_iovec(iov, buffer, length);
  ReceiveResult got;
  const int64_t r = receive_core(p, s, ConstByteView(iov), length, flags, nullptr, &got);
  if (r < 0) return error(-r);
  if (got.message)
    if (const int64_t w = write_name(p, address, length_at, got.from); w < 0) return error(-w);
  if (!got.message && address != 0 && !p.write_user_u32(length_at, 0)) return error(kEfault);      // (a stream reports no address)
  return (flags & kMsgTrunc) && got.message ? got.message_size : got.got;
}

uint64_t sys_sendmsg(Process& p, uint64_t fd, uint64_t message, uint64_t flags) { return do_sendmsg(p, fd, message, flags); }
uint64_t sys_recvmsg(Process& p, uint64_t fd, uint64_t message, uint64_t flags) { return do_recvmsg(p, fd, message, flags); }

uint64_t sys_sendmmsg(Process& p, uint64_t fd, uint64_t vector, uint64_t count, uint64_t flags) {
  if (count > kIovMax) count = kIovMax;
  uint64_t done = 0;
  for (; done < count; ++done) {
    const uint64_t r = do_sendmsg(p, fd, vector + done * kMmsghdrSize, flags);
    if (int64_t(r) < 0) return done ? done : r;
    if (!p.write_user_u32(vector + done * kMmsghdrSize + kMsghdrSize, uint32_t(r))) return done ? done : error(kEfault);
  }
  return done;
}

uint64_t sys_recvmmsg(Process& p, uint64_t fd, uint64_t vector, uint64_t count, uint64_t flags, uint64_t timeout) {
  (void)timeout;      // (each receive waits as the socket says; the overall timeout is not kept)
  if (count > kIovMax) count = kIovMax;
  uint64_t done = 0;
  for (; done < count; ++done) {
    const uint64_t call_flags = (done > 0 && (flags & kMsgWaitforone)) ? (flags | kMsgDontwait) : flags;
    uint32_t got = 0;
    const uint64_t r = do_recvmsg(p, fd, vector + done * kMmsghdrSize, call_flags & ~kMsgWaitforone, &got);
    if (int64_t(r) < 0) return done ? done : r;
    if (!p.write_user_u32(vector + done * kMmsghdrSize + kMsghdrSize, got)) return done ? done : error(kEfault);
  }
  return done;
}

uint64_t sys_shutdown(Process& p, uint64_t fd, uint64_t how) {
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  if (how > 2) return error(kEinval);
  Hold hold(*s.m);
  return result_of(s.m->shutdown_locked(s.id, how != 1, how != 0));
}

uint64_t sys_getsockname(Process& p, uint64_t fd, uint64_t address, uint64_t length_at) {
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  if (address == 0 || length_at == 0) return error(kEfault);
  UnixName name;
  {
    Hold hold(*s.m);
    if (!s.m->local_name_locked(s.id, &name)) return error(kEbadf);
  }
  const int64_t w = write_name(p, address, length_at, name);
  return w < 0 ? error(-w) : 0;
}

uint64_t sys_getpeername(Process& p, uint64_t fd, uint64_t address, uint64_t length_at) {
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  if (address == 0 || length_at == 0) return error(kEfault);
  UnixName name;
  {
    Hold hold(*s.m);
    if (!s.m->peer_name_locked(s.id, &name)) return error(kEnotconn);
  }
  const int64_t w = write_name(p, address, length_at, name);
  return w < 0 ? error(-w) : 0;
}

namespace {
constexpr uint64_t kSoReuseAddr = 2, kSoType = 3, kSoError = 4, kSoDontRoute = 5, kSoBroadcast = 6, kSoSndBuf = 7, kSoRcvBuf = 8, kSoKeepAlive = 9,
                   kSoLinger = 13, kSoPassCred = 16, kSoPeerCred = 17, kSoRcvLowat = 18, kSoSndLowat = 19, kSoRcvTimeo = 20, kSoSndTimeo = 21,
                   kSoAcceptConn = 30, kSoPassSec = 34, kSoSndBufForce = 32, kSoRcvBufForce = 33, kSoProtocol = 38, kSoDomain = 39, kSoRcvTimeoNew = 66,
                   kSoSndTimeoNew = 67;
}  // namespace

uint64_t sys_setsockopt(Process& p, uint64_t fd, uint64_t level, uint64_t name, uint64_t value, uint64_t length) {
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  if (level != kSolSocket) return error(kEopnotsupp);
  if (int32_t(length) < 0) return error(kEinval);
  switch (name) {
    case kSoPassCred: {
      uint32_t on = 0;
      if (length < 4) return error(kEinval);
      if (!read_u32(p, value, &on)) return error(kEfault);
      Hold hold(*s.m);
      s.m->set_pass_credentials_locked(s.id, on != 0);
      return 0;
    }
    case kSoRcvTimeo:
    case kSoSndTimeo:
    case kSoRcvTimeoNew:
    case kSoSndTimeoNew: {      // struct timeval (or the 64-bit __kernel_sock_timeval: the same layout on 64-bit)
      if (length < 16) return error(kEinval);
      uint64_t seconds = 0, micros = 0;
      if (!p.read_user_u64(value, &seconds) || !p.read_user_u64(value + 8, &micros)) return error(kEfault);
      if (int64_t(seconds) < 0 || micros >= 1000000) return error(kEinval);      // (Linux: EDOM; EINVAL is what programs test for)
      const uint64_t kMaxSeconds = ~uint64_t{0} / 1000000000u - 1;
      const uint64_t ns = seconds > kMaxSeconds ? ~uint64_t{0} : seconds * 1000000000u + micros * 1000u;
      Hold hold(*s.m);
      s.m->set_timeout_locked(s.id, name == kSoSndTimeo || name == kSoSndTimeoNew, ns);
      return 0;
    }
    case kSoReuseAddr: case kSoDontRoute: case kSoBroadcast: case kSoSndBuf: case kSoRcvBuf: case kSoKeepAlive: case kSoLinger: case kSoRcvLowat:
    case kSoSndLowat: case kSoPassSec: case kSoSndBufForce: case kSoRcvBufForce:
      return 0;      // (accepted; the buffers keep their one size)
    default: return error(kEnoprotoopt);
  }
}

uint64_t sys_getsockopt(Process& p, uint64_t fd, uint64_t level, uint64_t name, uint64_t value, uint64_t length_at) {
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  if (level != kSolSocket) return error(kEopnotsupp);
  uint32_t capacity = 0;
  if (length_at == 0 || !read_u32(p, length_at, &capacity)) return error(kEfault);
  if (int32_t(capacity) < 0) return error(kEinval);
  uint8_t out[16] = {};
  uint32_t size = 4;
  ByteWriter w{ByteView(out)};
  {
    Hold hold(*s.m);
    UnixType type = UnixType::Stream;
    if (!s.m->type_locked(s.id, &type)) return error(kEbadf);
    switch (name) {
      case kSoType: w.le32(uint32_t(type)); break;
      case kSoDomain: w.le32(kAfUnix); break;
      case kSoProtocol: w.le32(0); break;
      case kSoError: w.le32(uint32_t(-errno_of(s.m->take_error_locked(s.id)))); break;
      case kSoAcceptConn: w.le32(s.m->is_listening_locked(s.id) ? 1 : 0); break;
      case kSoSndBuf: case kSoRcvBuf: w.le32(UnixSocketManager::kBufferBytes); break;
      case kSoPassCred: w.le32(s.m->pass_credentials_locked(s.id) ? 1 : 0); break;
      case kSoPeerCred: {
        uint64_t pid = 0;
        if (!s.m->peer_pid_locked(s.id, &pid)) pid = 0;      // (not connected: Linux reports pid 0 and uid/gid -1)
        w.le32(uint32_t(pid));
        w.le32(pid ? 0 : ~uint32_t{0});
        w.le32(pid ? 0 : ~uint32_t{0});
        size = 12;
        break;
      }
      case kSoRcvTimeo: case kSoSndTimeo: case kSoRcvTimeoNew: case kSoSndTimeoNew: {
        const uint64_t ns = s.m->timeout_locked(s.id, name == kSoSndTimeo || name == kSoSndTimeoNew);
        w.le64(ns == ~uint64_t{0} ? 0 : ns / 1000000000u);
        w.le64(ns == ~uint64_t{0} ? 0 : ns % 1000000000u / 1000u);
        size = 16;
        break;
      }
      case kSoLinger: w.le32(0); w.le32(0); size = 8; break;
      case kSoReuseAddr: case kSoKeepAlive: case kSoBroadcast: case kSoDontRoute: case kSoPassSec: w.le32(0); break;
      case kSoRcvLowat: case kSoSndLowat: w.le32(1); break;
      default: return error(kEnoprotoopt);
    }
  }
  const uint32_t n = capacity < size ? capacity : size;
  if (n && !p.write_user(value, ConstByteView(out, n))) return error(kEfault);
  return p.write_user_u32(length_at, n) ? 0 : error(kEfault);
}

uint64_t sys_poll(Process& p, uint64_t fds, uint64_t count, uint64_t timeout_ns) { return do_poll(p, fds, count, timeout_ns); }

uint64_t sys_ppoll(Process& p, uint64_t fds, uint64_t count, uint64_t timespec) {
  uint64_t timeout = ~uint64_t{0};
  if (timespec != 0) {
    uint64_t seconds = 0, nanos = 0;
    if (!p.read_user_u64(timespec, &seconds) || !p.read_user_u64(timespec + 8, &nanos)) return error(kEfault);
    if (int64_t(seconds) < 0 || nanos >= 1000000000u) return error(kEinval);
    const uint64_t kMaxSeconds = ~uint64_t{0} / 1000000000u - 1;
    timeout = seconds > kMaxSeconds ? ~uint64_t{0} - 1 : seconds * 1000000000u + nanos;
  }
  return do_poll(p, fds, count, timeout);
}

uint64_t sys_select(Process& p, uint64_t count, uint64_t read_set, uint64_t write_set, uint64_t except_set, uint64_t time, bool nanoseconds) {
  constexpr uint64_t kFdSetBits = 1024;
  if (int64_t(count) < 0 || count > kFdSetBits) return error(kEinval);
  uint64_t timeout = ~uint64_t{0};
  if (time != 0) {
    uint64_t seconds = 0, fraction = 0;
    if (!p.read_user_u64(time, &seconds) || !p.read_user_u64(time + 8, &fraction)) return error(kEfault);
    const uint64_t unit = nanoseconds ? 1u : 1000u, limit = nanoseconds ? 1000000000u : 1000000u;
    if (int64_t(seconds) < 0 || fraction >= limit) return error(kEinval);
    const uint64_t kMaxSeconds = ~uint64_t{0} / 1000000000u - 1;
    timeout = seconds > kMaxSeconds ? ~uint64_t{0} - 1 : seconds * 1000000000u + fraction * unit;
  }
  const uint64_t bytes = (count + 63) / 64 * 8;      // (the kernel reads whole longs)
  uint8_t in_sets[3][kFdSetBits / 8] = {}, out_sets[3][kFdSetBits / 8] = {};
  const uint64_t sets[3] = {read_set, write_set, except_set};
  for (uint32_t k = 0; k < 3; ++k)
    if (sets[k] && bytes && !p.read_user(sets[k], ByteView(in_sets[k], bytes))) return error(kEfault);
  for (uint64_t fd = 0; fd < count; ++fd) {
    const bool asked = ((in_sets[0][fd / 8] | in_sets[1][fd / 8] | in_sets[2][fd / 8]) >> (fd % 8)) & 1;
    if (asked && p.files().kind(fd) == DescriptorKind::Closed) return error(kEbadf);
  }
  allocators::IAllocator* allocator = call_allocator(p);
  if (!allocator) return error(kEnomem);
  List<Watched> watched(*allocator);
  if (!watched.reserve(count)) return error(kEnomem);
  for (uint64_t fd = 0; fd < count; ++fd) watched.push_back(watch(p, fd));
  const int64_t r = wait_ready(p, timeout, [&](UnixSocketManager* m) {
    uint32_t ready = 0;
    for (uint32_t k = 0; k < 3; ++k) for (uint64_t i = 0; i < bytes; ++i) out_sets[k][i] = 0;
    for (uint64_t fd = 0; fd < count; ++fd) {
      const uint8_t bit = uint8_t(1u << (fd % 8));
      const bool want_read = in_sets[0][fd / 8] & bit, want_write = in_sets[1][fd / 8] & bit, want_except = in_sets[2][fd / 8] & bit;
      if (!want_read && !want_write && !want_except) continue;
      const uint16_t got = poll_events_locked(m, watched[fd], uint16_t(kPollIn | kPollOut));
      if (want_read && (got & (kPollIn | kPollHup | kPollErr))) { out_sets[0][fd / 8] |= bit; ++ready; }
      if (want_write && (got & (kPollOut | kPollErr))) { out_sets[1][fd / 8] |= bit; ++ready; }
      if (want_except && (got & kPollErr) && watched[fd].kind == DescriptorKind::UnixSocket) { out_sets[2][fd / 8] |= bit; ++ready; }
    }
    return ready;
  });
  if (r < 0) return error(-r);
  if (r == 0) for (uint32_t k = 0; k < 3; ++k) for (uint64_t i = 0; i < bytes; ++i) out_sets[k][i] = 0;
  for (uint32_t k = 0; k < 3; ++k)
    if (sets[k] && bytes && !p.write_user(sets[k], ConstByteView(out_sets[k], bytes))) return error(kEfault);
  return uint64_t(r);
}

// eventfd2(initial, flags) / eventfd(initial): a counter (UnixType::Event in the AF_UNIX manager, so that poll() and SCM_RIGHTS take it like a socket).
// EFD_CLOEXEC and EFD_NONBLOCK are O_CLOEXEC and O_NONBLOCK; EFD_SEMAPHORE makes a read take one.
uint64_t sys_eventfd2(Process& p, uint64_t initial, uint64_t flags) {
  constexpr uint64_t kEfdSemaphore = 1;
  if (flags & ~(kSockNonBlock | kSockCloexec | kEfdSemaphore)) return error(kEinval);
  UnixSocketManager* m = system_unix_sockets();
  if (!m) return error(kEnomem);
  uint32_t id = 0;
  if (const UnixStatus st = m->create_event(p.id(), uint32_t(initial), flags & kEfdSemaphore, &id); st != UnixStatus::Ok) return result_of(st);
  return give_fd(p, *m, id, flags & kSockCloexec, flags & kSockNonBlock);
}

namespace {

// An eventfd's read (8 bytes: the counter, which waits while it is 0) and write (8 bytes added, which waits while they would not fit).
bool is_event(const Sock& s) {
  Hold hold(*s.m);
  return s.m->is_event_locked(s.id);
}

uint64_t event_read(Process& p, const Sock& s, uint64_t buffer, uint64_t count) {
  if (count < 8) return error(kEinval);
  uint64_t value = 0;
  const int64_t r = run_blocking(p, s, false, false, [&] { return s.m->event_take_locked(s.id, &value); });
  if (r < 0) return error(-r);
  uint8_t bytes[8];
  for (uint32_t i = 0; i < 8; ++i) bytes[i] = uint8_t(value >> (8 * i));
  return p.write_user(buffer, ConstByteView(bytes, 8)) ? 8 : error(kEfault);      // (the value is gone if the buffer is bad, as on Linux)
}

uint64_t event_write(Process& p, const Sock& s, uint64_t buffer, uint64_t count) {
  if (count < 8) return error(kEinval);
  uint8_t bytes[8];
  if (!p.read_user(buffer, ByteView(bytes, 8))) return error(kEfault);
  uint64_t value = 0;
  for (uint32_t i = 0; i < 8; ++i) value |= uint64_t(bytes[i]) << (8 * i);
  if (value == ~uint64_t{0}) return error(kEinval);
  const int64_t r = run_blocking(p, s, true, false, [&] { return s.m->event_add_locked(s.id, value); });
  return r < 0 ? error(-r) : 8;
}

}  // namespace

uint64_t socket_read(Process& p, uint64_t fd, uint64_t buffer, uint64_t count) {
  Sock s;
  if (socket_of(p, fd, &s) == 0 && is_event(s)) return event_read(p, s, buffer, count);
  return sys_recvfrom(p, fd, buffer, count, 0, 0, 0);
}
uint64_t socket_write(Process& p, uint64_t fd, uint64_t buffer, uint64_t count) {
  Sock s;
  if (socket_of(p, fd, &s) == 0 && is_event(s)) return event_write(p, s, buffer, count);
  return sys_sendto(p, fd, buffer, count, 0, 0, 0);
}

uint64_t socket_vector_io(Process& p, uint64_t fd, uint64_t iov, uint64_t count, bool write) {
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  ByteBuffer raw(s.m->allocator());
  uint64_t total = 0;
  if (const int64_t r = read_iovecs(p, iov, count, &raw, &total); r < 0) return error(r == -kEmsgsize ? kEinval : -r);
  if (write) {
    const int64_t r = send_core(p, s, raw.view(), total, nullptr, 0, nullptr);
    return r < 0 ? error(-r) : uint64_t(r);
  }
  ReceiveResult got;
  const int64_t r = receive_core(p, s, raw.view(), total, 0, nullptr, &got);
  return r < 0 ? error(-r) : got.got;
}

uint64_t socket_ioctl(Process& p, uint64_t fd, uint64_t request, uint64_t argument) {
  Sock s;
  if (const int64_t r = socket_of(p, fd, &s); r < 0) return error(-r);
  constexpr uint64_t kFionread = 0x541B, kFionbio = 0x5421, kSiocoutq = 0x5411;
  switch (request) {
    case kFionread: {
      uint32_t n = 0;
      {
        Hold hold(*s.m);
        n = s.m->readable_bytes_locked(s.id);
      }
      return p.write_user_u32(argument, n) ? 0 : error(kEfault);
    }
    case kSiocoutq: return p.write_user_u32(argument, 0) ? 0 : error(kEfault);      // (sent bytes are in the peer's queue at once)
    case kFionbio: {
      uint32_t on = 0;
      if (!read_u32(p, argument, &on)) return error(kEfault);
      Hold hold(*s.m);
      s.m->set_non_blocking_locked(s.id, on != 0);
      return 0;
    }
    default: return error(kEnotty);
  }
}

bool socket_non_blocking(Process& p, uint64_t fd) {
  Sock s;
  if (socket_of(p, fd, &s) < 0) return false;
  Hold hold(*s.m);
  return s.m->non_blocking_locked(s.id);
}

void set_socket_non_blocking(Process& p, uint64_t fd, bool on) {
  Sock s;
  if (socket_of(p, fd, &s) < 0) return;
  Hold hold(*s.m);
  s.m->set_non_blocking_locked(s.id, on);
}

bool is_socket_file(StringView host_path) {
  UnixSocketManager* m = system_unix_sockets();
  return m && m->path_bound(host_path);
}

}  // namespace process::linuxabi
