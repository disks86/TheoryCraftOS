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
#include "Process/Process.hpp"
#include "Process/Objects.hpp"
#include "Process/Sockets.hpp"
#include "Process/UnixSockets.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"

// AF_INET and AF_INET6 sockets for Linux programs: TCP (SOCK_STREAM) and UDP (SOCK_DGRAM) over the system's network stacks, through the same socket manager
// Windows programs' sockets use (Sockets.hpp). A socket is a descriptor of kind Socket whose token is the manager's id, so dup(), fork() and close() treat
// it as they treat a Windows program's socket handle.
//
// The manager tries an operation once and says whether it would have had to wait; waiting is done here: until the network stacks have made progress, a
// signal arrives for the thread (EINTR), or the socket's timeout (SO_RCVTIMEO, SO_SNDTIMEO) runs out (EAGAIN).
//
// A Linux AF_INET6 socket takes IPv4 too unless IPV6_V6ONLY is set (the other way round from Windows), and shows IPv4 peers as ::ffff:a.b.c.d.
namespace process::linuxabi {

namespace {

constexpr int64_t kEintr = 4, kEbadf = 9, kEagain = 11, kEnomem = 12, kEfault = 14, kEinval = 22, kEnfile = 23, kEmfile = 24, kEpipe = 32, kEnotsock = 88,
                  kEdestaddrreq = 89, kEmsgsize = 90, kEprotonosupport = 93, kEsocktnosupport = 94, kEopnotsupp = 95, kEafnosupport = 97, kEaddrinuse = 98,
                  kEnetunreach = 101, kEconnreset = 104, kEnobufs = 105, kEisconn = 106, kEnotconn = 107, kEtimedout = 110, kEconnrefused = 111, kEalready = 114,
                  kEinprogress = 115, kEnoprotoopt = 92;
uint64_t error(int64_t e) { return uint64_t(-e); }

constexpr uint64_t kAfInet = 2, kAfInet6 = 10;
constexpr uint64_t kSockStream = 1, kSockDgram = 2, kSockNonBlock = 0x800, kSockCloexec = 0x80000;
constexpr uint64_t kMsgPeek = 2, kMsgCtrunc = 8, kMsgTrunc = 0x20, kMsgDontWait = 0x40, kMsgWaitAll = 0x100, kMsgErrQueue = 0x2000;
constexpr uint32_t kAddress4Bytes = 16, kAddress6Bytes = 28;
constexpr uint64_t kMostAtOnce = 16384;      // what one call of the manager moves
constexpr uint64_t kLargestDatagram = 65507;
constexpr uint64_t kWaitSliceUs = 2000;

// An NTSTATUS of the socket manager as an errno.
int64_t errno_of(uint32_t status) {
  switch (status) {
    case 0xC0000008u: return kEbadf;
    case 0xC000000Du: return kEinval;
    case 0xC0000236u: return kEconnrefused;
    case 0xC000020Au: return kEaddrinuse;
    case 0xC0000050u: return kEconnreset;
    case 0xC000000Eu: return kEnetunreach;      // no interface can reach it
    case 0xC0000140u: return kEnotconn;
    case 0xC0000209u: return kEnobufs;
    case 0xC0000141u: return kEafnosupport;     // an address the socket's family cannot carry
    case 0xC000009Au: return kEnfile;
    default: return kEinval;
  }
}

struct Inet {
  SocketManager* sockets = nullptr;
  uint32_t id = 0;
};

// The socket behind `fd`: 0, or -errno (EBADF, ENOTSOCK).
int64_t inet_of(Process& p, uint64_t fd, Inet* out) {
  const DescriptorKind kind = p.files().kind(fd);
  if (kind == DescriptorKind::Closed) return -kEbadf;
  if (kind != DescriptorKind::Socket) return -kEnotsock;
  out->sockets = system_sockets();
  out->id = uint32_t(p.files().token(fd));
  return out->sockets && out->sockets->exists(out->id) ? 0 : -kEbadf;
}

// struct sockaddr_in (16 bytes): family 2, port (network order), 4 address bytes, 8 zero bytes. struct sockaddr_in6 (28 bytes): family 10, port, flow
// information, 16 address bytes, scope id. 0, or -errno.
int64_t read_address(Process& p, uint64_t at, uint64_t length, bool ipv6_socket, network::IpAddress* address, uint16_t* port) {
  if (length < 2) return -kEinval;
  uint8_t raw[kAddress6Bytes] = {};
  const uint64_t n = length < sizeof(raw) ? length : sizeof(raw);
  if (!p.read_user(at, ByteView(raw, n))) return -kEfault;
  ByteReader in{ConstByteView(raw, n)};
  const uint16_t family = in.le16();
  if (family != (ipv6_socket ? kAfInet6 : kAfInet)) return -kEafnosupport;
  if (n < (ipv6_socket ? kAddress6Bytes : kAddress4Bytes)) return -kEinval;
  *port = in.be16();
  if (!ipv6_socket) {
    *address = network::IpAddress(network::Ipv4Address::from_value(in.be32()));
    return 0;
  }
  network::Ipv6Address v6;
  in.skip(4);
  if (!in.read_into(ByteView(v6.bytes))) return -kEinval;
  *address = network::IpAddress(v6);
  return 0;
}

uint32_t format_address(ByteView out, const network::IpAddress& address, uint16_t port) {
  ByteWriter w(out);
  if (address.v6) {
    w.le16(uint16_t(kAfInet6));
    w.be16(port);
    w.le32(0);
    w.bytes(ConstByteView(address.ipv6.bytes));
    w.le32(0);
  } else {
    w.le16(uint16_t(kAfInet));
    w.be16(port);
    w.be32(address.ipv4.value());
    w.zeros(8);
  }
  return uint32_t(w.written().size());
}

// An address for the program: as much of it as its buffer holds (`length_at`: the buffer's size going in, the address's whole size coming out), as Linux
// does. A null buffer wants none.
int64_t write_address(Process& p, uint64_t at, uint64_t length_at, const network::IpAddress& address, uint16_t port) {
  if (at == 0 || length_at == 0) return 0;
  uint8_t raw[4];
  if (!p.read_user(length_at, ByteView(raw, sizeof(raw)))) return -kEfault;
  ByteReader in{ConstByteView(raw, sizeof(raw))};
  const uint32_t capacity = in.le32();
  if (int32_t(capacity) < 0) return -kEinval;
  uint8_t bytes[kAddress6Bytes];
  const uint32_t n = format_address(ByteView(bytes, sizeof(bytes)), address, port);
  const uint32_t given = n < capacity ? n : capacity;
  if (given != 0 && !p.write_user(at, ConstByteView(bytes, given))) return -kEfault;
  ByteWriter w{ByteView(raw, sizeof(raw))};
  w.le32(n);
  return p.write_user(length_at, ConstByteView(raw, sizeof(raw))) ? 0 : -kEfault;
}

// Tries `attempt` (one of the manager's operations; it says in *would_block that it was not ready) until it does not have to wait any more. 0 with the
// attempt's NTSTATUS in *status, or -errno: EAGAIN (a socket or a call that does not wait, or the timeout ran out), EINTR.
template <typename Attempt>
int64_t wait_for(Process& p, const Inet& s, bool dont_wait, uint64_t timeout_ns, uint32_t* status, Attempt&& attempt) {
  IProcessRuntime* runtime = p.runtime();
  const uint64_t started = p.host().monotonic_ns();
  for (;;) {
    bool would_block = false;
    const uint64_t seen = s.sockets->progress();      // (read before the attempt: what happens after it ends the wait below)
    *status = attempt(&would_block);
    if (*status != 0 || !would_block) return 0;
    if (dont_wait || s.sockets->is_non_blocking(s.id)) return -kEagain;
    if (timeout_ns != 0 && p.host().monotonic_ns() - started >= timeout_ns) return -kEagain;
    if (p.exited() || (runtime && runtime->interrupted()) || p.signal_interrupts()) return -kEintr;
    if (runtime) runtime->unlock();
    if (!s.sockets->wait_for_progress(seen, kWaitSliceUs)) p.host().sleep_ns(kWaitSliceUs * 1000);
    if (runtime) runtime->lock();
    if (!s.sockets->exists(s.id)) return -kEbadf;      // (closed by another thread meanwhile)
  }
}

uint64_t give_descriptor(Process& p, SocketManager& sockets, uint32_t id, bool close_on_exec) {
  const int64_t slot = p.files().open_token(DescriptorKind::Socket, id);
  if (slot < 0) {
    sockets.release(id);
    return error(kEmfile);
  }
  p.files().set_close_on_exec(uint64_t(slot), close_on_exec);
  return uint64_t(slot);
}

// The bytes of an array of struct iovec (base, length: 8 bytes each), gathered into `out`. 0, or -errno.
int64_t gather(Process& p, uint64_t vectors, uint64_t count, uint64_t limit, ByteBuffer* out) {
  if (count > 1024) return -kEmsgsize;
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t base = 0, length = 0;
    if (!p.read_user_u64(vectors + i * 16, &base) || !p.read_user_u64(vectors + i * 16 + 8, &length)) return -kEfault;
    if (length > limit - out->size()) return -kEmsgsize;
    const uint64_t before = out->size();
    if (!out->put_zeros(length)) return -kEnomem;
    if (length != 0 && !p.read_user(base, out->writable_view().subview(before, length))) return -kEfault;
  }
  return 0;
}

// The room an array of struct iovec has in all (up to `limit`). -errno if it cannot be read.
int64_t room_of(Process& p, uint64_t vectors, uint64_t count, uint64_t limit) {
  if (count > 1024) return -kEmsgsize;
  uint64_t total = 0;
  for (uint64_t i = 0; i < count; ++i) {
    uint64_t length = 0;
    if (!p.read_user_u64(vectors + i * 16 + 8, &length)) return -kEfault;
    total = length > limit - total ? limit : total + length;
  }
  return int64_t(total);
}

// `data` spread over the vectors, in order. False if one cannot be written.
bool scatter(Process& p, uint64_t vectors, uint64_t count, ConstByteView data) {
  uint64_t done = 0;
  for (uint64_t i = 0; i < count && done < data.size(); ++i) {
    uint64_t base = 0, length = 0;
    if (!p.read_user_u64(vectors + i * 16, &base) || !p.read_user_u64(vectors + i * 16 + 8, &length)) return false;
    const uint64_t n = length < data.size() - done ? length : data.size() - done;
    if (n != 0 && !p.write_user(base, data.subview(done, n))) return false;
    done += n;
  }
  return true;
}

allocators::IAllocator* call_memory(Process& p);

// Sends `data`: all of it over TCP unless the call does not wait (then what there is room for), one datagram over UDP. The count sent, or -errno.
int64_t send_bytes(Process& p, const Inet& s, ConstByteView data, uint64_t flags, const network::IpAddress* to, const uint16_t* to_port) {
  SocketManager::Options* options = s.sockets->options(s.id);
  if (!options) return -kEbadf;
  if (options->shut_write) return -kEpipe;
  const bool dont_wait = (flags & kMsgDontWait) != 0;
  const uint64_t timeout = options->send_timeout_ns;
  if (!s.sockets->is_tcp(s.id)) {
    if (data.size() > kLargestDatagram) return -kEmsgsize;
    if (!to && s.sockets->state(s.id) != SocketManager::State::Connected) return -kEdestaddrreq;
    // (the stack sends no fragments: a datagram has to fit one frame)
    if (data.size() > 1472) return -kEmsgsize;
    uint32_t status = 0, sent = 0;
    if (const int64_t r = wait_for(p, s, dont_wait, timeout, &status, [&](bool* would_block) { return s.sockets->send(s.id, data, to, to_port, &sent, would_block); }); r < 0) return r;
    return status != 0 ? -errno_of(status) : int64_t(sent);
  }
  if (to && s.sockets->state(s.id) == SocketManager::State::Connected) to = nullptr;      // (an address given to a connected stream socket is not looked at)
  switch (s.sockets->state(s.id)) {
    case SocketManager::State::Connected: break;
    case SocketManager::State::Failed: return -(options->connect_started ? kEconnrefused : kEconnreset);
    case SocketManager::State::Connecting: return -kEagain;      // (not yet: Linux says EAGAIN for a send on a socket whose connect is in progress)
    default: return -kEpipe;      // (a stream socket nobody connected: EPIPE for a writer, as on Linux)
  }
  uint64_t done = 0;
  while (done < data.size()) {
    const uint64_t n = data.size() - done < kMostAtOnce ? data.size() - done : kMostAtOnce;
    uint32_t status = 0, sent = 0;
    const int64_t r = wait_for(p, s, dont_wait, timeout, &status, [&](bool* would_block) { return s.sockets->send(s.id, data.subview(done, n), nullptr, nullptr, &sent, would_block); });
    if (r < 0) return done != 0 ? int64_t(done) : r;
    if (status != 0) return done != 0 ? int64_t(done) : -(status == 0xC0000050u ? kEpipe : errno_of(status));      // (the connection is gone: EPIPE for a writer)
    done += sent;
    if (sent < n && (dont_wait || s.sockets->is_non_blocking(s.id))) break;
  }
  return int64_t(done);
}

// Receives into `out` (cleared first): up to `room` bytes of the stream (all of them with MSG_WAITALL), or one datagram (cut to `room`). The count, 0 at the
// end of a connection, or -errno. *from and *from_port say who sent it (a datagram).
int64_t receive_bytes(Process& p, const Inet& s, uint64_t room, uint64_t flags, ByteBuffer* out, network::IpAddress* from, uint16_t* from_port,
                      uint32_t* whole = nullptr) {
  SocketManager::Options* options = s.sockets->options(s.id);
  if (!options) return -kEbadf;
  out->clear();
  uint32_t datagram = 0;
  if (whole) *whole = 0;
  const bool tcp = s.sockets->is_tcp(s.id);
  if (tcp) {
    switch (s.sockets->state(s.id)) {
      case SocketManager::State::Connected: break;
      case SocketManager::State::Failed: return -(options->connect_started ? kEconnrefused : kEconnreset);
      case SocketManager::State::Connecting: return -kEagain;
      default: return -kEnotconn;
    }
  }
  if (options->shut_read || room == 0) return 0;
  const bool dont_wait = (flags & kMsgDontWait) != 0;
  if (flags & kMsgPeek) {      // looked at and left: the manager holds it for the receive that takes it (one go: MSG_WAITALL does not make a peek wait for more)
    allocators::IAllocator* allocator = call_memory(p);
    if (!allocator) return -kEnomem;
    if (room > SocketManager::kMostHeld) room = SocketManager::kMostHeld;
    if (!out->put_zeros(room)) return -kEnomem;
    uint32_t status = 0, got = 0;
    const int64_t r = wait_for(p, s, dont_wait, options->receive_timeout_ns, &status, [&](bool* would_block) {
      bool closed = false;
      return s.sockets->peek(s.id, out->writable_view(), &got, from, from_port, would_block, &closed, *allocator, &datagram);
    });
    if (r < 0) return r;
    if (status != 0) return -errno_of(status);
    out->truncate(got);
    if (whole) *whole = datagram;
    return int64_t(got);
  }
  if (room > (tcp ? uint64_t{1} << 20 : kLargestDatagram)) room = tcp ? uint64_t{1} << 20 : kLargestDatagram;
  if (!out->put_zeros(tcp ? room : kLargestDatagram < 2048 ? kLargestDatagram : (room < 2048 ? 2048 : room))) return -kEnomem;
  uint64_t done = 0;
  for (;;) {
    const uint64_t want = tcp ? (room - done < kMostAtOnce ? room - done : kMostAtOnce) : out->size();
    uint32_t status = 0, got = 0;
    bool closed = false;
    const int64_t r = wait_for(p, s, dont_wait, options->receive_timeout_ns, &status,
                               [&](bool* would_block) { return s.sockets->receive(s.id, out->writable_view().subview(done, want), &got, from, from_port, would_block, &closed, &datagram); });
    if (r < 0) {
      if (done == 0) return r;
      break;
    }
    if (status != 0) {
      if (done == 0) return -errno_of(status);
      break;
    }
    done += got;
    if (!tcp) {
      if (whole) *whole = datagram;
      if (done > room) done = room;      // (the rest of a datagram that does not fit is lost, as on Linux)
      break;
    }
    if (closed || done == room || !(flags & kMsgWaitAll)) break;
  }
  out->truncate(done);
  return int64_t(done);
}

// The address a call was given to send to (a null one: none). 0, or -errno.
int64_t destination_of(Process& p, const Inet& s, uint64_t address, uint64_t length, network::IpAddress* to, uint16_t* port, bool* has) {
  *has = address != 0 && length != 0;
  if (!*has) return 0;
  return read_address(p, address, length, s.sockets->is_ipv6(s.id), to, port);
}

// An allocator for a call's own buffers: the one the process's files use, or the system's.
allocators::IAllocator* call_memory(Process& p) {
  if (p.files().attached()) return &p.files().allocator();
  if (UnixSocketManager* unix_sockets = system_unix_sockets()) return &unix_sockets->allocator();
  if (ObjectManager* objects = system_objects()) return objects->allocator();
  return nullptr;
}

uint64_t put_u32(Process& p, uint64_t at, uint32_t value) {
  uint8_t raw[4];
  ByteWriter w{ByteView(raw, sizeof(raw))};
  w.le32(value);
  return p.write_user(at, ConstByteView(raw, sizeof(raw))) ? 0 : error(kEfault);
}

bool get_u32(Process& p, uint64_t at, uint32_t* value) {
  uint8_t raw[4];
  if (!p.read_user(at, ByteView(raw, sizeof(raw)))) return false;
  ByteReader in{ConstByteView(raw, sizeof(raw))};
  *value = in.le32();
  return true;
}

}  // namespace

