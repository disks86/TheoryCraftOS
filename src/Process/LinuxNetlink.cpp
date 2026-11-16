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
#include "Process/LinuxNetlink.hpp"

#include "Process/LinuxIpc.hpp"
#include "Process/LinuxSockets.hpp"
#include "Process/Process.hpp"
#include "Process/Sockets.hpp"
#include "Process/UnixSockets.hpp"
#include "Utility/ByteStream.hpp"

// AF_NETLINK sockets for Linux programs. A socket is a "linked" descriptor of the AF_UNIX manager (so poll, dup, fork and close treat it like any
// other); what it is lives in the Linux IPC manager's table. Its other end is the kernel: a datagram sent to port 0 is a run of requests, answered at
// once into the socket's own queue (LinuxNetlinkMessages.cpp says what), and a socket that bound multicast groups is told what changed in the network.
// Nothing announces such changes here, so they are found by comparing the network with what the socket was last told, whenever the socket is looked at.
//
// Protocols: NETLINK_ROUTE (interfaces, addresses and routes, read only), NETLINK_GENERIC (the controller, which knows of no family but itself) and
// NETLINK_KOBJECT_UEVENT (device events: a socket that can be bound and never hears anything). Two sockets of one protocol can also send to each other.
namespace process::linuxabi {

namespace {

constexpr int64_t kEintr = 4, kEbadf = 9, kEagain = 11, kEnomem = 12, kEfault = 14, kEinval = 22, kEmfile = 24, kEmsgsize = 90, kEnoprotoopt = 92,
                  kEprotonosupport = 93, kEsocktnosupport = 94, kEopnotsupp = 95, kEaddrinuse = 98, kEnobufs = 105, kEconnrefused = 111;
uint64_t error(int64_t e) { return uint64_t(-e); }

constexpr uint64_t kAfNetlink = 16, kNameSize = 12;      // struct sockaddr_nl: nl_family (2), padding (2), nl_pid (4), nl_groups (4)
constexpr uint64_t kMsgPeek = 2, kMsgTrunc = 0x20, kMsgCtrunc = 8, kMsgDontWait = 0x40;
constexpr uint64_t kLargestMessage = 64 * 1024;
constexpr uint32_t kIdStep = 32768;
using Socket = LinuxIpcManager::NetlinkSocket;

struct Locked {
  ISystemSync& sync;
  explicit Locked(ISystemSync& s) : sync(s) { sync.lock(); }
  ~Locked() { sync.unlock(); }
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;
};

uint64_t link_of(const LinuxIpcManager& ipc, const Socket* socket) { return uint64_t(socket - &ipc.netlink_sockets[0]) + uint64_t{socket->sequence} * kIdStep; }

Socket* socket_of(LinuxIpcManager& ipc, uint64_t link) {
  const uint64_t index = link % kIdStep, sequence = link / kIdStep;
  if (index >= LinuxIpcManager::kNetlinkSockets || !ipc.netlink_sockets[index].used || ipc.netlink_sockets[index].sequence != sequence) return nullptr;
  return &ipc.netlink_sockets[index];
}

// The last descriptor of a socket was closed (called by the descriptor manager with the system's lock held).
void socket_gone(uint64_t link, uint32_t) {
  LinuxIpcManager* ipc = existing_linux_ipc();
  if (!ipc) return;
  Socket* socket = socket_of(*ipc, link);
  if (!socket) return;
  const uint16_t sequence = uint16_t((socket->sequence + 1) % 0x7FFF);
  *socket = Socket();
  socket->sequence = sequence;
}

Socket* with_port(LinuxIpcManager& ipc, uint32_t protocol, uint32_t port) {
  for (Socket& other : ipc.netlink_sockets)
    if (other.used && other.bound && other.protocol == protocol && other.port == port) return &other;
  return nullptr;
}

// Gives the socket an address if it has none: the process's number, or (a second socket of the process) one nobody has, counted down from where Linux
// starts looking.
void bind_itself(LinuxIpcManager& ipc, Socket& socket, uint64_t pid) {
  if (socket.bound) return;
  uint32_t port = uint32_t(pid);
  if (port == 0 || with_port(ipc, socket.protocol, port)) {
    port = 0xFFFFEFFFu;      // (-4097)
    while (with_port(ipc, socket.protocol, port)) --port;
  }
  socket.port = port;
  socket.bound = true;
}

// A datagram at the end of the socket's queue. One that does not fit is lost, and the socket's next read says so.
void queue(Socket& to, uint32_t group, uint32_t sender, ConstByteView data) {
  ByteBuffer& pending = *to.pending;
  const uint64_t before = pending.size();
  if (before > LinuxIpcManager::kNetlinkPendingBytes || data.size() + netlink::kQueuedHead > LinuxIpcManager::kNetlinkPendingBytes - before ||
      !(pending.put32(uint32_t(data.size())) && pending.put32(group) && pending.put32(sender) && pending.append(data))) {
    pending.truncate(before);
    to.overflowed = !to.no_enobufs;
  }
}

// Brings a socket that listens for changes up to now: what is different from what it was last told is queued as notices (the system's lock held).
void collect_locked(Socket& socket) {
  if (socket.groups == 0 || socket.protocol != netlink::kProtocolRoute || !socket.seen) return;
  netlink::Snapshot now;
  network_snapshot(&now);
  ByteBuffer& pending = *socket.pending;
  const uint64_t before = pending.size();
  if (!netlink::changes(*socket.seen, now, socket.groups, &pending) || pending.size() > LinuxIpcManager::kNetlinkPendingBytes) {
    pending.truncate(before);
    socket.overflowed = !socket.no_enobufs;
  }
  *socket.seen = now;
}

// Starts (or stops) listening for changes: from now on, so what the network is at this moment is not news.
bool set_groups(LinuxIpcManager& ipc, Socket& socket, uint32_t groups) {
  if (groups != 0 && socket.protocol == netlink::kProtocolRoute && !socket.seen) {
    socket.seen = UniquePointer<netlink::Snapshot>::make(ipc.allocator());
    if (!socket.seen) return false;
    network_snapshot(socket.seen.get());
  }
  socket.groups = groups;
  return true;
}

// A struct sockaddr_nl a program handed in. 0, or -errno; *family is what it says it is.
int64_t read_name(Process& p, uint64_t address, uint64_t length, uint16_t* family, uint32_t* port, uint32_t* groups) {
  if (length < 2) return -kEinval;
  uint8_t raw[kNameSize] = {};
  const uint64_t take = length < kNameSize ? length : kNameSize;
  if (!p.read_user(address, ByteView(raw).first(take))) return -kEfault;
  ByteReader in{ConstByteView(raw)};
  *family = in.le16();
  in.skip(2);
  *port = in.le32();
  *groups = in.le32();
  if (*family == kAfNetlink && length < kNameSize) return -kEinval;
  return 0;
}

// A struct sockaddr_nl for the program, cut to the room it has; the size it would take goes to `length_at` (a 32-bit count) if there is one.
int64_t write_name(Process& p, uint64_t address, uint64_t room, uint64_t length_at, uint32_t port, uint32_t groups) {
  uint8_t raw[kNameSize];
  ByteWriter w{ByteView(raw)};
  w.le16(uint16_t(kAfNetlink));
  w.le16(0);
  w.le32(port);
  w.le32(groups);
  const uint64_t n = room < kNameSize ? room : kNameSize;
  if (address != 0 && n != 0 && !p.write_user(address, ConstByteView(raw).first(n))) return -kEfault;
  if (length_at != 0 && !p.write_user_u32(length_at, uint32_t(kNameSize))) return -kEfault;
  return 0;
}

// Sends one datagram from the socket: to the kernel (answered at once), or to the socket of the same protocol that has the port. The count, or -errno.
int64_t send_datagram(Process& p, uint64_t link, ConstByteView data, bool has_to, uint32_t to_port) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return -kEbadf;
  if (data.size() > kLargestMessage) return -kEmsgsize;
  Locked locked(ipc->sync());
  Socket* socket = socket_of(*ipc, link);
  if (!socket) return -kEbadf;
  bind_itself(*ipc, *socket, p.id());
  const uint32_t port = has_to ? to_port : socket->peer_port;
  if (port != 0) {
    Socket* other = with_port(*ipc, socket->protocol, port);
    if (!other) return -kEconnrefused;
    queue(*other, 0, socket->port, data);
    ipc->sync().notify_all();
    return int64_t(data.size());
  }
  if (socket->protocol == netlink::kProtocolUevent) return -kEconnrefused;      // (the kernel only speaks on this one)
  netlink::Snapshot network;
  network_snapshot(&network);
  netlink::Asker asker;
  asker.protocol = socket->protocol;
  asker.port = socket->port;
  asker.cap_ack = socket->cap_ack;
  ByteBuffer& pending = *socket->pending;
  const uint64_t before = pending.size();
  if (!netlink::answer(asker, data, network, &pending) || pending.size() > LinuxIpcManager::kNetlinkPendingBytes) {
    pending.truncate(before);
    socket->overflowed = !socket->no_enobufs;
  }
  ipc->sync().notify_all();
  return int64_t(data.size());
}

struct Received {
  uint32_t whole = 0, group = 0, sender = 0;
  bool packet_info = false, pass_credentials = false;
};

// Takes (or with MSG_PEEK looks at) the first datagram of the socket's queue: up to `room` bytes of it into `out`. The count copied, or -errno.
int64_t receive_datagram(Process& p, uint64_t link, bool non_blocking, uint64_t room, uint64_t flags, ByteBuffer* out, Received* got) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return -kEbadf;
  int64_t result = 0;
  uint64_t timeout = ~uint64_t{0};
  {
    Locked locked(ipc->sync());
    if (const Socket* socket = socket_of(*ipc, link)) timeout = socket->receive_timeout_ns;
  }
  if (non_blocking || (flags & kMsgDontWait)) timeout = 0;
  const uint32_t outcome = system_wait(p, ipc->sync(), timeout, [&]() -> bool {
    Socket* socket = socket_of(*ipc, link);
    if (!socket) { result = -kEbadf; return true; }
    collect_locked(*socket);
    if (socket->overflowed) {      // (told once; what did arrive can be read after it)
      socket->overflowed = false;
      result = -kEnobufs;
      return true;
    }
    ByteBuffer& pending = *socket->pending;
    if (pending.size() < netlink::kQueuedHead) return false;
    ByteReader in{pending.view()};
    got->whole = in.le32();
    got->group = in.le32();
    got->sender = in.le32();
    got->packet_info = socket->packet_info;
    got->pass_credentials = socket->pass_credentials;
    if (got->whole > in.remaining()) {      // (cannot be: the queue is only written whole)
      pending.clear();
      return false;
    }
    const uint64_t copied = got->whole < room ? got->whole : room;
    out->clear();
    if (!out->append(in.take(copied))) { result = -kEnomem; return true; }
    if (!(flags & kMsgPeek)) {
      const uint64_t taken = netlink::kQueuedHead + got->whole;
      ByteView all = pending.writable_view();
      all.first(all.size() - taken).copy_from(ConstByteView(all).subview(taken));
      pending.truncate(all.size() - taken);
    }
    result = int64_t(copied);
    return true;
  });
  if (outcome == 1) return -kEagain;
  if (outcome == 2) return -kEintr;
  return result;
}

