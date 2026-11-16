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
constexpr uint64_t kMsgPeek = 2, kMsgTrunc = 0x20, kMsgDontWait = 0x40, kMsgWaitAll = 0x100;
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
int64_t receive_bytes(Process& p, const Inet& s, uint64_t room, uint64_t flags, ByteBuffer* out, network::IpAddress* from, uint16_t* from_port) {
  SocketManager::Options* options = s.sockets->options(s.id);
  if (!options) return -kEbadf;
  if (flags & kMsgPeek) return -kEopnotsupp;      // (the stack hands data out once; nothing can be looked at and left)
  out->clear();
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
  if (room > (tcp ? uint64_t{1} << 20 : kLargestDatagram)) room = tcp ? uint64_t{1} << 20 : kLargestDatagram;
  if (!out->put_zeros(tcp ? room : kLargestDatagram < 2048 ? kLargestDatagram : (room < 2048 ? 2048 : room))) return -kEnomem;
  uint64_t done = 0;
  for (;;) {
    const uint64_t want = tcp ? (room - done < kMostAtOnce ? room - done : kMostAtOnce) : out->size();
    uint32_t status = 0, got = 0;
    bool closed = false;
    const int64_t r = wait_for(p, s, dont_wait, options->receive_timeout_ns, &status,
                               [&](bool* would_block) { return s.sockets->receive(s.id, out->writable_view().subview(done, want), &got, from, from_port, would_block, &closed); });
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
  const int64_t got = receive_bytes(p, s, length, flags, &data, &from, &from_port);
  if (got < 0) return error(-got);
  if (got != 0 && !p.write_user(buffer, data.view())) return error(kEfault);
  if (!s.sockets->is_tcp(s.id))
    if (const int64_t w = write_address(p, address, length_at, from, from_port); w < 0) return error(-w);
  return uint64_t(got);
}

// struct msghdr: msg_name (8), msg_namelen (4, then 4 of padding), msg_iov (8), msg_iovlen (8), msg_control (8), msg_controllen (8), msg_flags (4).
// Control messages are not carried: what a program sends with them is sent without, and none come back.
uint64_t inet_sendmsg(Process& p, uint64_t fd, uint64_t message, uint64_t flags) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  uint8_t raw[56];
  if (!p.read_user(message, ByteView(raw, sizeof(raw)))) return error(kEfault);
  ByteReader in{ConstByteView(raw, sizeof(raw))};
  const uint64_t name = in.le64();
  const uint32_t name_length = in.le32();
  in.skip(4);
  const uint64_t vectors = in.le64(), count = in.le64();
  network::IpAddress to;
  uint16_t port = 0;
  bool has = false;
  if (const int64_t r = destination_of(p, s, name, name_length, &to, &port, &has); r < 0) return error(-r);
  allocators::IAllocator* allocator = call_memory(p);
  if (!allocator) return error(kEnomem);
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
  ByteReader in{ConstByteView(raw, sizeof(raw))};
  const uint64_t name = in.le64();
  in.skip(8);
  const uint64_t vectors = in.le64(), count = in.le64();
  const int64_t room = room_of(p, vectors, count, uint64_t{1} << 20);
  if (room < 0) return error(-room);
  allocators::IAllocator* allocator = call_memory(p);
  if (!allocator) return error(kEnomem);
  ByteBuffer data(*allocator);
  network::IpAddress from;
  uint16_t from_port = 0;
  const int64_t got = receive_bytes(p, s, uint64_t(room), flags, &data, &from, &from_port);
  if (got < 0) return error(-got);
  if (!scatter(p, vectors, count, data.view())) return error(kEfault);
  // msg_namelen (at +8) holds the name buffer's size going in and the address's size coming out; no control data, no flags.
  if (s.sockets->is_tcp(s.id)) {
    if (name != 0 && put_u32(p, message + 8, 0) != 0) return error(kEfault);
  } else if (const int64_t w = write_address(p, name, name != 0 ? message + 8 : 0, from, from_port); w < 0) {
    return error(-w);
  }
  if (!p.write_user_u64(message + 40, 0) || put_u32(p, message + 48, 0) != 0) return error(kEfault);
  return uint64_t(got);
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
      case kSoDontRoute: case kSoOobInline: case kSoRcvLowat: case kSoPriority: case kSoTimestamp: case kSoIncomingCpu: return 0;      // (taken; nothing here depends on them)
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
      // IP_TOS, IP_TTL, IP_RECVERR, IP_PKTINFO, IP_MTU_DISCOVER, IP_RECVTOS, IP_RECVTTL, IP_FREEBIND, IP_BIND_ADDRESS_NO_PORT, IP_MULTICAST_TTL,
      // IP_MULTICAST_LOOP, IP_MULTICAST_IF: taken, and nothing changes
      case 1: case 2: case 11: case 8: case 10: case 13: case 12: case 15: case 24: case 33: case 34: case 32: return 0;
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
      // IPV6_UNICAST_HOPS, IPV6_MULTICAST_IF, IPV6_MULTICAST_HOPS, IPV6_MULTICAST_LOOP, IPV6_MTU_DISCOVER, IPV6_RECVERR, IPV6_RECVPKTINFO, IPV6_TCLASS,
      // IPV6_RECVTCLASS, IPV6_RECVHOPLIMIT: taken, and nothing changes
      case 16: case 17: case 18: case 19: case 23: case 25: case 49: case 67: case 66: case 51: return 0;
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
      case kSoDontRoute: case kSoOobInline: case kSoPriority: case kSoTimestamp: return number(0);
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
  if (level == kIpprotoIp && (name == 1 || name == 2)) return number(name == 2 ? 64 : 0);      // IP_TOS, IP_TTL
  if (level == kIpprotoIpv6 && s.sockets->is_ipv6(s.id) && name == 16) return number(64);      // IPV6_UNICAST_HOPS
  return error(kEnoprotoopt);
}

uint64_t inet_ioctl(Process& p, uint64_t fd, uint64_t request, uint64_t argument) {
  Inet s;
  if (const int64_t r = inet_of(p, fd, &s); r < 0) return error(-r);
  switch (request) {
    case 0x541B:      // FIONREAD: the bytes of the stream that wait, or the size of the next datagram (here: how many datagrams wait, at least one byte each)
      return put_u32(p, argument, s.sockets->readable(s.id));
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

}  // namespace process::linuxabi