bool is_inet_descriptor(Process& p, uint64_t fd) { return p.files().kind(fd) == DescriptorKind::Socket; }

// socket(AF_INET or AF_INET6, SOCK_STREAM or SOCK_DGRAM [| SOCK_NONBLOCK | SOCK_CLOEXEC], 0 or the type's own protocol)
uint64_t inet_socket(Process& p, uint64_t domain, uint64_t type, uint64_t protocol) {
  const uint64_t base = type & 0xF, extra = type & ~uint64_t{0xF};
  if (extra & ~(kSockNonBlock | kSockCloexec)) return error(kEinval);
  if (base != kSockStream && base != kSockDgram) return error(base == 3 /* SOCK_RAW */ || base == 5 || base == 4 || base == 10 ? kEsocktnosupport : kEinval);
  if (protocol != 0 && protocol != (base == kSockStream ? 6u : 17u)) return error(kEprotonosupport);      // IPPROTO_TCP, IPPROTO_UDP (no ICMP sockets)
  SocketManager* sockets = system_sockets();
  if (!sockets) return error(kEafnosupport);      // (no network interface, so no network stack yet)
  const SocketManager::Created made = sockets->create(base == kSockStream ? SocketProto::Tcp : SocketProto::Udp, domain == kAfInet6);
  if (made.id == 0) return error(kEnfile);
  if (domain == kAfInet6) sockets->set_v6_only(made.id, false);      // (Linux's default: an IPv6 socket takes IPv4 too)
  if (extra & kSockNonBlock) sockets->set_non_blocking(made.id, true);
  return give_descriptor(p, *sockets, made.id, (extra & kSockCloexec) != 0);
}