// The control messages that come with a datagram, as the socket asked: SCM_CREDENTIALS (SO_PASSCRED: who sent it; the kernel is process 0 of user 0)
// and NETLINK_PKTINFO (the group it was sent to).
bool build_control(const Received& got, uint64_t capacity, ByteBuffer* out, bool* cut) {
  uint8_t raw[12];
  if (got.pass_credentials) {
    ByteWriter w{ByteView(raw)};
    w.le32(got.sender);
    w.le32(0);
    w.le32(0);
    if (!add_control_message(*out, capacity, 1 /* SOL_SOCKET */, 2 /* SCM_CREDENTIALS */, ConstByteView(raw), cut)) return false;
  }
  if (got.packet_info) {
    ByteWriter w{ByteView(raw).first(4)};
    w.le32(got.group);
    if (!add_control_message(*out, capacity, 270 /* SOL_NETLINK */, 1 /* NETLINK_PKTINFO */, ConstByteView(raw).first(4), cut)) return false;
  }
  return true;
}

bool read_u32(Process& p, uint64_t at, uint32_t* out) {
  uint8_t raw[4];
  if (!p.read_user(at, ByteView(raw))) return false;
  ByteReader in{ConstByteView(raw)};
  *out = in.le32();
  return true;
}

allocators::IAllocator* call_memory() {
  LinuxIpcManager* ipc = system_linux_ipc();
  return ipc ? &ipc->allocator() : nullptr;
}

}  // namespace

void network_snapshot(netlink::Snapshot* out) {
  *out = netlink::Snapshot();
  netlink::Interface& lo = out->interfaces[out->count++];
  lo.index = 1;
  lo.name[0] = 'l';
  lo.name[1] = 'o';
  lo.name_size = 2;
  lo.loopback = true;
  lo.mtu = 65536;
  SocketManager* sockets = system_sockets();
  if (!sockets) return;
  network::IpConfiguration c;
  for (uint32_t i = 0; out->count < netlink::kMostInterfaces && sockets->interface_configuration(i, &c); ++i) {
    netlink::Interface& on = out->interfaces[out->count++];
    on.index = 2 + i;      // (the numbers IP_PKTINFO reports: SocketManager::arrival)
    on.name[0] = 'e';
    on.name[1] = 't';
    on.name[2] = 'h';
    on.name[3] = char('0' + i % 10);
    on.name_size = 4;
    hardware::MacAddress mac{};
    if (sockets->interface_mac(i, &mac)) ByteView(on.mac).copy_from(ConstByteView(mac.bytes));
    if (c.configured()) {
      on.has4 = true;
      ByteView(on.address4).copy_from(ConstByteView(c.address.octets));
      ByteView(on.gateway4).copy_from(ConstByteView(c.gateway.octets));
      uint32_t mask = c.netmask.value();
      while (mask & 0x80000000u) {      // (the leading ones of the mask)
        ++on.prefix4;
        mask <<= 1;
      }
    }
    network::Ipv6Configuration c6;
    if (!sockets->interface_configuration6(i, &c6)) continue;
    on.mtu = c6.mtu != 0 ? c6.mtu : 1500;
    if (c6.link_local_ready) {
      on.has_link6 = true;
      ByteView(on.link6).copy_from(ConstByteView(c6.link_local.bytes));
    }
    if (c6.global_ready) {
      on.has_global6 = true;
      ByteView(on.global6).copy_from(ConstByteView(c6.global.bytes));
      on.prefix6 = c6.prefix_length != 0 && c6.prefix_length <= 128 ? c6.prefix_length : 64;
    }
    if (!(c6.router == network::kIpv6Any)) {
      on.has_router6 = true;
      ByteView(on.router6).copy_from(ConstByteView(c6.router.bytes));
    }
  }
}