uint64_t inet_bind(Process& p, uint64_t fd, uint64_t address, uint64_t length) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  network::IpAddress local;
  uint16_t port = 0;
  if (const int64_t r = read_address(p, address, length, s.sockets->is_ipv6(s.id), &local, &port); r < 0) return error(-r);
  if (s.sockets->is_bound(s.id)) return error(kEinval);      // (bound already)
  const uint32_t status = s.sockets->bind(s.id, local, port);
  if (status != 0) return error(errno_of(status));
  SocketManager::Options* options = s.sockets->options(s.id);
  options->has_bound_address = true;      // (every interface's own address is used whatever was named; the name is what the program reads back)
  options->bound_address = local;
  return 0;
}

// connect(): a datagram socket only remembers its peer. A stream socket's handshake takes time: a socket that does not wait answers EINPROGRESS (and EALREADY
// if asked again meanwhile) and turns writable when it is over, with the outcome in SO_ERROR.
uint64_t inet_connect(Process& p, uint64_t fd, uint64_t address, uint64_t length) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  network::IpAddress to;
  uint16_t port = 0;
  // An address of no family (AF_UNSPEC) takes a datagram socket's peer away again, which is how a C library uses one socket to try several hosts.
  if (length >= 2) {
    uint8_t family[2];
    if (!p.read_user(address, ByteView(family, sizeof(family)))) return error(kEfault);
    if (family[0] == 0 && family[1] == 0) return s.sockets->disconnect(s.id) ? 0 : error(kEafnosupport);
  }
  if (const int64_t r = read_address(p, address, length, s.sockets->is_ipv6(s.id), &to, &port); r < 0) return error(-r);
  SocketManager::Options* options = s.sockets->options(s.id);
  if (s.sockets->is_tcp(s.id)) {
    switch (s.sockets->state(s.id)) {
      case SocketManager::State::Listening: return error(kEisconn);
      case SocketManager::State::Connected: return error(kEisconn);
      case SocketManager::State::Connecting: if (s.sockets->is_non_blocking(s.id)) return error(kEalready); break;
      case SocketManager::State::Failed: return error(kEconnrefused);
      default: break;
    }
  }
  options->connect_started = true;
  uint32_t status = 0;
  const int64_t r = wait_for(p, s, false, options->send_timeout_ns, &status, [&](bool* would_block) { return s.sockets->connect(s.id, to, port, would_block); });
  if (r == -kEagain) return error(s.sockets->is_non_blocking(s.id) ? kEinprogress : kEtimedout);
  if (r < 0) return error(-r);
  if (status != 0) return error(errno_of(status));
  return 0;
}