// The interface ioctls of a socket (SIOCGIF*), the older way to ask what netlink tells: struct ifreq is the interface's name (16 bytes) and 24 bytes
// for what is asked. if_nametoindex() and if_indextoname() are these.
bool interface_ioctl(Process& p, uint64_t request, uint64_t argument, uint64_t* result) {
  constexpr uint64_t kGetName = 0x8910, kGetConf = 0x8912, kGetFlags = 0x8913, kGetAddr = 0x8915, kGetDstAddr = 0x8917, kGetBrdAddr = 0x8919, kGetNetmask = 0x891B,
                     kGetMetric = 0x891D, kGetMtu = 0x8921, kGetHwAddr = 0x8927, kGetIndex = 0x8933, kGetTxQueueLen = 0x8942;
  constexpr uint64_t kRequestSize = 40;
  constexpr int64_t kEperm = 1, kEnodev = 19, kEaddrnotavail = 99;
  if (request < 0x8910 || request > 0x894F) return false;
  netlink::Snapshot network;
  network_snapshot(&network);
  if (request == kGetConf) {      // struct ifconf: ifc_len (4, and 4 of padding), ifc_req (8). Every interface with an IPv4 address, as many as fit.
    uint8_t head[16];
    if (!p.read_user(argument, ByteView(head))) { *result = error(kEfault); return true; }
    ByteReader in{ConstByteView(head)};
    const uint32_t room = in.le32();
    in.skip(4);
    const uint64_t at = in.le64();
    uint32_t used = 0;
    for (uint32_t i = 0; i < network.count; ++i) {
      const netlink::Interface& on = network.interfaces[i];
      if (!on.loopback && !on.has4) continue;
      if (at != 0 && (int32_t(room) < 0 || room - used < kRequestSize)) break;
      if (at != 0) {
        uint8_t raw[kRequestSize] = {};
        ByteWriter w{ByteView(raw)};
        w.bytes(ConstByteView(reinterpret_cast<const uint8_t*>(on.name), on.name_size));      // (a name's characters as bytes)
        w.zeros(16 - on.name_size);
        w.le16(2);      // AF_INET
        w.le16(0);
        static constexpr uint8_t kLoopback[4] = {127, 0, 0, 1};
        w.bytes(on.loopback ? ConstByteView(kLoopback) : ConstByteView(on.address4));
        if (!p.write_user(at + used, ConstByteView(raw))) { *result = error(kEfault); return true; }
      }
      used += uint32_t(kRequestSize);
    }
    *result = p.write_user_u32(argument, used) ? 0 : error(kEfault);
    return true;
  }
  const bool known = request == kGetName || request == kGetFlags || request == kGetAddr || request == kGetDstAddr || request == kGetBrdAddr || request == kGetNetmask ||
                     request == kGetMetric || request == kGetMtu || request == kGetHwAddr || request == kGetIndex || request == kGetTxQueueLen;
  if (!known) {      // (setting any of it, and what else there is in this range: not a Linux program's to do here)
    *result = error(kEperm);
    return true;
  }
  uint8_t raw[kRequestSize];
  if (!p.read_user(argument, ByteView(raw))) { *result = error(kEfault); return true; }
  const netlink::Interface* on = nullptr;
  ByteReader in{ConstByteView(raw)};
  const ConstByteView name = in.take(16);
  const uint32_t number = in.le32();
  for (uint32_t i = 0; i < network.count && !on; ++i) {
    const netlink::Interface& each = network.interfaces[i];
    if (request == kGetName) {
      if (each.index == number) on = &each;
    } else if (name.first(each.name_size).equals(ConstByteView(reinterpret_cast<const uint8_t*>(each.name), each.name_size)) && (each.name_size == 16 || name[each.name_size] == 0)) {
      on = &each;
    }
  }
  if (!on) { *result = error(kEnodev); return true; }
  ByteWriter w{ByteView(raw)};
  auto address = [&](ConstByteView bytes) {
    w.le16(2);
    w.le16(0);
    w.bytes(bytes);
    w.zeros(8);
  };
  static constexpr uint8_t kLoopback[4] = {127, 0, 0, 1}, kLoopbackMask[4] = {255, 0, 0, 0};
  uint8_t mask[4] = {}, broadcast[4] = {};
  for (uint32_t bit = 0; bit < 32; ++bit) {
    const uint8_t at = uint8_t(0x80u >> (bit % 8));
    if (bit < on->prefix4) mask[bit / 8] = uint8_t(mask[bit / 8] | at);
    broadcast[bit / 8] = uint8_t(bit < on->prefix4 ? (broadcast[bit / 8] | (on->address4[bit / 8] & at)) : (broadcast[bit / 8] | at));
  }
  const bool has4 = on->loopback || on->has4;
  if (request == kGetName) {
    w.bytes(ConstByteView(reinterpret_cast<const uint8_t*>(on->name), on->name_size));
    w.zeros(16 - on->name_size);
  } else {
    (void)w.reserve(16);      // (the name stays as it was handed in)
    switch (request) {
      case kGetIndex: w.le32(on->index); break;
      case kGetFlags: w.le16(on->loopback ? 0x49 : 0x1043); break;      // IFF_UP | IFF_LOOPBACK | IFF_RUNNING; IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST
      case kGetMtu: w.le32(on->mtu); break;
      case kGetMetric: w.le32(0); break;
      case kGetTxQueueLen: w.le32(1000); break;
      case kGetHwAddr:
        w.le16(on->loopback ? 772 : 1);      // ARPHRD_LOOPBACK, ARPHRD_ETHER
        w.bytes(ConstByteView(on->mac));
        w.zeros(8);
        break;
      case kGetAddr: case kGetDstAddr:
        if (!has4) { *result = error(kEaddrnotavail); return true; }
        address(on->loopback ? ConstByteView(kLoopback) : ConstByteView(on->address4));
        break;
      case kGetNetmask:
        if (!has4) { *result = error(kEaddrnotavail); return true; }
        address(on->loopback ? ConstByteView(kLoopbackMask) : ConstByteView(mask));
        break;
      case kGetBrdAddr: {
        if (!has4) { *result = error(kEaddrnotavail); return true; }
        static constexpr uint8_t kNone[4] = {};
        address(on->loopback ? ConstByteView(kNone) : ConstByteView(broadcast));
        break;
      }
      default: break;
    }
  }
  *result = p.write_user(argument, ConstByteView(raw)) ? 0 : error(kEfault);
  return true;
}

uint64_t netlink_socket(Process& p, uint64_t type, uint64_t protocol) {
  constexpr uint64_t kNonBlock = 0x800, kCloseOnExec = 0x80000;
  const uint64_t base = type & 0xF, extra = type & ~uint64_t{0xF};
  if (extra & ~(kNonBlock | kCloseOnExec)) return error(kEinval);
  if (base != 2 && base != 3) return error(kEsocktnosupport);      // SOCK_DGRAM and SOCK_RAW: the same thing here
  if (protocol != netlink::kProtocolRoute && protocol != netlink::kProtocolUevent && protocol != netlink::kProtocolGeneric) return error(kEprotonosupport);
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEnomem);
  UnixSocketManager::set_link_release_hook(UnixType::Netlink, socket_gone);
  uint64_t link = 0;
  {
    Locked locked(ipc->sync());
    Socket* socket = nullptr;
    for (Socket& candidate : ipc->netlink_sockets)
      if (!candidate.used) { socket = &candidate; break; }
    if (!socket) return error(kEmfile);
    UniquePointer<ByteBuffer> pending = UniquePointer<ByteBuffer>::make(ipc->allocator(), ipc->allocator());
    if (!pending) return error(kEnomem);
    const uint16_t sequence = socket->sequence;
    *socket = Socket();
    socket->sequence = sequence;
    socket->used = true;
    socket->protocol = uint32_t(protocol);
    socket->pending = static_cast<UniquePointer<ByteBuffer>&&>(pending);
    link = link_of(*ipc, socket);
  }
  const uint64_t fd = open_linked(p, UnixType::Netlink, link, 0, (extra & kCloseOnExec) != 0, (extra & kNonBlock) != 0);
  if (int64_t(fd) < 0 && int64_t(fd) != -kEmfile) {      // (EMFILE: the descriptor was made and let go again, which freed the socket)
    Locked locked(ipc->sync());
    socket_gone(link, 0);
  }
  return fd;
}

bool netlink_descriptor(Process& p, uint64_t fd, uint64_t* link, bool* non_blocking) {
  if (p.files().kind(fd) != DescriptorKind::UnixSocket) return false;
  uint32_t mode = 0;
  return linked_of(p, fd, UnixType::Netlink, link, &mode, non_blocking) == 0;
}

uint64_t netlink_bind(Process& p, uint64_t link, uint64_t address, uint64_t length) {
  uint16_t family = 0;
  uint32_t port = 0, groups = 0;
  if (const int64_t r = read_name(p, address, length, &family, &port, &groups); r < 0) return error(-r);
  if (family != kAfNetlink) return error(kEinval);
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEbadf);
  Locked locked(ipc->sync());
  Socket* socket = socket_of(*ipc, link);
  if (!socket) return error(kEbadf);
  if (socket->bound) {
    if (port != 0 && port != socket->port) return error(kEinval);      // (an address is for good)
  } else if (port == 0) {
    bind_itself(*ipc, *socket, p.id());
  } else {
    if (with_port(*ipc, socket->protocol, port)) return error(kEaddrinuse);
    socket->port = port;
    socket->bound = true;
  }
  return set_groups(*ipc, *socket, groups) ? 0 : error(kEnomem);
}

uint64_t netlink_connect(Process& p, uint64_t link, uint64_t address, uint64_t length) {
  uint16_t family = 0;
  uint32_t port = 0, groups = 0;
  if (const int64_t r = read_name(p, address, length, &family, &port, &groups); r < 0) return error(-r);
  if (family != kAfNetlink && family != 0) return error(kEinval);
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEbadf);
  Locked locked(ipc->sync());
  Socket* socket = socket_of(*ipc, link);
  if (!socket) return error(kEbadf);
  if (family == 0) {      // AF_UNSPEC: back to the kernel
    socket->peer_port = 0;
    socket->peer_groups = 0;
    return 0;
  }
  bind_itself(*ipc, *socket, p.id());
  socket->peer_port = port;
  socket->peer_groups = groups;
  return 0;
}