uint64_t inet_listen(Process& p, uint64_t fd, uint64_t backlog) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  if (!s.sockets->is_tcp(s.id)) return error(kEopnotsupp);
  if (s.sockets->state(s.id) == SocketManager::State::Listening) return 0;
  if (s.sockets->state(s.id) != SocketManager::State::Unconnected) return error(kEinval);
  const uint32_t status = s.sockets->listen(s.id, uint32_t(backlog));
  return status == 0 ? 0 : error(errno_of(status));
}

uint64_t inet_accept4(Process& p, uint64_t fd, uint64_t address, uint64_t length_at, uint64_t flags) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  if (flags & ~(kSockNonBlock | kSockCloexec)) return error(kEinval);
  if (!s.sockets->is_tcp(s.id)) return error(kEopnotsupp);
  if (s.sockets->state(s.id) != SocketManager::State::Listening) return error(kEinval);
  if (address != 0 && length_at != 0) {      // (looked at before a connection is taken, so that a bad one loses none)
    uint32_t capacity = 0;
    if (!get_u32(p, length_at, &capacity)) return error(kEfault);
    if (int32_t(capacity) < 0) return error(kEinval);
  }
  uint32_t status = 0, accepted = 0;
  network::IpAddress peer;
  uint16_t peer_port = 0;
  const int64_t r = wait_for(p, s, false, s.sockets->options(s.id)->receive_timeout_ns, &status,
                             [&](bool* would_block) { return s.sockets->accept(s.id, &accepted, &peer, &peer_port, would_block); });
  if (r < 0) return error(-r);
  if (status != 0) return error(errno_of(status));
  if (flags & kSockNonBlock) s.sockets->set_non_blocking(accepted, true);
  const uint64_t new_fd = give_descriptor(p, *s.sockets, accepted, (flags & kSockCloexec) != 0);
  if (int64_t(new_fd) < 0) return new_fd;
  if (const int64_t w = write_address(p, address, length_at, peer, peer_port); w < 0) {
    p.close_descriptor(new_fd);
    return error(-w);
  }
  return new_fd;
}

uint64_t inet_sendto(Process& p, uint64_t fd, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address, uint64_t address_length) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  network::IpAddress to;
  uint16_t port = 0;
  bool has = false;
  if (const int64_t r = destination_of(p, s, address, address_length, &to, &port, &has); r < 0) return error(-r);
  const bool tcp = s.sockets->is_tcp(s.id);
  if (!tcp && length > kLargestDatagram) return error(kEmsgsize);
  if (length > 0x7FFFF000u) length = 0x7FFFF000u;
  // The program's bytes are moved in pieces, so that a large send of a stream needs no buffer of its size.
  allocators::IAllocator* allocator = call_memory(p);
  if (!allocator) return error(kEnomem);
  ByteBuffer piece(*allocator);
  uint64_t done = 0;
  do {
    const uint64_t n = tcp ? (length - done < 65536 ? length - done : 65536) : length;
    piece.clear();
    if (!piece.put_zeros(n)) return done != 0 ? done : error(kEnomem);
    if (n != 0 && !p.read_user(buffer + done, piece.writable_view())) return done != 0 ? done : error(kEfault);
    const int64_t sent = send_bytes(p, s, piece.view(), flags, has ? &to : nullptr, has ? &port : nullptr);
    if (sent < 0) return done != 0 ? done : error(-sent);
    done += uint64_t(sent);
    if (uint64_t(sent) < n) break;
  } while (tcp && done < length);
  return done;
}

uint64_t inet_recvfrom(Process& p, uint64_t fd, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address, uint64_t length_at) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  if (length > 0x7FFFF000u) length = 0x7FFFF000u;
  allocators::IAllocator* allocator = call_memory(p);
  if (!allocator) return error(kEnomem);
  ByteBuffer data(*allocator);
  network::IpAddress from;
  uint16_t from_port = 0;
  if (flags & kMsgErrQueue) return error(kEagain);      // (the queue of errors that came back for what was sent: nothing is ever in it)
  uint32_t whole = 0;
  const int64_t got = receive_bytes(p, s, length, flags, &data, &from, &from_port, &whole);
  if (got < 0) return error(-got);
  const bool tcp = s.sockets->is_tcp(s.id);
  // (MSG_TRUNC on a stream: the bytes are taken off it and not handed over)
  if (got != 0 && !(tcp && (flags & kMsgTrunc)) && !p.write_user(buffer, data.view())) return error(kEfault);
  if (!tcp) {
    if (const int64_t w = write_address(p, address, length_at, from, from_port); w < 0) return error(-w);
    if (flags & kMsgTrunc) return whole;      // (MSG_TRUNC asks for the datagram's real size, also when the buffer was too small for it)
  }
  return uint64_t(got);
}

// --- control messages (ancillary data) -------------------------------------------------------------------------------------------------------------------
// struct cmsghdr: cmsg_len (8: the header and the data), cmsg_level (4), cmsg_type (4), then the data; the next one starts at a multiple of 8.
//
// Received with a datagram, when the socket asked for them: where the packet arrived (IP_PKTINFO, IPV6_PKTINFO), its TTL or hop limit, its TOS or traffic
// class, and when it came (SO_TIMESTAMP, SO_TIMESTAMPNS). The network stack hands a datagram over with its sender and nothing else, so: the address it was
// sent to is the socket's interface's own (or the loopback), which is right except for a broadcast or multicast; the TTL is reported as 64 and the TOS as
// 0; the time is when the program received it.
//
// Sent with a message: the ones a program may pass (a source address or interface, a TTL, a TOS) are checked and taken, and change nothing: there is one
// address to send from on each interface, and the stack sets the rest.