uint64_t netlink_sendto(Process& p, uint64_t link, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address, uint64_t address_length) {
  (void)flags;
  if (length > kLargestMessage) return error(kEmsgsize);
  allocators::IAllocator* allocator = call_memory();
  if (!allocator) return error(kEnomem);
  uint16_t family = 0;
  uint32_t port = 0, groups = 0;
  const bool has_to = address != 0;
  if (has_to) {
    if (const int64_t r = read_name(p, address, address_length, &family, &port, &groups); r < 0) return error(-r);
    if (family != kAfNetlink) return error(kEinval);
  }
  ByteBuffer data(*allocator);
  if (!data.put_zeros(length)) return error(kEnomem);
  if (length != 0 && !p.read_user(buffer, data.writable_view())) return error(kEfault);
  const int64_t sent = send_datagram(p, link, data.view(), has_to, port);
  return sent < 0 ? error(-sent) : uint64_t(sent);
}

// struct msghdr: msg_name (8), msg_namelen (4, then 4 of padding), msg_iov (8), msg_iovlen (8), msg_control (8), msg_controllen (8), msg_flags (4).

uint64_t netlink_sendmsg(Process& p, uint64_t link, uint64_t message, uint64_t flags) {
  (void)flags;
  uint8_t raw[56];
  if (!p.read_user(message, ByteView(raw))) return error(kEfault);
  ByteReader in{ConstByteView(raw)};
  const uint64_t name = in.le64();
  const uint32_t name_length = in.le32();
  in.skip(4);
  const uint64_t vectors = in.le64(), count = in.le64();
  uint16_t family = 0;
  uint32_t port = 0, groups = 0;
  const bool has_to = name != 0;
  if (has_to) {
    if (const int64_t r = read_name(p, name, name_length, &family, &port, &groups); r < 0) return error(-r);
    if (family != kAfNetlink) return error(kEinval);
  }
  allocators::IAllocator* allocator = call_memory();
  if (!allocator) return error(kEnomem);
  ByteBuffer data(*allocator);
  if (const int64_t r = gather_vectors(p, vectors, count, kLargestMessage, &data); r < 0) return error(-r);
  const int64_t sent = send_datagram(p, link, data.view(), has_to, port);      // (control messages: credentials a program may state; not looked at)
  return sent < 0 ? error(-sent) : uint64_t(sent);
}

uint64_t netlink_recvfrom(Process& p, uint64_t link, bool non_blocking, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address, uint64_t length_at) {
  allocators::IAllocator* allocator = call_memory();
  if (!allocator) return error(kEnomem);
  if (length > kLargestMessage * 4) length = kLargestMessage * 4;
  uint32_t room = 0;
  if (address != 0 && length_at != 0 && !read_u32(p, length_at, &room)) return error(kEfault);
  ByteBuffer data(*allocator);
  Received got;
  const int64_t copied = receive_datagram(p, link, non_blocking, length, flags, &data, &got);
  if (copied < 0) return error(-copied);
  if (copied != 0 && !p.write_user(buffer, data.view())) return error(kEfault);
  if (address != 0 && length_at != 0)
    if (const int64_t w = write_name(p, address, room, length_at, got.sender, got.group != 0 ? uint32_t{1} << ((got.group - 1) & 31) : 0); w < 0) return error(-w);
  return (flags & kMsgTrunc) ? got.whole : uint64_t(copied);      // (MSG_TRUNC asks for the datagram's real size)
}

uint64_t netlink_recvmsg(Process& p, uint64_t link, bool non_blocking, uint64_t message, uint64_t flags) {
  uint8_t raw[56];
  if (!p.read_user(message, ByteView(raw))) return error(kEfault);
  ByteReader in{ConstByteView(raw)};
  const uint64_t name = in.le64();
  const uint32_t name_room = in.le32();
  in.skip(4);
  const uint64_t vectors = in.le64(), count = in.le64(), control = in.le64(), control_capacity = in.le64();
  const int64_t room = vectors_room(p, vectors, count, kLargestMessage * 4);
  if (room < 0) return error(-room);
  allocators::IAllocator* allocator = call_memory();
  if (!allocator) return error(kEnomem);
  ByteBuffer data(*allocator), ancillary(*allocator);
  Received got;
  const int64_t copied = receive_datagram(p, link, non_blocking, uint64_t(room), flags, &data, &got);
  if (copied < 0) return error(-copied);
  if (!scatter_vectors(p, vectors, count, data.view())) return error(kEfault);
  if (name != 0)
    if (const int64_t w = write_name(p, name, name_room, message + 8, got.sender, got.group != 0 ? uint32_t{1} << ((got.group - 1) & 31) : 0); w < 0) return error(-w);
  uint32_t out_flags = got.whole > uint64_t(copied) ? uint32_t(kMsgTrunc) : 0;
  bool cut = false;
  if (!build_control(got, control != 0 ? control_capacity : 0, &ancillary, &cut)) return error(kEnomem);
  if (cut) out_flags |= uint32_t(kMsgCtrunc);
  if (ancillary.size() != 0 && !p.write_user(control, ancillary.view())) return error(kEfault);
  if (!p.write_user_u64(message + 40, ancillary.size()) || !p.write_user_u32(message + 48, out_flags)) return error(kEfault);
  return (flags & kMsgTrunc) ? got.whole : uint64_t(copied);
}

uint64_t netlink_vector_io(Process& p, uint64_t link, bool non_blocking, uint64_t vectors, uint64_t count, bool write) {
  allocators::IAllocator* allocator = call_memory();
  if (!allocator) return error(kEnomem);
  ByteBuffer data(*allocator);
  if (write) {
    if (const int64_t r = gather_vectors(p, vectors, count, kLargestMessage, &data); r < 0) return error(-r);
    const int64_t sent = send_datagram(p, link, data.view(), false, 0);
    return sent < 0 ? error(-sent) : uint64_t(sent);
  }
  const int64_t room = vectors_room(p, vectors, count, kLargestMessage * 4);
  if (room < 0) return error(-room);
  Received got;
  const int64_t copied = receive_datagram(p, link, non_blocking, uint64_t(room), 0, &data, &got);
  if (copied < 0) return error(-copied);
  return scatter_vectors(p, vectors, count, data.view()) ? uint64_t(copied) : error(kEfault);
}

uint64_t netlink_getsockname(Process& p, uint64_t link, uint64_t address, uint64_t length_at, bool peer) {
  uint32_t room = 0;
  if (!read_u32(p, length_at, &room)) return error(kEfault);
  if (int32_t(room) < 0) return error(kEinval);
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEbadf);
  uint32_t port = 0, groups = 0;
  {
    Locked locked(ipc->sync());
    const Socket* socket = socket_of(*ipc, link);
    if (!socket) return error(kEbadf);
    port = peer ? socket->peer_port : socket->port;
    groups = peer ? socket->peer_groups : socket->groups;
  }
  const int64_t w = write_name(p, address, room, length_at, port, groups);
  return w < 0 ? error(-w) : 0;
}

namespace {
constexpr uint64_t kSolSocket = 1, kSolNetlink = 270;
constexpr uint64_t kSoType = 3, kSoError = 4, kSoSndBuf = 7, kSoRcvBuf = 8, kSoPassCred = 16, kSoRcvTimeo = 20, kSoSndTimeo = 21, kSoAttachFilter = 26,
                   kSoDetachFilter = 27, kSoSndBufForce = 32, kSoRcvBufForce = 33, kSoProtocol = 38, kSoDomain = 39, kSoRcvTimeoNew = 66, kSoSndTimeoNew = 67;
constexpr uint64_t kAddMembership = 1, kDropMembership = 2, kPktInfo = 3, kBroadcastError = 4, kNoEnobufs = 5, kListenAllNsid = 8, kListMemberships = 9,
                   kCapAck = 10, kExtAck = 11, kGetStrictChk = 12;
}  // namespace

uint64_t netlink_setsockopt(Process& p, uint64_t link, uint64_t level, uint64_t name, uint64_t value, uint64_t length) {
  if (int32_t(length) < 0) return error(kEinval);
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEbadf);
  uint8_t raw[16] = {};
  const uint64_t take = length < sizeof(raw) ? length : sizeof(raw);
  const bool filter = level == kSolSocket && (name == kSoAttachFilter || name == kSoDetachFilter);
  if (!filter && take != 0 && !p.read_user(value, ByteView(raw).first(take))) return error(kEfault);
  ByteReader in{ConstByteView(raw)};
  const uint32_t number = in.le32();
  Locked locked(ipc->sync());
  Socket* socket = socket_of(*ipc, link);
  if (!socket) return error(kEbadf);
  if (level == kSolSocket) {
    switch (name) {
      case kSoSndBuf: case kSoSndBufForce: if (length < 4) return error(kEinval); socket->send_buffer = number > 0x3FFFFFFF ? 0x7FFFFFFE : (number < 2304 ? 4608 : number * 2); return 0;
      case kSoRcvBuf: case kSoRcvBufForce: if (length < 4) return error(kEinval); socket->receive_buffer = number > 0x3FFFFFFF ? 0x7FFFFFFE : (number < 1152 ? 2304 : number * 2); return 0;
      case kSoPassCred: if (length < 4) return error(kEinval); socket->pass_credentials = number != 0; return 0;
      case kSoRcvTimeo: case kSoSndTimeo: case kSoRcvTimeoNew: case kSoSndTimeoNew: {      // struct timeval: seconds and microseconds (zero: for ever)
        if (length < 16) return error(kEinval);
        ByteReader time{ConstByteView(raw)};
        const uint64_t seconds = time.le64(), micro = time.le64();
        if (micro >= 1000000) return error(33 /* EDOM */);
        const uint64_t ns = int64_t(seconds) < 0 ? 0 : (seconds > 9000000000ull ? ~uint64_t{0} : seconds * 1000000000ull + micro * 1000);
        (name == kSoRcvTimeo || name == kSoRcvTimeoNew ? socket->receive_timeout_ns : socket->send_timeout_ns) = ns == 0 ? ~uint64_t{0} : ns;
        return 0;
      }
      // A filter (a classic BPF program run on each datagram, which udev's library attaches to hear only of the devices it wants): taken and not run.
      case kSoAttachFilter: socket->filtered = true; return 0;
      case kSoDetachFilter: if (!socket->filtered) return error(2 /* ENOENT */); socket->filtered = false; return 0;
      default: return error(kEnoprotoopt);
    }
  }
  if (level != kSolNetlink) return error(kEnoprotoopt);
  if (length < 4) return error(kEinval);
  switch (name) {
    case kAddMembership: case kDropMembership: {      // a group by its number (1..32 are the bits of nl_groups; a higher one there is nothing behind)
      if (number == 0) return error(kEinval);
      if (number > 32) return name == kAddMembership ? error(kEinval) : 0;
      const uint32_t bit = uint32_t{1} << (number - 1);
      bind_itself(*ipc, *socket, p.id());
      return set_groups(*ipc, *socket, name == kAddMembership ? (socket->groups | bit) : (socket->groups & ~bit)) ? 0 : error(kEnomem);
    }
    case kPktInfo: socket->packet_info = number != 0; return 0;
    case kBroadcastError: socket->broadcast_error = number != 0; return 0;
    case kNoEnobufs: socket->no_enobufs = number != 0; if (number != 0) socket->overflowed = false; return 0;
    case kListenAllNsid: socket->all_namespaces = number != 0; return 0;
    case kCapAck: socket->cap_ack = number != 0; return 0;
    case kExtAck: socket->ext_ack = number != 0; return 0;
    case kGetStrictChk: socket->strict_check = number != 0; return 0;
    default: return error(kEnoprotoopt);
  }
}