namespace {

constexpr uint64_t kControlHead = 16;
constexpr uint32_t kLevelSocket = 1, kLevelIp = 0, kLevelIpv6 = 41;
constexpr uint32_t kIpTos = 1, kIpTtl = 2, kIpPktInfo = 8, kIpRecvTtl = 12, kIpRecvTos = 13, kIpv6RecvPktInfo = 49, kIpv6PktInfo = 50, kIpv6RecvHopLimit = 51,
                   kIpv6HopLimit = 52, kIpv6RecvTclass = 66, kIpv6Tclass = 67, kScmRights = 1, kScmCredentials = 2, kScmTimestamp = 29, kScmTimestampNs = 35;

bool is_mapped(const network::IpAddress& a) {
  if (!a.v6) return false;
  for (int i = 0; i < 10; ++i)
    if (a.ipv6.bytes[i] != 0) return false;
  return a.ipv6.bytes[10] == 0xFF && a.ipv6.bytes[11] == 0xFF;
}

// One control message at the end of `out` if `capacity` has room for all of it; else *cut (MSG_CTRUNC: the program's buffer was too small for what there was).
bool add_control(ByteBuffer& out, uint64_t capacity, uint32_t level, uint32_t type, ConstByteView data, bool* cut) {
  // As Linux fills the buffer: a message that does not fit whole is cut short (its length says by how much) when at least its header fits, and nothing
  // follows it; MSG_CTRUNC tells the program either way.
  if (*cut) return true;
  const uint64_t room = out.size() < capacity ? capacity - out.size() : 0;
  uint64_t length = kControlHead + data.size();
  if (room < kControlHead) {
    *cut = true;
    return true;
  }
  if (length > room) {
    *cut = true;
    length = room;
  }
  uint64_t padded = (length + 7) & ~uint64_t{7};
  if (padded > room) padded = room;
  return out.put32(uint32_t(length)) && out.put32(0) && out.put32(level) && out.put32(type) && out.append(data.first(length - kControlHead)) && out.put_zeros(padded - length);
}

// What comes with a datagram from `from`, as the socket's options ask. False without memory.
bool build_control(Process& p, const Inet& s, const network::IpAddress& from, uint64_t capacity, ByteBuffer* out, bool* cut) {
  const SocketManager::Options* options = s.sockets->options(s.id);
  if (!options) return true;
  uint8_t raw[24];
  if (options->timestamp != 0) {      // struct timeval (seconds, microseconds) or struct timespec (seconds, nanoseconds)
    const uint64_t now = p.host().realtime_ns();
    ByteWriter w{ByteView(raw, 16)};
    w.le64(now / 1000000000u);
    w.le64(options->timestamp == 1 ? now % 1000000000u / 1000u : now % 1000000000u);
    if (!add_control(*out, capacity, kLevelSocket, options->timestamp == 1 ? kScmTimestamp : kScmTimestampNs, ConstByteView(raw, 16), cut)) return false;
  }
  const bool any = options->packet_info || options->packet_info6 || options->receive_ttl || options->receive_tos || options->receive_hop_limit || options->receive_traffic_class;
  if (!any) return true;
  network::IpAddress destination;
  uint32_t interface_index = 0;
  if (!s.sockets->arrival(s.id, from, &destination, &interface_index)) return true;
  const bool version4 = !from.v6 || is_mapped(from);
  // (the order is Linux's: an IPv6 socket's own message first, then the ones of the packet's kind)
  if (s.sockets->is_ipv6(s.id) && options->packet_info6 && destination.v6) {      // struct in6_pktinfo: ipi6_addr, ipi6_ifindex (an IPv4 packet's address is shown mapped)
    ByteWriter w{ByteView(raw, 20)};
    w.bytes(ConstByteView(destination.ipv6.bytes));
    w.le32(interface_index);
    if (!add_control(*out, capacity, kLevelIpv6, kIpv6PktInfo, ConstByteView(raw, 20), cut)) return false;
  }
  if (version4) {
    if (options->packet_info) {      // struct in_pktinfo: ipi_ifindex, ipi_spec_dst, ipi_addr
      ByteWriter w{ByteView(raw, 12)};
      w.le32(interface_index);
      for (int twice = 0; twice < 2; ++twice) {
        if (destination.v6) w.bytes(ConstByteView(destination.ipv6.bytes).subview(12, 4));
        else w.be32(destination.ipv4.value());
      }
      if (!add_control(*out, capacity, kLevelIp, kIpPktInfo, ConstByteView(raw, 12), cut)) return false;
    }
    if (options->receive_ttl) {
      ByteWriter w{ByteView(raw, 4)};
      w.le32(64);
      if (!add_control(*out, capacity, kLevelIp, kIpTtl, ConstByteView(raw, 4), cut)) return false;
    }
    if (options->receive_tos) {
      raw[0] = 0;
      if (!add_control(*out, capacity, kLevelIp, kIpTos, ConstByteView(raw, 1), cut)) return false;
    }
    return true;
  }
  if (!s.sockets->is_ipv6(s.id)) return true;
  if (options->receive_hop_limit) {
    ByteWriter w{ByteView(raw, 4)};
    w.le32(64);
    if (!add_control(*out, capacity, kLevelIpv6, kIpv6HopLimit, ConstByteView(raw, 4), cut)) return false;
  }
  if (options->receive_traffic_class) {
    ByteWriter w{ByteView(raw, 4)};
    w.le32(0);
    if (!add_control(*out, capacity, kLevelIpv6, kIpv6Tclass, ConstByteView(raw, 4), cut)) return false;
  }
  return true;
}

// The control messages a program passes to sendmsg(), judged as Linux judges them: each has to be whole; one of the socket's own family or of the socket
// level has to be one that is known, of the right size and with a value in range; one of another level is passed over. What they ask for (a source
// address, a hop count for this one datagram) is not acted on: the datagram goes out the usual way. 0, or -errno.
int64_t check_control(Process& p, const Inet& s, uint64_t control, uint64_t length, allocators::IAllocator& allocator) {
  if (control == 0 || length == 0) return 0;
  if (length > 20480) return -kEnobufs;      // (more than a socket may be handed at once)
  ByteBuffer raw(allocator);
  if (!raw.put_zeros(length)) return -kEnomem;
  if (!p.read_user(control, raw.writable_view())) return -kEfault;
  const ConstByteView all = raw.view();
  const bool ipv6 = s.sockets->is_ipv6(s.id);
  // a 32-bit number in the data, when the data is exactly that
  auto number = [](ConstByteView data, int64_t* out) {
    if (data.size() != 4) return false;
    ByteReader in{data};
    *out = int32_t(in.le32());
    return true;
  };
  for (uint64_t at = 0; all.size() - at >= kControlHead;) {      // (less than a header at the end is not a message)
    ByteReader in{all.subview(at, kControlHead)};
    const uint64_t size = in.le64();
    const uint32_t level = in.le32(), type = in.le32();
    if (size < kControlHead || size > all.size() - at) return -kEinval;
    const ConstByteView data = all.subview(at + kControlHead, size - kControlHead);
    int64_t value = 0;
    if (level == kLevelSocket) {
      // SCM_RIGHTS and SCM_CREDENTIALS mean something over AF_UNIX only and are passed over here; SO_MARK and SO_TIMESTAMPING are a 32-bit number,
      // SCM_TXTIME a 64-bit one
      if (type == kScmRights || type == kScmCredentials) {}
      else if (type == 36 || type == 37) { if (data.size() != 4) return -kEinval; }
      else if (type == 61) { if (data.size() != 8) return -kEinval; }
      else return -kEinval;
    } else if (level == kLevelIp && !ipv6) {
      if (type == kIpPktInfo) { if (data.size() != 12) return -kEinval; }
      else if (type == kIpTtl) { if (!number(data, &value) || value < 1 || value > 255) return -kEinval; }
      else if (type == kIpTos) { if (data.size() != 1 && (!number(data, &value) || value < 0 || value > 255)) return -kEinval; }
      else if (type == 7 /* IP_RETOPTS */) { if (data.size() > 40) return -kEinval; }
      else return -kEinval;
    } else if (level == kLevelIpv6 && ipv6) {
      if (type == kIpv6PktInfo) { if (data.size() < 20) return -kEinval; }
      else if (type == kIpv6HopLimit || type == kIpv6Tclass) { if (!number(data, &value) || value < -1 || value > 255) return -kEinval; }
      else if (type == 62 /* IPV6_DONTFRAG */) { if (!number(data, &value)) return -kEinval; }
      else return -kEinval;
    } else if (level == 17 /* SOL_UDP */ && !s.sockets->is_tcp(s.id)) {
      if (type != 103 /* UDP_SEGMENT */ || data.size() != 2) return -kEinval;
    }
    const uint64_t step = (size + 7) & ~uint64_t{7};
    if (step >= all.size() - at) break;
    at += step;
  }
  return 0;
}

}  // namespace

// struct msghdr: msg_name (8), msg_namelen (4, then 4 of padding), msg_iov (8), msg_iovlen (8), msg_control (8), msg_controllen (8), msg_flags (4).