uint64_t netlink_getsockopt(Process& p, uint64_t link, uint64_t level, uint64_t name, uint64_t value, uint64_t length_at) {
  uint32_t room = 0;
  if (!read_u32(p, length_at, &room)) return error(kEfault);
  if (int32_t(room) < 0) return error(kEinval);
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEbadf);
  uint8_t raw[16] = {};
  ByteWriter w{ByteView(raw)};
  {
    Locked locked(ipc->sync());
    const Socket* socket = socket_of(*ipc, link);
    if (!socket) return error(kEbadf);
    auto time = [&](uint64_t ns) {
      w.le64(ns == ~uint64_t{0} ? 0 : ns / 1000000000ull);
      w.le64(ns == ~uint64_t{0} ? 0 : ns % 1000000000ull / 1000);
    };
    if (level == kSolSocket) {
      switch (name) {
        case kSoType: w.le32(3); break;      // SOCK_RAW
        case kSoError: w.le32(0); break;
        case kSoSndBuf: w.le32(socket->send_buffer); break;
        case kSoRcvBuf: w.le32(socket->receive_buffer); break;
        case kSoPassCred: w.le32(socket->pass_credentials ? 1 : 0); break;
        case kSoProtocol: w.le32(socket->protocol); break;
        case kSoDomain: w.le32(uint32_t(kAfNetlink)); break;
        case kSoRcvTimeo: case kSoRcvTimeoNew: time(socket->receive_timeout_ns); break;
        case kSoSndTimeo: case kSoSndTimeoNew: time(socket->send_timeout_ns); break;
        default: return error(kEnoprotoopt);
      }
    } else if (level == kSolNetlink) {
      switch (name) {
        case kPktInfo: w.le32(socket->packet_info ? 1 : 0); break;
        case kBroadcastError: w.le32(socket->broadcast_error ? 1 : 0); break;
        case kNoEnobufs: w.le32(socket->no_enobufs ? 1 : 0); break;
        case kListenAllNsid: w.le32(socket->all_namespaces ? 1 : 0); break;
        case kListMemberships: w.le32(socket->groups); break;      // (the groups as a bit set, a 32-bit word at a time: one word here)
        case kCapAck: w.le32(socket->cap_ack ? 1 : 0); break;
        case kExtAck: w.le32(socket->ext_ack ? 1 : 0); break;
        case kGetStrictChk: w.le32(socket->strict_check ? 1 : 0); break;
        default: return error(kEnoprotoopt);
      }
      if (room < 4) return error(kEinval);
    } else {
      return error(kEnoprotoopt);
    }
  }
  const uint64_t n = room < w.position() ? room : w.position();
  if (n != 0 && !p.write_user(value, ConstByteView(raw).first(n))) return error(kEfault);
  return p.write_user_u32(length_at, uint32_t(n)) ? 0 : error(kEfault);
}

uint64_t netlink_pending(Process& p, uint64_t link, uint64_t argument) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEbadf);
  uint32_t size = 0;
  {
    Locked locked(ipc->sync());
    Socket* socket = socket_of(*ipc, link);
    if (!socket) return error(kEbadf);
    collect_locked(*socket);
    if (socket->pending->size() >= netlink::kQueuedHead) {
      ByteReader in{socket->pending->view()};
      size = in.le32();
    }
  }
  return p.write_user_u32(argument, size) ? 0 : error(kEfault);
}

bool netlink_ready_locked(uint64_t link, bool* watching) {
  *watching = false;
  LinuxIpcManager* ipc = existing_linux_ipc();
  if (!ipc) return false;
  Socket* socket = socket_of(*ipc, link);
  if (!socket) return false;
  collect_locked(*socket);
  *watching = socket->groups != 0 && socket->protocol == netlink::kProtocolRoute;
  return socket->pending->size() != 0 || socket->overflowed;
}

}  // namespace process::linuxabi