uint64_t inet_sendmsg(Process& p, uint64_t fd, uint64_t message, uint64_t flags) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  uint8_t raw[56];
  if (!p.read_user(message, ByteView(raw, sizeof(raw)))) return error(kEfault);
  ByteReader in{ConstByteView(raw, sizeof(raw))};
  const uint64_t name = in.le64();
  const uint32_t name_length = in.le32();
  in.skip(4);
  const uint64_t vectors = in.le64(), count = in.le64(), control = in.le64(), control_length = in.le64();
  network::IpAddress to;
  uint16_t port = 0;
  bool has = false;
  if (const int64_t r = destination_of(p, s, name, name_length, &to, &port, &has); r < 0) return error(-r);
  allocators::IAllocator* allocator = call_memory(p);
  if (!allocator) return error(kEnomem);
  if (const int64_t r = check_control(p, s, control, control_length, *allocator); r < 0) return error(-r);
  ByteBuffer data(*allocator);
  if (const int64_t r = gather(p, vectors, count, s.sockets->is_tcp(s.id) ? uint64_t{1} << 24 : kLargestDatagram, &data); r < 0) return error(-r);
  const int64_t sent = send_bytes(p, s, data.view(), flags, has ? &to : nullptr, has ? &port : nullptr);
  return sent < 0 ? error(-sent) : uint64_t(sent);
}

uint64_t inet_recvmsg(Process& p, uint64_t fd, uint64_t message, uint64_t flags) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  uint8_t raw[56];
  if (!p.read_user(message, ByteView(raw, sizeof(raw)))) return error(kEfault);
  if (flags & kMsgErrQueue) return error(kEagain);
  ByteReader in{ConstByteView(raw, sizeof(raw))};
  const uint64_t name = in.le64();
  in.skip(8);
  const uint64_t vectors = in.le64(), count = in.le64(), control = in.le64(), control_capacity = in.le64();
  const int64_t room = room_of(p, vectors, count, uint64_t{1} << 20);
  if (room < 0) return error(-room);
  allocators::IAllocator* allocator = call_memory(p);
  if (!allocator) return error(kEnomem);
  ByteBuffer data(*allocator);
  network::IpAddress from;
  uint16_t from_port = 0;
  uint32_t whole = 0;
  const int64_t got = receive_bytes(p, s, uint64_t(room), flags, &data, &from, &from_port, &whole);
  if (got < 0) return error(-got);
  const bool tcp = s.sockets->is_tcp(s.id);
  if (!(tcp && (flags & kMsgTrunc)) && !scatter(p, vectors, count, data.view())) return error(kEfault);
  // msg_namelen (at +8) holds the name buffer's size going in and the address's size coming out; msg_controllen (+40) and msg_flags (+48) say what came
  // with the data.
  uint32_t out_flags = 0;
  ByteBuffer ancillary(*allocator);
  if (tcp) {
    if (name != 0 && put_u32(p, message + 8, 0) != 0) return error(kEfault);
  } else {
    if (const int64_t w = write_address(p, name, name != 0 ? message + 8 : 0, from, from_port); w < 0) return error(-w);
    if (whole > uint64_t(got)) out_flags |= uint32_t(kMsgTrunc);
    bool cut = false;
    if (!build_control(p, s, from, control != 0 ? control_capacity : 0, &ancillary, &cut)) return error(kEnomem);
    if (cut) out_flags |= uint32_t(kMsgCtrunc);
    if (ancillary.size() != 0 && !p.write_user(control, ancillary.view())) return error(kEfault);
  }
  if (!p.write_user_u64(message + 40, ancillary.size()) || put_u32(p, message + 48, out_flags) != 0) return error(kEfault);
  return !tcp && (flags & kMsgTrunc) ? whole : uint64_t(got);
}

// readv() and writev() of a socket: one receive into, one send of, all the buffers together (a datagram is not split).
uint64_t inet_vector_io(Process& p, uint64_t fd, uint64_t vectors, uint64_t count, bool write) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  allocators::IAllocator* allocator = call_memory(p);
  if (!allocator) return error(kEnomem);
  ByteBuffer data(*allocator);
  if (write) {
    if (const int64_t r = gather(p, vectors, count, s.sockets->is_tcp(s.id) ? uint64_t{1} << 24 : kLargestDatagram, &data); r < 0) return error(-r);
    const int64_t sent = send_bytes(p, s, data.view(), 0, nullptr, nullptr);
    return sent < 0 ? error(-sent) : uint64_t(sent);
  }
  const int64_t room = room_of(p, vectors, count, uint64_t{1} << 20);
  if (room < 0) return error(-room);
  const int64_t got = receive_bytes(p, s, uint64_t(room), 0, &data, nullptr, nullptr);
  if (got < 0) return error(-got);
  return scatter(p, vectors, count, data.view()) ? uint64_t(got) : error(kEfault);
}

uint64_t inet_shutdown(Process& p, uint64_t fd, uint64_t how) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  if (how > 2) return error(kEinval);
  SocketManager::Options* options = s.sockets->options(s.id);
  const SocketManager::State state = s.sockets->state(s.id);
  if (state != SocketManager::State::Connected && state != SocketManager::State::Failed) return error(kEnotconn);
  if (how != 1) options->shut_read = true;
  if (how != 0) {
    options->shut_write = true;
    if (s.sockets->is_tcp(s.id)) s.sockets->shutdown(s.id, 1);      // (the other end reads the end of the stream)
  }
  return 0;
}

uint64_t inet_getsockname(Process& p, uint64_t fd, uint64_t address, uint64_t length_at) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  network::IpAddress local;
  uint16_t port = 0;
  if (!s.sockets->local_name(s.id, &local, &port)) return error(kEbadf);
  // (an address of the socket's own family, also before anything fixed one)
  if (s.sockets->is_ipv6(s.id) && !local.v6) local = network::IpAddress(network::kIpv6Any);
  // (until it is connected, a socket that was bound to an address shows that address)
  if (const SocketManager::Options* options = s.sockets->options(s.id); options->has_bound_address && s.sockets->state(s.id) != SocketManager::State::Connected)
    local = options->bound_address;
  if (address == 0 || length_at == 0) return error(kEfault);
  const int64_t w = write_address(p, address, length_at, local, port);
  return w < 0 ? error(-w) : 0;
}

uint64_t inet_getpeername(Process& p, uint64_t fd, uint64_t address, uint64_t length_at) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  network::IpAddress peer;
  uint16_t port = 0;
  if (s.sockets->state(s.id) != SocketManager::State::Connected || !s.sockets->peer_name(s.id, &peer, &port)) return error(kEnotconn);
  if (address == 0 || length_at == 0) return error(kEfault);
  const int64_t w = write_address(p, address, length_at, peer, port);
  return w < 0 ? error(-w) : 0;
}

namespace {

constexpr uint64_t kSolSocket = 1, kIpprotoIp = 0, kIpprotoTcp = 6, kIpprotoUdp = 17, kIpprotoIpv6 = 41;
// SOL_SOCKET
constexpr uint64_t kSoReuseAddr = 2, kSoType = 3, kSoError = 4, kSoDontRoute = 5, kSoBroadcast = 6, kSoSndBuf = 7, kSoRcvBuf = 8, kSoKeepAlive = 9, kSoOobInline = 10,
                   kSoLinger = 13, kSoReusePort = 15, kSoRcvLowat = 18, kSoRcvTimeo = 20, kSoSndTimeo = 21, kSoAcceptConn = 30, kSoSndBufForce = 32, kSoRcvBufForce = 33,
                   kSoProtocol = 38, kSoDomain = 39, kSoPriority = 12, kSoTimestamp = 29, kSoBindToDevice = 25, kSoIncomingCpu = 49;
constexpr uint32_t kDefaultBuffer = 212992;      // what a Linux socket reports before anyone set it

// What a socket would report as its error (SO_ERROR): a connection that could not be made, or one that broke.
uint32_t pending_error(const Inet& s) {
  if (s.sockets->state(s.id) != SocketManager::State::Failed) return 0;
  return uint32_t(s.sockets->options(s.id)->connect_started && !s.sockets->peer_closed(s.id) ? kEconnrefused : kEconnreset);
}

// struct timeval (seconds, microseconds: 8 bytes each) as nanoseconds; false if it is not valid.
bool timeout_from(Process& p, uint64_t at, uint64_t length, uint64_t* ns) {
  if (length < 16) return false;
  uint64_t seconds = 0, micro = 0;
  if (!p.read_user_u64(at, &seconds) || !p.read_user_u64(at + 8, &micro)) return false;
  if (int64_t(seconds) < 0 || micro >= 1000000u) return false;
  *ns = seconds > (~uint64_t{0} >> 1) / 1000000000u ? ~uint64_t{0} >> 1 : seconds * 1000000000u + micro * 1000u;
  return true;
}

}  // namespace

uint64_t inet_setsockopt(Process& p, uint64_t fd, uint64_t level, uint64_t name, uint64_t value, uint64_t length) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  SocketManager::Options* options = s.sockets->options(s.id);
  if (int32_t(length) < 0) return error(kEinval);
  uint32_t number = 0;
  const bool has_number = length >= 4 && get_u32(p, value, &number);
  if (level == kSolSocket) {
    switch (name) {
      case kSoRcvTimeo: case kSoSndTimeo: {
        uint64_t ns = 0;
        if (length < 16) return error(kEinval);
        uint64_t probe = 0;
        if (!p.read_user_u64(value, &probe)) return error(kEfault);
        if (!timeout_from(p, value, length, &ns)) return error(kEinval);      // (EDOM on Linux for microseconds out of range)
        (name == kSoRcvTimeo ? options->receive_timeout_ns : options->send_timeout_ns) = ns;
        return 0;
      }
      case kSoLinger: {
        uint32_t on = 0, seconds = 0;
        if (length < 8) return error(kEinval);
        if (!get_u32(p, value, &on) || !get_u32(p, value + 4, &seconds)) return error(kEfault);
        options->linger_on = on != 0;
        options->linger_seconds = seconds;
        return 0;
      }
      case kSoBindToDevice: return 0;      // (a name: every interface is tried anyway)
      default: break;
    }
    if (length < 4) return error(kEinval);
    if (!has_number) return error(kEfault);
    switch (name) {
      case kSoReuseAddr: options->reuse_address = number != 0; return 0;
      case kSoReusePort: options->reuse_port = number != 0; return 0;
      case kSoKeepAlive: options->keep_alive = number != 0; return 0;
      case kSoBroadcast: options->broadcast = number != 0; return 0;
      case kSoSndBuf: case kSoSndBufForce: options->send_buffer = number < 1024 ? 2048 : (number > 0x3FFFFFFFu ? 0x7FFFFFFEu : number * 2); return 0;      // (Linux doubles what is asked for)
      case kSoRcvBuf: case kSoRcvBufForce: options->receive_buffer = number < 1024 ? 2048 : (number > 0x3FFFFFFFu ? 0x7FFFFFFEu : number * 2); return 0;
      case kSoTimestamp: options->timestamp = number != 0 ? 1 : 0; return 0;        // SO_TIMESTAMP: recvmsg says when each datagram came
      case 35: options->timestamp = number != 0 ? 2 : 0; return 0;                  // SO_TIMESTAMPNS: the same in nanoseconds
      case kSoDontRoute: case kSoOobInline: case kSoRcvLowat: case kSoPriority: case kSoIncomingCpu: return 0;      // (taken; nothing here depends on them)
      default: return error(kEnoprotoopt);
    }
  }
  if (length < 4) return error(kEinval);
  if (!has_number) return error(kEfault);
  if (level == kIpprotoTcp) {
    if (!s.sockets->is_tcp(s.id)) return error(kEnoprotoopt);
    switch (name) {
      case 1: options->no_delay = number != 0; return 0;      // TCP_NODELAY (the stack sends what it is given at once either way)
      // TCP_MAXSEG, TCP_CORK, TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT, TCP_SYNCNT, TCP_DEFER_ACCEPT, TCP_QUICKACK, TCP_USER_TIMEOUT, TCP_FASTOPEN,
      // TCP_FASTOPEN_CONNECT, TCP_NOTSENT_LOWAT: taken, and nothing changes
      case 2: case 3: case 4: case 5: case 6: case 7: case 9: case 12: case 18: case 23: case 30: case 25: return 0;
      default: return error(kEnoprotoopt);
    }
  }
  if (level == kIpprotoIp) {
    if (s.sockets->is_ipv6(s.id) && s.sockets->v6_only(s.id)) return error(kEnoprotoopt);
    switch (name) {
      // what recvmsg() says about each datagram (see build_control)
      case kIpPktInfo: options->packet_info = number != 0; return 0;
      case kIpRecvTtl: options->receive_ttl = number != 0; return 0;
      case kIpRecvTos: options->receive_tos = number != 0; return 0;
      // IP_TOS, IP_TTL, IP_RECVERR, IP_MTU_DISCOVER, IP_FREEBIND, IP_BIND_ADDRESS_NO_PORT, IP_MULTICAST_TTL, IP_MULTICAST_LOOP, IP_MULTICAST_IF: taken,
      // and nothing changes
      case 1: case 2: case 11: case 10: case 15: case 24: case 33: case 34: case 32: return 0;
      default: return error(kEnoprotoopt);
    }
  }
  if (level == kIpprotoIpv6) {
    if (!s.sockets->is_ipv6(s.id)) return error(kEnoprotoopt);
    switch (name) {
      case 26: {      // IPV6_V6ONLY: only before the socket is bound or used
        if (s.sockets->v6_only(s.id) == (number != 0)) return 0;
        return s.sockets->set_v6_only(s.id, number != 0) == 0 ? 0 : error(kEinval);
      }
      case kIpv6RecvPktInfo: options->packet_info6 = number != 0; return 0;
      case kIpv6RecvHopLimit: options->receive_hop_limit = number != 0; return 0;
      case kIpv6RecvTclass: options->receive_traffic_class = number != 0; return 0;
      // IPV6_UNICAST_HOPS, IPV6_MULTICAST_IF, IPV6_MULTICAST_HOPS, IPV6_MULTICAST_LOOP, IPV6_MTU_DISCOVER, IPV6_RECVERR, IPV6_TCLASS: taken, and nothing
      // changes
      case 16: case 17: case 18: case 19: case 23: case 25: case 67: return 0;
      default: return error(kEnoprotoopt);
    }
  }
  if (level == kIpprotoUdp) return error(kEnoprotoopt);
  return error(kEnoprotoopt);
}

uint64_t inet_getsockopt(Process& p, uint64_t fd, uint64_t level, uint64_t name, uint64_t value, uint64_t length_at) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  SocketManager::Options* options = s.sockets->options(s.id);
  uint32_t capacity = 0;
  if (!get_u32(p, length_at, &capacity)) return error(kEfault);
  if (int32_t(capacity) < 0) return error(kEinval);
  // Most options are an int; the answer is cut to the buffer's size as Linux cuts it.
  auto answer = [&](ConstByteView bytes) -> uint64_t {
    const uint32_t n = uint32_t(bytes.size()) < capacity ? uint32_t(bytes.size()) : capacity;
    if (n != 0 && !p.write_user(value, bytes.first(n))) return error(kEfault);
    return put_u32(p, length_at, n);
  };
  auto number = [&](uint32_t v) -> uint64_t {
    uint8_t raw[4];
    ByteWriter w{ByteView(raw, sizeof(raw))};
    w.le32(v);
    return answer(ConstByteView(raw, sizeof(raw)));
  };
  const bool tcp = s.sockets->is_tcp(s.id);
  if (level == kSolSocket) {
    switch (name) {
      case kSoType: return number(tcp ? uint32_t(kSockStream) : uint32_t(kSockDgram));
      case kSoDomain: return number(s.sockets->is_ipv6(s.id) ? uint32_t(kAfInet6) : uint32_t(kAfInet));
      case kSoProtocol: return number(tcp ? 6 : 17);
      case kSoError: return number(pending_error(s));
      case kSoAcceptConn: return number(s.sockets->state(s.id) == SocketManager::State::Listening);
      case kSoReuseAddr: return number(options->reuse_address);
      case kSoReusePort: return number(options->reuse_port);
      case kSoKeepAlive: return number(options->keep_alive);
      case kSoBroadcast: return number(options->broadcast);
      case kSoSndBuf: return number(options->send_buffer ? options->send_buffer : kDefaultBuffer);
      case kSoRcvBuf: return number(options->receive_buffer ? options->receive_buffer : kDefaultBuffer);
      case kSoTimestamp: return number(options->timestamp == 1);
      case 35: return number(options->timestamp == 2);
      case kSoDontRoute: case kSoOobInline: case kSoPriority: return number(0);
      case kSoRcvLowat: return number(1);
      case kSoLinger: {
        uint8_t raw[8];
        ByteWriter w{ByteView(raw, sizeof(raw))};
        w.le32(options->linger_on);
        w.le32(options->linger_seconds);
        return answer(ConstByteView(raw, sizeof(raw)));
      }
      case kSoRcvTimeo: case kSoSndTimeo: {
        const uint64_t ns = name == kSoRcvTimeo ? options->receive_timeout_ns : options->send_timeout_ns;
        uint8_t raw[16];
        ByteWriter w{ByteView(raw, sizeof(raw))};
        w.le64(ns / 1000000000u);
        w.le64(ns % 1000000000u / 1000u);
        return answer(ConstByteView(raw, sizeof(raw)));
      }
      default: return error(kEnoprotoopt);
    }
  }
  if (level == kIpprotoTcp && tcp) {
    switch (name) {
      case 1: return number(options->no_delay);
      case 2: return number(1460);      // TCP_MAXSEG
      default: return error(kEnoprotoopt);      // (TCP_INFO among them)
    }
  }
  if (level == kIpprotoIpv6 && s.sockets->is_ipv6(s.id) && name == 26) return number(s.sockets->v6_only(s.id));
  if (level == kIpprotoIp && name == kIpPktInfo) return number(options->packet_info);
  if (level == kIpprotoIp && name == kIpRecvTtl) return number(options->receive_ttl);
  if (level == kIpprotoIp && name == kIpRecvTos) return number(options->receive_tos);
  if (level == kIpprotoIpv6 && s.sockets->is_ipv6(s.id) && name == kIpv6RecvPktInfo) return number(options->packet_info6);
  if (level == kIpprotoIpv6 && s.sockets->is_ipv6(s.id) && name == kIpv6RecvHopLimit) return number(options->receive_hop_limit);
  if (level == kIpprotoIpv6 && s.sockets->is_ipv6(s.id) && name == kIpv6RecvTclass) return number(options->receive_traffic_class);
  if (level == kIpprotoIp && (name == 1 || name == 2)) return number(name == 2 ? 64 : 0);      // IP_TOS, IP_TTL
  if (level == kIpprotoIpv6 && s.sockets->is_ipv6(s.id) && name == 16) return number(64);      // IPV6_UNICAST_HOPS
  return error(kEnoprotoopt);
}

uint64_t inet_ioctl(Process& p, uint64_t fd, uint64_t request, uint64_t argument) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  switch (request) {
    case 0x541B:      // FIONREAD: the bytes of the stream that wait; for datagrams the size of the next one, which the stack does not say before it is taken:
                      // the most one can be (they are never put together from fragments), so that a buffer of that size loses nothing
      return put_u32(p, argument, s.sockets->is_tcp(s.id) ? s.sockets->readable(s.id) : s.sockets->readable(s.id) != 0 ? 1472 : 0);
    case 0x5411:      // TIOCOUTQ: nothing is ever left unsent that the program could wait for
      return put_u32(p, argument, 0);
    case 0x5421: {    // FIONBIO
      uint32_t on = 0;
      if (!get_u32(p, argument, &on)) return error(kEfault);
      s.sockets->set_non_blocking(s.id, on != 0);
      return 0;
    }
    default: return error(25);      // ENOTTY
  }
}

bool inet_non_blocking(Process& p, uint64_t fd) {
  Inet s;
  return inet_of(p, fd, &s) == 0 && s.sockets->is_non_blocking(s.id);
}

void set_inet_non_blocking(Process& p, uint64_t fd, bool on) {
  Inet s;
  if (inet_of(p, fd, &s) == 0) s.sockets->set_non_blocking(s.id, on);
}

// What poll() reports for a socket (POLLIN 1, POLLOUT 4, POLLERR 8, POLLHUP 0x10, POLLRDNORM 0x40, POLLWRNORM 0x100, POLLRDHUP 0x2000).
uint16_t inet_poll_events(uint32_t id, uint16_t events) {
  SocketManager* sockets = system_sockets();
  if (!sockets || !sockets->exists(id)) return 0x20;      // POLLNVAL
  uint16_t out = 0;
  const SocketManager::State state = sockets->state(id);
  const SocketManager::Options* options = sockets->options(id);
  if (sockets->ready_to_read(id) || options->shut_read) out |= events & (1 | 0x40);
  const bool tcp = sockets->is_tcp(id);
  if (!tcp || state == SocketManager::State::Connected || state == SocketManager::State::Failed) out |= events & (4 | 0x100);
  if (state == SocketManager::State::Failed) out |= 8 | 0x10;
  if (tcp && sockets->peer_closed(id)) {
    out |= events & 0x2000;
    if (options->shut_write) out |= 0x10;      // (both directions are over)
  }
  if (tcp && state == SocketManager::State::Unconnected) out |= (events & 4) | 0x10;      // (a stream socket nobody connected: writable and hung up, as on Linux)
  return out;
}

// The same helpers for the other kinds of socket (LinuxNetlink.cpp).
int64_t gather_vectors(Process& p, uint64_t vectors, uint64_t count, uint64_t limit, ByteBuffer* out) { return gather(p, vectors, count, limit, out); }
int64_t vectors_room(Process& p, uint64_t vectors, uint64_t count, uint64_t limit) { return room_of(p, vectors, count, limit); }
bool scatter_vectors(Process& p, uint64_t vectors, uint64_t count, ConstByteView data) { return scatter(p, vectors, count, data); }
bool add_control_message(ByteBuffer& out, uint64_t capacity, uint32_t level, uint32_t type, ConstByteView data, bool* cut) {
  return add_control(out, capacity, level, type, data, cut);
}

}  // namespace process::linuxabi
