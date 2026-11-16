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
#include "Process/LinuxPacket.hpp"

#include "Process/LinuxBpf.hpp"
#include "Process/LinuxIpc.hpp"
#include "Process/LinuxNetlink.hpp"
#include "Process/LinuxSockets.hpp"
#include "Process/Process.hpp"
#include "Process/Sockets.hpp"
#include "Process/UnixSockets.hpp"
#include "Utility/ByteStream.hpp"

// AF_PACKET sockets for Linux programs. A socket is a "linked" descriptor of the AF_UNIX manager; what it is lives in the Linux IPC manager's table. It
// holds a frame tap (network::IFrameTap) on every network interface and tells each which frames it wants: of one ether type or of all, from the wire or
// from the loopback, as bind() and the options say. SOCK_RAW hands over and takes whole Ethernet frames; SOCK_DGRAM leaves the link header to the kernel
// (the sender's address comes with the frame, and the header of a frame to send is written from the address it is sent to).
//
// The interfaces are numbered as everywhere else here (LinuxNetlink.cpp): 1 is the loopback, 2 the first network interface. Packets a program sends to
// the machine itself never are frames; they are shown as frames with a link header of zero addresses, once each, as received.
namespace process::linuxabi {

namespace {

constexpr int64_t kEperm = 1, kEnoent = 2, kEintr = 4, kEnxio = 6, kEbadf = 9, kEagain = 11, kEnomem = 12, kEfault = 14, kEnodev = 19, kEinval = 22, kEmfile = 24,
                  kEbusy = 16, kEnospc = 28, kEalready = 114, kEmsgsize = 90, kEnoprotoopt = 92, kEsocktnosupport = 94, kEopnotsupp = 95, kEnobufs = 105;
uint64_t error(int64_t e) { return uint64_t(-e); }

constexpr uint64_t kAfPacket = 17;
constexpr uint64_t kNameSize = 20;      // struct sockaddr_ll: sll_family (2), sll_protocol (2, network order), sll_ifindex (4), sll_hatype (2), sll_pkttype (1), sll_halen (1), sll_addr (8)
constexpr uint64_t kMsgPeek = 2, kMsgTrunc = 0x20, kMsgCtrunc = 8, kMsgDontWait = 0x40;
constexpr uint32_t kIdStep = 32768, kHead = 14, kMostData = 1500;
constexpr uint16_t kEveryProtocol = 3;      // ETH_P_ALL
constexpr uint64_t kWaitSliceUs = 2000;
using Socket = LinuxIpcManager::PacketSocket;
using Frame = LinuxIpcManager::PacketFrame;

struct Locked {
  ISystemSync& sync;
  explicit Locked(ISystemSync& s) : sync(s) { sync.lock(); }
  ~Locked() { sync.unlock(); }
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;
};

uint64_t link_of(const LinuxIpcManager& ipc, const Socket* socket) { return uint64_t(socket - &ipc.packet_sockets[0]) + uint64_t{socket->sequence} * kIdStep; }

Socket* socket_of(LinuxIpcManager& ipc, uint64_t link) {
  const uint64_t index = link % kIdStep, sequence = link / kIdStep;
  if (index >= LinuxIpcManager::kPacketSockets || !ipc.packet_sockets[index].used || ipc.packet_sockets[index].sequence != sequence) return nullptr;
  return &ipc.packet_sockets[index];
}

uint16_t ether_type_of(const Frame& f);

// PACKET_FANOUT. A group's members each hold taps of their own, so each is shown every frame; what makes a frame go to one of them only is that they
// all work out the same member for it, from the frame (its flow, or which of the group's frames it is: rank_of) and the group as it is when the frame
// is looked at. A frame that waits while the group changes may so go to nobody or to two: joining and leaving are rare.
constexpr uint8_t kFanoutHash = 0, kFanoutLoadBalance = 1, kFanoutCpu = 2, kFanoutRollover = 3, kFanoutRandom = 4, kFanoutQueue = 5, kFanoutCbpf = 6;
constexpr uint16_t kFanoutFlagRollover = 0x1000, kFanoutFlagUniqueId = 0x2000, kFanoutFlagIgnoreOutgoing = 0x4000, kFanoutFlagDefragment = 0x8000;

uint32_t mix(uint32_t h) {
  h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
  return h;
}

// A number that is the same for both directions of a flow: of an IPv4 or IPv6 frame its two addresses and (TCP, UDP) its two ports; of anything else its
// ether type and the two hardware addresses.
uint32_t flow_of(ConstByteView frame) {
  auto sum = [](ConstByteView bytes) {
    uint32_t v = 0, i = 0;
    for (const uint8_t b : bytes) v += uint32_t{b} << (8 * (i++ & 3));
    return v;
  };
  if (frame.size() < kHead) return 0;
  const uint16_t type = uint16_t(frame[12] << 8 | frame[13]);
  const ConstByteView packet = frame.dropped(kHead);
  uint32_t ends = 0, protocol = 0;
  ConstByteView transport;
  if (type == 0x0800 && packet.size() >= 20 && (packet[0] >> 4) == 4) {
    const uint32_t header = (packet[0] & 0x0Fu) * 4u;
    ends = sum(packet.subview(12, 4)) + sum(packet.subview(16, 4));
    protocol = packet[9];
    if (header >= 20 && header <= packet.size() && (uint16_t(packet[6] << 8 | packet[7]) & 0x1FFF) == 0) transport = packet.dropped(header);      // (not a later fragment)
  } else if (type == 0x86DD && packet.size() >= 40 && (packet[0] >> 4) == 6) {
    ends = sum(packet.subview(8, 16)) + sum(packet.subview(24, 16));
    protocol = packet[6];
    transport = packet.dropped(40);
  } else {
    return mix(type + sum(frame.first(6)) + sum(frame.subview(6, 6)));
  }
  uint32_t ports = 0;
  if ((protocol == 6 || protocol == 17) && transport.size() >= 4) ports = uint32_t(transport[0] << 8 | transport[1]) + uint32_t(transport[2] << 8 | transport[3]);
  return mix(ends + mix(ports + protocol));
}

// Is the frame the socket's taps just handed it one for this socket? (Always, outside a group; in one, only if the group's choice for it is this socket.)
// A frame's number among the frames of its group, the same whichever member asks (see PacketFanout::ranked).
uint32_t rank_of(LinuxIpcManager::PacketFanout& group, const Frame& f) {
  using Group = LinuxIpcManager::PacketFanout;
  const uint32_t known = group.ranked_count < Group::kRemembered ? group.ranked_count : Group::kRemembered;
  for (uint32_t i = 0; i < known; ++i) {      // (the newest first: that is where a member that keeps up finds its frame)
    const Group::Ranked& r = group.ranked[(group.ranked_count - 1 - i) % Group::kRemembered];
    if (r.interface == f.interface && r.sequence == f.info.sequence) return r.rank;
  }
  Group::Ranked& r = group.ranked[group.ranked_count % Group::kRemembered];
  r.interface = f.interface;
  r.sequence = f.info.sequence;
  r.rank = group.next_rank++;
  ++group.ranked_count;
  return r.rank;
}

bool chosen(LinuxIpcManager& ipc, const Socket& socket, uint64_t link, const Frame& f) {
  if (socket.fanout < 0 || uint32_t(socket.fanout) >= LinuxIpcManager::kPacketFanouts) return true;
  LinuxIpcManager::PacketFanout& group = ipc.packet_fanouts[socket.fanout];
  if (!group.used || group.count == 0) return true;
  const ConstByteView frame = ConstByteView(f.data).first(f.info.length);
  uint32_t pick = 0;
  switch (group.type) {
    case kFanoutHash: pick = flow_of(frame); break;
    case kFanoutLoadBalance: pick = rank_of(group, f); break;      // (each in turn)
    case kFanoutRandom: pick = mix(f.info.sequence + 0x9E3779B9u); break;
    case kFanoutCbpf: {      // the program's answer
      if (!group.chooser || group.chooser->size() == 0) break;
      bpf::Packet packet;
      packet.data = socket.cooked ? frame.dropped(kHead) : frame;
      if (socket.cooked) packet.link = frame.first(kHead);
      packet.network_offset = socket.cooked ? 0 : kHead;
      packet.wire_length = uint32_t(packet.data.size());
      packet.protocol = ether_type_of(f);
      packet.packet_type = uint8_t(f.info.kind);
      packet.interface_index = f.interface;
      pick = bpf::run(group.chooser->view(), packet);
      break;
    }
    // PACKET_FANOUT_CPU and PACKET_FANOUT_QM: frames are not told apart by processor or by queue here, so they all count as the first's;
    // PACKET_FANOUT_ROLLOVER: the first member, which is never found too busy
    default: break;
  }
  return group.members[pick % group.count] == link;
}

// Takes a socket out of its group; the last one out ends the group.
void leave_fanout(LinuxIpcManager& ipc, Socket& socket, uint64_t link) {
  if (socket.fanout < 0 || uint32_t(socket.fanout) >= LinuxIpcManager::kPacketFanouts) return;
  LinuxIpcManager::PacketFanout& group = ipc.packet_fanouts[socket.fanout];
  socket.fanout = -1;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < group.count; ++i)
    if (group.members[i] != link) group.members[kept++] = group.members[i];
  group.count = kept;
  if (kept == 0) group = LinuxIpcManager::PacketFanout();
}

// The last descriptor of a socket was closed (called by the descriptor manager with the system's lock held).
void socket_gone(uint64_t link, uint32_t) {
  LinuxIpcManager* ipc = existing_linux_ipc();
  if (!ipc) return;
  Socket* socket = socket_of(*ipc, link);
  if (!socket) return;
  leave_fanout(*ipc, *socket, link);
  for (SharedPointer<network::IFrameTap>& tap : socket->taps)
    if (tap) tap->close();
  const uint16_t sequence = uint16_t((socket->sequence + 1) % 0x7FFF);
  *socket = Socket();
  socket->sequence = sequence;
}

// Tells the socket's taps what it wants now.
void listen(Socket& socket) {
  for (uint32_t i = 0; i < LinuxIpcManager::kPacketTaps; ++i) {
    if (!socket.taps[i]) continue;
    const bool wire = socket.interface == 0 || socket.interface == 2 + i, loopback = socket.interface == 0 || socket.interface == 1;
    socket.taps[i]->listen(socket.protocol, socket.protocol == kEveryProtocol, wire, loopback, !socket.ignore_outgoing);
  }
}

uint16_t ether_type_of(const Frame& f) { return uint16_t(f.data[12] << 8 | f.data[13]); }

// The frame as the socket's program is to see it: whole, or without its link header.
ConstByteView shown(const Socket& socket, const Frame& f) {
  const ConstByteView whole = ConstByteView(f.data).first(f.info.length);
  return socket.cooked ? whole.dropped(kHead) : whole;
}

// Makes the next frame the socket is to get its held one: taken from a tap (each in turn), and through the filter. False if there is none now (the
// system's lock held).
bool fetch_locked(LinuxIpcManager& ipc, Socket& socket) {
  const uint64_t link = link_of(ipc, &socket);
  if (socket.has_held) return true;
  if (!socket.held) {
    socket.held = UniquePointer<Frame>::make(ipc.allocator());
    if (!socket.held) return false;
  }
  Frame& f = *socket.held;
  for (;;) {
    bool any = false;
    for (uint32_t n = 0; n < LinuxIpcManager::kPacketTaps && !any; ++n) {
      const uint32_t i = (socket.next_tap + n) % LinuxIpcManager::kPacketTaps;
      if (!socket.taps[i] || !socket.taps[i]->receive(ByteView(f.data), &f.info)) continue;
      any = true;
      socket.next_tap = (i + 1) % LinuxIpcManager::kPacketTaps;
      socket.drops += socket.taps[i]->take_dropped();
      f.interface = f.info.loopback ? 1 : 2 + i;
    }
    if (!any) return false;
    if (f.info.length > sizeof(f.data)) f.info.length = sizeof(f.data);
    if (f.info.length < kHead) continue;
    if (!chosen(ipc, socket, link, f)) continue;      // (another socket of its fan-out group gets this one)
    const ConstByteView data = shown(socket, f);
    f.kept = uint32_t(data.size());
    if (socket.filter && socket.filter->size() != 0) {
      bpf::Packet packet;
      packet.data = data;
      if (socket.cooked) packet.link = ConstByteView(f.data).first(kHead);
      packet.network_offset = socket.cooked ? 0 : kHead;
      packet.wire_length = uint32_t(data.size());
      packet.protocol = ether_type_of(f);
      packet.packet_type = uint8_t(f.info.kind);
      packet.interface_index = f.interface;
      const uint32_t wanted = bpf::run(socket.filter->view(), packet);
      if (wanted == 0) continue;      // (not for this socket)
      if (wanted < f.kept) f.kept = wanted;
    }
    ++socket.packets;
    socket.has_held = true;
    return true;
  }
}

void name_of(const Frame& f, ByteView out);

ByteView receive_ring(Socket& socket) {
  return socket.receive_block ? socket.receive_block.get()->bytes().first(socket.receive_request.bytes()) : ByteView();
}
ByteView transmit_ring(Socket& socket) {
  return socket.transmit_block ? socket.transmit_block.get()->bytes().first(socket.transmit_request.bytes()) : ByteView();
}

// A socket with a receive ring: every frame that waits goes into the ring now, and a block that has waited long enough is handed over (the system's
// lock held). Nothing drives this but the socket being looked at: poll(), a receive, an option.
void pump_locked(LinuxIpcManager& ipc, Socket& socket, uint64_t realtime_ns, uint64_t monotonic_ns) {
  if (!socket.receive_block) return;
  const ByteView ring = receive_ring(socket);
  while (fetch_locked(ipc, socket)) {
    const Frame& f = *socket.held;
    uint8_t name[kNameSize];
    name_of(f, ByteView(name));
    ring::Frame frame;
    if (!socket.cooked) frame.link = ConstByteView(f.data).first(kHead);
    frame.data = ConstByteView(f.data).subview(kHead, f.info.length - kHead);
    frame.kept = f.kept;
    frame.name = ConstByteView(name);
    frame.time_ns = realtime_ns;
    if (!socket.receiver.put(ring, frame, monotonic_ns)) {      // (no room: counted as lost, not as given)
      --socket.packets;
      ++socket.drops;
    }
    socket.has_held = false;
  }
  socket.receiver.tick(ring, monotonic_ns);
}

// Waits for the socket's next frame and copies it to `out`; with MSG_PEEK it stays the next one. 0, or -errno.
int64_t receive_frame(Process& p, uint64_t link, bool non_blocking, uint64_t flags, Frame* out, Socket* settings) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return -kEbadf;
  IProcessRuntime* runtime = p.runtime();
  SocketManager* sockets = system_sockets();
  const uint64_t started = p.host().monotonic_ns();
  for (;;) {
    const uint64_t seen = sockets ? sockets->progress() : 0;      // (read before the attempt: what happens after it ends the wait below)
    uint64_t timeout = ~uint64_t{0};
    {
      Locked locked(ipc->sync());
      Socket* socket = socket_of(*ipc, link);
      if (!socket) return -kEbadf;
      // (with a receive ring frames go there: nothing ever comes this way)
      pump_locked(*ipc, *socket, p.host().realtime_ns(), p.host().monotonic_ns());
      if (!socket->receive_block && fetch_locked(*ipc, *socket)) {
        *out = *socket->held;
        settings->cooked = socket->cooked;
        settings->aux_data = socket->aux_data;
        if (!(flags & kMsgPeek)) socket->has_held = false;
        return 0;
      }
      if (!socket->held && !socket->receive_block) return -kEnomem;
      timeout = socket->receive_timeout_ns;
    }
    if (non_blocking || (flags & kMsgDontWait)) return -kEagain;
    if (timeout != ~uint64_t{0} && p.host().monotonic_ns() - started >= timeout) return -kEagain;
    if (p.exited() || (runtime && runtime->interrupted()) || p.signal_interrupts()) return -kEintr;
    if (runtime) runtime->unlock();
    if (!sockets || !sockets->wait_for_progress(seen, kWaitSliceUs)) p.host().sleep_ns(kWaitSliceUs * 1000);
    if (runtime) runtime->lock();
  }
}

// struct sockaddr_ll for a frame: who sent it, on which interface, and how it came by.
void name_of(const Frame& f, ByteView out) {      // (declared above)
  ByteWriter w{out};
  w.le16(uint16_t(kAfPacket));
  w.be16(ether_type_of(f));
  w.le32(f.interface);
  w.le16(f.info.loopback ? 772 : 1);      // ARPHRD_LOOPBACK, ARPHRD_ETHER
  w.u8(uint8_t(f.info.kind));
  w.u8(6);
  w.bytes(ConstByteView(f.data).subview(6, 6));
  w.zeros(2);
}

bool read_u32(Process& p, uint64_t at, uint32_t* out) {
  uint8_t raw[4];
  if (!p.read_user(at, ByteView(raw))) return false;
  ByteReader in{ConstByteView(raw)};
  *out = in.le32();
  return true;
}

bool known_interface(uint32_t index) {
  netlink::Snapshot network;
  network_snapshot(&network);
  for (uint32_t i = 0; i < network.count; ++i)
    if (network.interfaces[i].index == index) return true;
  return false;
}

// Sends what a program handed in: a whole frame, or (a socket without link headers) what goes after the header that is written here from the address.
// `has_to`: the call named a struct sockaddr_ll, whose interface, protocol and address are `to_*`. The count, or -errno.
int64_t send_frame(uint64_t link, ConstByteView data, bool has_to, uint32_t to_interface, uint16_t to_protocol, ConstByteView to_address) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return -kEbadf;
  SharedPointer<network::IFrameTap> tap;
  uint8_t frame[network::FrameTap::kMaxFrame];
  ByteWriter w{ByteView(frame)};
  uint32_t interface = 0;
  {
    Locked locked(ipc->sync());
    Socket* socket = socket_of(*ipc, link);
    if (!socket) return -kEbadf;
    interface = has_to && to_interface != 0 ? to_interface : socket->interface;
    if (interface == 0) return -kEnxio;      // (no interface was named, by the call or by bind)
    if (interface != 1 && (interface < 2 || interface - 2 >= LinuxIpcManager::kPacketTaps || !socket->taps[interface - 2])) return -kEnxio;
    tap = interface == 1 ? socket->taps[0] : socket->taps[interface - 2];
    if (!tap) return -kEnxio;
    if (socket->cooked) {
      // (the loopback has no addresses, so none is needed there; a frame for the wire has to be told where to go)
      if (interface != 1 && (!has_to || to_address.size() < 6)) return -kEinval;
      if (data.size() > kMostData) return -kEmsgsize;
      const uint16_t protocol = to_protocol != 0 ? to_protocol : socket->protocol;
      if (interface == 1) {      // (the loopback has no addresses)
        w.zeros(12);
      } else {
        w.bytes(to_address.first(6));
        w.bytes(ConstByteView(tap->mac_address().bytes));
      }
      w.be16(protocol);
    } else {
      if (data.size() < kHead) return -kEinval;
      if (data.size() > kHead + kMostData) return -kEmsgsize;
    }
  }
  w.bytes(data);
  if (!w.ok()) return -kEmsgsize;
  const ConstByteView whole = w.written();
  // (the loopback has no wire: an IP packet sent on it comes back to the machine, and anything else goes nowhere, as it would there)
  if (interface == 1) {
    (void)tap->send_looped(whole);
    return int64_t(data.size());
  }
  return tap->send(whole) ? int64_t(data.size()) : -kEnobufs;
}

// A struct sockaddr_ll a program handed in with a send. 0, or -errno.
int64_t read_name(Process& p, uint64_t address, uint64_t length, uint32_t* interface, uint16_t* protocol, ByteView hardware, uint32_t* hardware_size) {
  if (length < 12) return -kEinval;      // (up to sll_addr at least)
  uint8_t raw[kNameSize] = {};
  if (!p.read_user(address, ByteView(raw).first(length < kNameSize ? length : kNameSize))) return -kEfault;
  ByteReader in{ConstByteView(raw)};
  if (in.le16() != kAfPacket) return -kEinval;
  *protocol = in.be16();
  *interface = in.le32();
  in.skip(3);
  const uint8_t size = in.u8();
  *hardware_size = size > 8 ? 8 : size;
  if (length < 12 + uint64_t{*hardware_size}) return -kEinval;
  hardware.first(8).copy_from(ConstByteView(raw).subview(12, 8));
  return 0;
}

// send() of a socket with a transmit ring: every frame the program left to be sent goes out, in order, until one that is not to be sent. The bytes sent
// in all, or -errno if the first could not go.
int64_t send_ring(uint64_t link, bool has_to, uint32_t to_interface, uint16_t to_protocol, ConstByteView to_address) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return -kEbadf;
  int64_t total = 0;
  for (uint32_t frames = 0; frames < 65536; ++frames) {
    uint8_t copy[kHead + kMostData];
    uint32_t size = 0;
    {
      Locked locked(ipc->sync());
      Socket* socket = socket_of(*ipc, link);
      if (!socket) return -kEbadf;
      ConstByteView frame;
      ring::Transmitter::Wrong wrong = ring::Transmitter::Wrong::No;
      if (!socket->transmitter.next(transmit_ring(*socket), socket->transmit_offsets, socket->cooked ? kMostData : kHead + kMostData, socket->skip_wrong, &frame, &wrong)) break;
      if (wrong != ring::Transmitter::Wrong::No) {
        if (socket->skip_wrong) continue;
        return wrong == ring::Transmitter::Wrong::TooLong ? -kEmsgsize : -kEinval;      // (whatever went before it: the call fails, as on Linux)
      }
      size = uint32_t(frame.size());
      ByteView(copy).first(size).copy_from(frame);      // (once: the program may write there again)
    }
    const int64_t sent = send_frame(link, ConstByteView(copy).first(size), has_to, to_interface, to_protocol, to_address);
    if (sent < 0) return total != 0 ? total : sent;
    total += sent;
  }
  return total;
}

bool has_transmit_ring(uint64_t link) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return false;
  Locked locked(ipc->sync());
  const Socket* socket = socket_of(*ipc, link);
  return socket && socket->transmit_block;
}

allocators::IAllocator* call_memory() {
  LinuxIpcManager* ipc = system_linux_ipc();
  return ipc ? &ipc->allocator() : nullptr;
}

}  // namespace

uint64_t packet_socket(Process& p, uint64_t type, uint64_t protocol) {
  constexpr uint64_t kNonBlock = 0x800, kCloseOnExec = 0x80000;
  const uint64_t base = type & 0xF, extra = type & ~uint64_t{0xF};
  if (extra & ~(kNonBlock | kCloseOnExec)) return error(kEinval);
  if (base != 2 && base != 3) return error(kEsocktnosupport);      // SOCK_DGRAM, SOCK_RAW
  if (protocol > 0xFFFF) return error(kEinval);
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEnomem);
  UnixSocketManager::set_link_release_hook(UnixType::Packet, socket_gone);
  uint64_t link = 0;
  {
    Locked locked(ipc->sync());
    Socket* socket = nullptr;
    for (Socket& candidate : ipc->packet_sockets)
      if (!candidate.used) { socket = &candidate; break; }
    if (!socket) return error(kEmfile);
    const uint16_t sequence = socket->sequence;
    *socket = Socket();
    socket->sequence = sequence;
    socket->used = true;
    socket->cooked = base == 2;
    socket->protocol = uint16_t((protocol & 0xFF) << 8 | protocol >> 8);      // (handed in in network order: htons(ETH_P_ALL))
    if (SocketManager* sockets = system_sockets())
      for (uint32_t i = 0; i < LinuxIpcManager::kPacketTaps; ++i) socket->taps[i] = sockets->open_frame_tap(i);
    listen(*socket);
    link = link_of(*ipc, socket);
  }
  const uint64_t fd = open_linked(p, UnixType::Packet, link, 0, (extra & kCloseOnExec) != 0, (extra & kNonBlock) != 0);
  if (int64_t(fd) < 0 && int64_t(fd) != -kEmfile) {      // (EMFILE: the descriptor was made and let go again, which freed the socket)
    Locked locked(ipc->sync());
    socket_gone(link, 0);
  }
  return fd;
}

bool packet_descriptor(Process& p, uint64_t fd, uint64_t* link, bool* non_blocking) {
  if (p.files().kind(fd) != DescriptorKind::UnixSocket) return false;
  uint32_t mode = 0;
  return linked_of(p, fd, UnixType::Packet, link, &mode, non_blocking) == 0;
}

// bind(): the interface to listen on (0: every one) and, if it names one, the protocol.
uint64_t packet_bind(Process& p, uint64_t link, uint64_t address, uint64_t length) {
  if (length < kNameSize) return error(kEinval);
  uint32_t interface = 0, hardware_size = 0;
  uint16_t protocol = 0;
  uint8_t hardware[8];
  if (const int64_t r = read_name(p, address, length, &interface, &protocol, ByteView(hardware), &hardware_size); r < 0) return error(-r);
  if (interface != 0 && !known_interface(interface)) return error(kEnodev);
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEbadf);
  Locked locked(ipc->sync());
  Socket* socket = socket_of(*ipc, link);
  if (!socket) return error(kEbadf);
  if (socket->fanout >= 0) return error(kEinval);      // (a group's members listen to the same thing: no more binding)
  socket->interface = interface;
  if (protocol != 0) socket->protocol = protocol;
  listen(*socket);
  return 0;
}

uint64_t packet_sendto(Process& p, uint64_t link, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address, uint64_t address_length) {
  (void)flags;
  if (length > kHead + kMostData) return error(kEmsgsize);
  uint32_t interface = 0, hardware_size = 0;
  uint16_t protocol = 0;
  uint8_t hardware[8] = {};
  if (address != 0)
    if (const int64_t r = read_name(p, address, address_length, &interface, &protocol, ByteView(hardware), &hardware_size); r < 0) return error(-r);
  if (has_transmit_ring(link)) {      // (what the call hands in is not looked at: the frames are in the ring)
    const int64_t sent = send_ring(link, address != 0, interface, protocol, ConstByteView(hardware).first(hardware_size));
    return sent < 0 ? error(-sent) : uint64_t(sent);
  }
  uint8_t data[kHead + kMostData];
  if (length != 0 && !p.read_user(buffer, ByteView(data).first(length))) return error(kEfault);
  const int64_t sent = send_frame(link, ConstByteView(data).first(length), address != 0, interface, protocol, ConstByteView(hardware).first(hardware_size));
  return sent < 0 ? error(-sent) : uint64_t(sent);
}

// struct msghdr: msg_name (8), msg_namelen (4, then 4 of padding), msg_iov (8), msg_iovlen (8), msg_control (8), msg_controllen (8), msg_flags (4).

uint64_t packet_sendmsg(Process& p, uint64_t link, uint64_t message, uint64_t flags) {
  (void)flags;
  uint8_t raw[56];
  if (!p.read_user(message, ByteView(raw))) return error(kEfault);
  ByteReader in{ConstByteView(raw)};
  const uint64_t name = in.le64();
  const uint32_t name_length = in.le32();
  in.skip(4);
  const uint64_t vectors = in.le64(), count = in.le64();
  uint32_t interface = 0, hardware_size = 0;
  uint16_t protocol = 0;
  uint8_t hardware[8] = {};
  if (name != 0)
    if (const int64_t r = read_name(p, name, name_length, &interface, &protocol, ByteView(hardware), &hardware_size); r < 0) return error(-r);
  if (has_transmit_ring(link)) {
    const int64_t sent = send_ring(link, name != 0, interface, protocol, ConstByteView(hardware).first(hardware_size));
    return sent < 0 ? error(-sent) : uint64_t(sent);
  }
  allocators::IAllocator* allocator = call_memory();
  if (!allocator) return error(kEnomem);
  ByteBuffer data(*allocator);
  if (const int64_t r = gather_vectors(p, vectors, count, kHead + kMostData, &data); r < 0) return error(-r);
  const int64_t sent = send_frame(link, data.view(), name != 0, interface, protocol, ConstByteView(hardware).first(hardware_size));
  return sent < 0 ? error(-sent) : uint64_t(sent);
}

uint64_t packet_recvfrom(Process& p, uint64_t link, bool non_blocking, uint64_t buffer, uint64_t length, uint64_t flags, uint64_t address, uint64_t length_at) {
  allocators::IAllocator* allocator = call_memory();
  if (!allocator) return error(kEnomem);
  uint32_t room = 0;
  if (address != 0 && length_at != 0 && !read_u32(p, length_at, &room)) return error(kEfault);
  UniquePointer<Frame> frame = UniquePointer<Frame>::make(*allocator);
  if (!frame) return error(kEnomem);
  Socket settings;
  if (const int64_t r = receive_frame(p, link, non_blocking, flags, frame.get(), &settings); r < 0) return error(-r);
  const ConstByteView data = shown(settings, *frame);
  const uint64_t kept = frame->kept < data.size() ? frame->kept : data.size(), copied = kept < length ? kept : length;
  if (copied != 0 && !p.write_user(buffer, data.first(copied))) return error(kEfault);
  if (address != 0 && length_at != 0) {
    uint8_t name[kNameSize];
    name_of(*frame, ByteView(name));
    const uint64_t n = room < kNameSize ? room : kNameSize;
    if (n != 0 && !p.write_user(address, ConstByteView(name).first(n))) return error(kEfault);
    if (!p.write_user_u32(length_at, uint32_t(kNameSize))) return error(kEfault);
  }
  return (flags & kMsgTrunc) ? data.size() : copied;      // (MSG_TRUNC asks for the frame's real size)
}

uint64_t packet_recvmsg(Process& p, uint64_t link, bool non_blocking, uint64_t message, uint64_t flags) {
  uint8_t raw[56];
  if (!p.read_user(message, ByteView(raw))) return error(kEfault);
  ByteReader in{ConstByteView(raw)};
  const uint64_t name = in.le64();
  const uint32_t name_room = in.le32();
  in.skip(4);
  const uint64_t vectors = in.le64(), count = in.le64(), control = in.le64(), control_capacity = in.le64();
  const int64_t room = vectors_room(p, vectors, count, 65536);
  if (room < 0) return error(-room);
  allocators::IAllocator* allocator = call_memory();
  if (!allocator) return error(kEnomem);
  UniquePointer<Frame> frame = UniquePointer<Frame>::make(*allocator);
  if (!frame) return error(kEnomem);
  Socket settings;
  if (const int64_t r = receive_frame(p, link, non_blocking, flags, frame.get(), &settings); r < 0) return error(-r);
  const ConstByteView data = shown(settings, *frame);
  const uint64_t kept = frame->kept < data.size() ? frame->kept : data.size(), copied = kept < uint64_t(room) ? kept : uint64_t(room);
  if (!scatter_vectors(p, vectors, count, data.first(copied))) return error(kEfault);
  if (name != 0) {
    uint8_t bytes[kNameSize];
    name_of(*frame, ByteView(bytes));
    const uint64_t n = name_room < kNameSize ? name_room : kNameSize;
    if (n != 0 && !p.write_user(name, ConstByteView(bytes).first(n))) return error(kEfault);
    if (!p.write_user_u32(message + 8, uint32_t(kNameSize))) return error(kEfault);
  }
  uint32_t out_flags = copied < data.size() ? uint32_t(kMsgTrunc) : 0;
  ByteBuffer ancillary(*allocator);
  if (settings.aux_data) {
    // PACKET_AUXDATA: struct tpacket_auxdata, tp_status (4: TP_STATUS_USER), tp_len, tp_snaplen (4 each: the frame's size and how much of it is
    // handed over), tp_mac, tp_net (2 each: where its link and network headers are), tp_vlan_tci, tp_vlan_tpid (2 each: none)
    uint8_t aux[20];
    ByteWriter w{ByteView(aux)};
    w.le32(1);
    w.le32(uint32_t(data.size()));
    w.le32(uint32_t(kept));
    w.le16(0);
    w.le16(settings.cooked ? 0 : uint16_t(kHead));
    w.le16(0);
    w.le16(0);
    bool cut = false;
    if (!add_control_message(ancillary, control != 0 ? control_capacity : 0, 263 /* SOL_PACKET */, 8 /* PACKET_AUXDATA */, ConstByteView(aux), &cut)) return error(kEnomem);
    if (cut) out_flags |= uint32_t(kMsgCtrunc);
    if (ancillary.size() != 0 && !p.write_user(control, ancillary.view())) return error(kEfault);
  }
  if (!p.write_user_u64(message + 40, ancillary.size()) || !p.write_user_u32(message + 48, out_flags)) return error(kEfault);
  return (flags & kMsgTrunc) ? data.size() : copied;
}

uint64_t packet_vector_io(Process& p, uint64_t link, bool non_blocking, uint64_t vectors, uint64_t count, bool write) {
  allocators::IAllocator* allocator = call_memory();
  if (!allocator) return error(kEnomem);
  if (write && has_transmit_ring(link)) {
    const int64_t sent = send_ring(link, false, 0, 0, ConstByteView());
    return sent < 0 ? error(-sent) : uint64_t(sent);
  }
  if (write) {
    ByteBuffer data(*allocator);
    if (const int64_t r = gather_vectors(p, vectors, count, kHead + kMostData, &data); r < 0) return error(-r);
    const int64_t sent = send_frame(link, data.view(), false, 0, 0, ConstByteView());
    return sent < 0 ? error(-sent) : uint64_t(sent);
  }
  const int64_t room = vectors_room(p, vectors, count, 65536);
  if (room < 0) return error(-room);
  UniquePointer<Frame> frame = UniquePointer<Frame>::make(*allocator);
  if (!frame) return error(kEnomem);
  Socket settings;
  if (const int64_t r = receive_frame(p, link, non_blocking, 0, frame.get(), &settings); r < 0) return error(-r);
  const ConstByteView data = shown(settings, *frame);
  const uint64_t kept = frame->kept < data.size() ? frame->kept : data.size(), copied = kept < uint64_t(room) ? kept : uint64_t(room);
  return scatter_vectors(p, vectors, count, data.first(copied)) ? copied : error(kEfault);
}

// getsockname(): the interface and protocol the socket listens to, and that interface's hardware address.
uint64_t packet_getsockname(Process& p, uint64_t link, uint64_t address, uint64_t length_at) {
  uint32_t room = 0;
  if (!read_u32(p, length_at, &room)) return error(kEfault);
  if (int32_t(room) < 0) return error(kEinval);
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEbadf);
  uint32_t interface = 0;
  uint16_t protocol = 0;
  {
    Locked locked(ipc->sync());
    const Socket* socket = socket_of(*ipc, link);
    if (!socket) return error(kEbadf);
    interface = socket->interface;
    protocol = socket->protocol;
  }
  netlink::Snapshot network;
  network_snapshot(&network);
  const netlink::Interface* on = nullptr;
  for (uint32_t i = 0; i < network.count; ++i)
    if (network.interfaces[i].index == interface) on = &network.interfaces[i];
  uint8_t raw[kNameSize] = {};
  ByteWriter w{ByteView(raw)};
  w.le16(uint16_t(kAfPacket));
  w.be16(protocol);
  w.le32(interface);
  w.le16(!on ? 0xFFFE /* ARPHRD_VOID: no interface */ : on->loopback ? 772 : 1);
  w.u8(0);
  w.u8(on ? 6 : 0);
  if (on) w.bytes(ConstByteView(on->mac));
  const uint64_t size = on ? 18 : 12, n = room < size ? room : size;      // (up to the end of the address, as Linux counts it)
  if (n != 0 && !p.write_user(address, ConstByteView(raw).first(n))) return error(kEfault);
  return p.write_user_u32(length_at, uint32_t(size)) ? 0 : error(kEfault);
}

namespace {
constexpr uint64_t kSolSocket = 1, kSolPacket = 263;
constexpr uint64_t kSoType = 3, kSoError = 4, kSoBroadcast = 6, kSoSndBuf = 7, kSoRcvBuf = 8, kSoPriority = 12, kSoRcvTimeo = 20, kSoBindToDevice = 25, kSoAttachFilter = 26,
                   kSoDetachFilter = 27, kSoSndBufForce = 32, kSoRcvBufForce = 33, kSoMark = 36, kSoProtocol = 38, kSoDomain = 39, kSoLockFilter = 44, kSoRcvTimeoNew = 66;
constexpr uint64_t kAddMembership = 1, kDropMembership = 2, kReceiveRing = 5, kStatistics = 6, kCopyThreshold = 7, kAuxData = 8, kVersion = 10, kHeaderLength = 11, kReserve = 12,
                   kTransmitRing = 13, kLoss = 14, kTimestamp = 17, kFanout = 18, kTransmitOffsets = 19, kQdiscBypass = 20, kFanoutData = 22, kIgnoreOutgoing = 23;
constexpr uint64_t kLargestRing = 64ull << 20;
}  // namespace

uint64_t packet_setsockopt(Process& p, uint64_t link, uint64_t level, uint64_t name, uint64_t value, uint64_t length) {
  if (int32_t(length) < 0) return error(kEinval);
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEbadf);
  uint8_t raw[16] = {};
  const uint64_t take = length < sizeof(raw) ? length : sizeof(raw);
  if (take != 0 && !p.read_user(value, ByteView(raw).first(take))) return error(kEfault);
  ByteReader in{ConstByteView(raw)};
  const uint32_t number = in.le32();
  if (level == kSolSocket && name == kSoAttachFilter) {
    // struct sock_fprog: len (2: instructions, and 6 of padding), filter (8: where they are). The program is checked before it is taken.
    if (length < 16) return error(kEinval);
    ByteReader program{ConstByteView(raw)};
    const uint16_t count = program.le16();
    program.skip(6);
    const uint64_t at = program.le64();
    if (count == 0 || count > bpf::kMostInstructions) return error(kEinval);
    UniquePointer<ByteBuffer> filter = UniquePointer<ByteBuffer>::make(ipc->allocator(), ipc->allocator());
    if (!filter || !filter->put_zeros(uint64_t{count} * bpf::kInstructionSize)) return error(kEnomem);
    if (!p.read_user(at, filter->writable_view())) return error(kEfault);
    if (!bpf::valid(filter->view())) return error(kEinval);
    Locked locked(ipc->sync());
    Socket* socket = socket_of(*ipc, link);
    if (!socket) return error(kEbadf);
    if (socket->locked_filter) return error(kEperm);
    socket->filter = static_cast<UniquePointer<ByteBuffer>&&>(filter);
    return 0;
  }
  if (level == kSolSocket && name == kSoBindToDevice) {      // an interface by its name (an empty one: every interface again)
    char text[17] = {};
    for (uint64_t i = 0; i < take && i < 16; ++i) text[i] = char(raw[i]);
    uint32_t size = 0;
    while (size < 16 && text[size] != 0) ++size;
    netlink::Snapshot network;
    network_snapshot(&network);
    uint32_t interface = 0;
    for (uint32_t i = 0; i < network.count && size != 0; ++i)
      if (StringView(network.interfaces[i].name, network.interfaces[i].name_size) == StringView(text, size)) interface = network.interfaces[i].index;
    if (size != 0 && interface == 0) return error(kEnodev);
    Locked locked(ipc->sync());
    Socket* socket = socket_of(*ipc, link);
    if (!socket) return error(kEbadf);
    socket->interface = interface;
    listen(*socket);
    return 0;
  }
  Locked locked(ipc->sync());
  Socket* socket = socket_of(*ipc, link);
  if (!socket) return error(kEbadf);
  if (level == kSolSocket) {
    switch (name) {
      case kSoSndBuf: case kSoSndBufForce: if (length < 4) return error(kEinval); socket->send_buffer = number > 0x3FFFFFFF ? 0x7FFFFFFE : (number < 2304 ? 4608 : number * 2); return 0;
      case kSoRcvBuf: case kSoRcvBufForce: if (length < 4) return error(kEinval); socket->receive_buffer = number > 0x3FFFFFFF ? 0x7FFFFFFE : (number < 1152 ? 2304 : number * 2); return 0;
      case kSoRcvTimeo: case kSoRcvTimeoNew: {      // struct timeval: seconds and microseconds (zero: for ever)
        if (length < 16) return error(kEinval);
        ByteReader time{ConstByteView(raw)};
        const uint64_t seconds = time.le64(), micro = time.le64();
        if (micro >= 1000000) return error(33 /* EDOM */);
        const uint64_t ns = int64_t(seconds) < 0 ? 0 : (seconds > 9000000000ull ? ~uint64_t{0} : seconds * 1000000000ull + micro * 1000);
        socket->receive_timeout_ns = ns == 0 ? ~uint64_t{0} : ns;
        return 0;
      }
      case kSoDetachFilter:
        if (length < 4) return error(kEinval);
        if (socket->locked_filter) return error(kEperm);
        if (!socket->filter) return error(kEnoent);
        socket->filter = UniquePointer<ByteBuffer>();
        return 0;
      case kSoLockFilter: if (length < 4) return error(kEinval); if (socket->locked_filter && number == 0) return error(kEperm); socket->locked_filter = number != 0; return 0;
      case kSoBroadcast: case kSoPriority: case kSoMark: return length < 4 ? error(kEinval) : 0;      // (taken; nothing here depends on them)
      default: return error(kEnoprotoopt);
    }
  }
  if (level != kSolPacket) return error(kEnoprotoopt);
  switch (name) {
    // PACKET_ADD_MEMBERSHIP, PACKET_DROP_MEMBERSHIP (struct packet_mreq: promiscuous mode, multicast addresses): the interface hands up what it
    // hands up either way
    case kAddMembership: case kDropMembership: return length < 16 ? error(kEinval) : 0;
    case kAuxData: if (length < 4) return error(kEinval); socket->aux_data = number != 0; return 0;
    case kIgnoreOutgoing: if (length < 4) return error(kEinval); socket->ignore_outgoing = number != 0; listen(*socket); return 0;
    case kLoss: if (length < 4) return error(kEinval); socket->skip_wrong = number != 0; return 0;
    case kQdiscBypass: case kCopyThreshold: case kTimestamp: return length < 4 ? error(kEinval) : 0;      // (taken)
    case kTransmitOffsets: if (length < 4) return error(kEinval); socket->transmit_offsets = number != 0; return 0;
    // PACKET_VERSION and PACKET_RESERVE say how a ring is laid out: only while there is none
    case kVersion:
      if (length < 4 || number > 2) return error(kEinval);
      if (socket->receive_block || socket->transmit_block) return error(kEbusy);
      socket->version = uint8_t(number);
      return 0;
    case kReserve:
      if (length < 4 || number > 0xFFFF) return error(kEinval);
      if (socket->receive_block || socket->transmit_block) return error(kEbusy);
      socket->reserve = number;
      return 0;
    case kReceiveRing: case kTransmitRing: {
      // struct tpacket_req: tp_block_size, tp_block_nr, tp_frame_size, tp_frame_nr; for TPACKET_V3 struct tpacket_req3, which adds tp_retire_blk_tov,
      // tp_sizeof_priv and tp_feature_req_word. A ring of no blocks takes the ring away. Neither while a program has the ring mapped.
      const bool transmit = name == kTransmitRing;
      const ring::Version version = ring::Version(socket->version);
      const uint64_t size = version == ring::Version::V3 ? 28 : 16;
      if (length < size) return error(kEinval);
      uint8_t bytes[28] = {};
      if (!p.read_user(value, ByteView(bytes).first(size))) return error(kEfault);
      ByteReader request{ConstByteView(bytes)};
      ring::Request r;
      r.block_size = request.le32();
      r.block_count = request.le32();
      r.frame_size = request.le32();
      r.frame_count = request.le32();
      r.retire_ms = request.le32();
      r.private_size = request.le32();
      r.features = request.le32();
      BlockRef& block = transmit ? socket->transmit_block : socket->receive_block;
      if (block && block.get()->uses() > 1) return error(kEbusy);
      if (r.block_count == 0) {
        if (r.frame_count != 0) return error(kEinval);
        block.reset();
        (transmit ? socket->transmit_request : socket->receive_request) = ring::Request();
        return 0;
      }
      if (transmit && version == ring::Version::V3 && (r.retire_ms != 0 || r.private_size != 0 || r.features != 0)) return error(kEinval);
      if (!ring::valid(version, r, transmit ? 0 : socket->reserve)) return error(kEinval);
      if (r.bytes() > kLargestRing) return error(kEnomem);
      BlockRef made(MemoryBlock::make(ipc->allocator(), r.bytes()));
      if (!made) return error(kEnomem);
      block = static_cast<BlockRef&&>(made);
      if (transmit) {
        socket->transmit_request = r;
        socket->transmitter.start(version, r);
      } else {
        socket->receive_request = r;
        socket->receiver.start(version, r, socket->reserve);
        socket->has_held = false;      // (a frame that waited for recv() is not what the ring starts with)
      }
      return 0;
    }
    case kFanout: {
      // An int: the group's number in the low half, how frames are shared out and flags in the high half; or struct fanout_args, which adds the most
      // members the group may have. A new number makes a group; a known one is joined if everything about it is the same.
      if (length < 4) return error(kEinval);
      const uint16_t id = uint16_t(number), type_flags = uint16_t(number >> 16), flags = type_flags & 0xFF00;
      const uint8_t type = uint8_t(type_flags);
      uint32_t most = LinuxIpcManager::kPacketSockets;
      if (length >= 8) {
        ByteReader more{ConstByteView(raw).subview(4, 4)};
        const uint32_t asked = more.le32();
        if (asked > 65536) return error(kEinval);
        if (asked != 0 && asked < most) most = asked;
      }
      if (type > kFanoutCbpf + 1 || (flags & ~(kFanoutFlagRollover | kFanoutFlagUniqueId | kFanoutFlagIgnoreOutgoing | kFanoutFlagDefragment)) != 0) return error(kEinval);
      if (type == kFanoutCbpf + 1) return error(kEinval);      // PACKET_FANOUT_EBPF: there are no such programs here
      if (type == kFanoutRollover && (flags & kFanoutFlagRollover)) return error(kEinval);
      if (socket->fanout >= 0) return error(kEalready);
      if (socket->protocol == 0) return error(kEinval);      // (a socket that listens for nothing has nothing to share)
      LinuxIpcManager::PacketFanout* group = nullptr;
      if (flags & kFanoutFlagUniqueId) {      // the kernel picks a number nobody uses
        if (id != 0) return error(kEinval);
        for (uint32_t candidate = 0; candidate < 65536 && !group; ++candidate) {      // (from 0, as Linux counts)
          bool taken = false;
          for (const LinuxIpcManager::PacketFanout& other : ipc->packet_fanouts) taken = taken || (other.used && other.id == candidate);
          if (taken) continue;
          for (LinuxIpcManager::PacketFanout& free : ipc->packet_fanouts)
            if (!free.used && !group) { group = &free; group->id = uint16_t(candidate); }
          break;
        }
        if (!group) return error(kEnospc);
      } else {
        for (LinuxIpcManager::PacketFanout& other : ipc->packet_fanouts)
          if (other.used && other.id == id) group = &other;
        if (group) {
          if (group->type != type || group->flags != (flags & ~kFanoutFlagUniqueId) || group->protocol != socket->protocol || group->interface != socket->interface) return error(kEinval);
          if (group->count >= group->most || group->count >= LinuxIpcManager::kPacketSockets) return error(kEnospc);
        } else {
          for (LinuxIpcManager::PacketFanout& free : ipc->packet_fanouts)
            if (!free.used && !group) { group = &free; group->id = id; }
          if (!group) return error(kEnospc);
        }
      }
      if (!group->used) {
        group->used = true;
        group->type = type;
        group->flags = flags & ~kFanoutFlagUniqueId;
        group->protocol = socket->protocol;
        group->interface = socket->interface;
        group->most = most;
        group->count = 0;
      }
      group->members[group->count++] = link;
      socket->fanout = int32_t(group - &ipc->packet_fanouts[0]);
      if (group->flags & kFanoutFlagIgnoreOutgoing) {
        socket->ignore_outgoing = true;
        listen(*socket);
      }
      return 0;
    }
    case kFanoutData: {      // PACKET_FANOUT_CBPF's program (struct sock_fprog, as SO_ATTACH_FILTER takes): the member is its answer modulo how many there are
      if (socket->fanout < 0) return error(kEinval);
      LinuxIpcManager::PacketFanout& group = ipc->packet_fanouts[socket->fanout];
      if (group.type != kFanoutCbpf || length < 16) return error(kEinval);
      ByteReader program{ConstByteView(raw)};
      const uint16_t count = program.le16();
      program.skip(6);
      const uint64_t at = program.le64();
      if (count == 0 || count > bpf::kMostInstructions) return error(kEinval);
      UniquePointer<ByteBuffer> chooser = UniquePointer<ByteBuffer>::make(ipc->allocator(), ipc->allocator());
      if (!chooser || !chooser->put_zeros(uint64_t{count} * bpf::kInstructionSize)) return error(kEnomem);
      if (!p.read_user(at, chooser->writable_view())) return error(kEfault);
      if (!bpf::valid(chooser->view())) return error(kEinval);
      group.chooser = static_cast<UniquePointer<ByteBuffer>&&>(chooser);
      return 0;
    }
    default: return error(kEnoprotoopt);
  }
}

uint64_t packet_getsockopt(Process& p, uint64_t link, uint64_t level, uint64_t name, uint64_t value, uint64_t length_at) {
  uint32_t room = 0;
  if (!read_u32(p, length_at, &room)) return error(kEfault);
  if (int32_t(room) < 0) return error(kEinval);
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEbadf);
  uint8_t raw[16] = {};
  ByteWriter w{ByteView(raw)};
  uint32_t requested = 0;      // (PACKET_HDRLEN reads a version from where it writes its answer)
  if (level == kSolPacket && name == kHeaderLength && room >= 4 && !read_u32(p, value, &requested)) return error(kEfault);
  {
    Locked locked(ipc->sync());
    Socket* socket = socket_of(*ipc, link);
    if (!socket) return error(kEbadf);
    if (level == kSolSocket) {
      switch (name) {
        case kSoType: w.le32(socket->cooked ? 2 : 3); break;
        case kSoError: w.le32(0); break;
        case kSoSndBuf: w.le32(socket->send_buffer); break;
        case kSoRcvBuf: w.le32(socket->receive_buffer); break;
        case kSoProtocol: w.le32(uint32_t((socket->protocol & 0xFF) << 8 | socket->protocol >> 8)); break;      // (as it was handed in: network order)
        case kSoDomain: w.le32(uint32_t(kAfPacket)); break;
        case kSoLockFilter: w.le32(socket->locked_filter ? 1 : 0); break;
        case kSoRcvTimeo: case kSoRcvTimeoNew:
          w.le64(socket->receive_timeout_ns == ~uint64_t{0} ? 0 : socket->receive_timeout_ns / 1000000000ull);
          w.le64(socket->receive_timeout_ns == ~uint64_t{0} ? 0 : socket->receive_timeout_ns % 1000000000ull / 1000);
          break;
        default: return error(kEnoprotoopt);
      }
    } else if (level == kSolPacket) {
      switch (name) {
        case kStatistics:      // struct tpacket_stats: frames taken and frames lost since this was last asked (TPACKET_V3: and how often the ring was full)
          pump_locked(*ipc, *socket, p.host().realtime_ns(), p.host().monotonic_ns());
          for (SharedPointer<network::IFrameTap>& tap : socket->taps)
            if (tap) socket->drops += tap->take_dropped();
          w.le32(socket->packets + socket->drops);
          w.le32(socket->drops);
          if (socket->version == 2) w.le32(socket->receiver.frozen());
          socket->packets = 0;
          socket->drops = 0;
          break;
        case kVersion: w.le32(socket->version); break;
        case kFanout: {      // the group's number, how it shares frames out and its flags, in one word (0: in no group)
          if (socket->fanout < 0) { w.le32(0); break; }
          const LinuxIpcManager::PacketFanout& group = ipc->packet_fanouts[socket->fanout];
          w.le32(uint32_t{group.id} | uint32_t{group.type} << 16 | uint32_t{group.flags} << 16);
          break;
        }
        case kTransmitOffsets: w.le32(socket->transmit_offsets ? 1 : 0); break;
        case kHeaderLength: {      // the size of a ring frame's header in the version the program names
          if (room < 4 || requested > 2) return error(kEinval);
          w.le32(ring::header_size(requested));
          break;
        }
        case kAuxData: w.le32(socket->aux_data ? 1 : 0); break;
        case kIgnoreOutgoing: w.le32(socket->ignore_outgoing ? 1 : 0); break;
        case kReserve: w.le32(socket->reserve); break;
        case kLoss: w.le32(socket->skip_wrong ? 1 : 0); break;
        case kQdiscBypass: case kCopyThreshold: case kTimestamp: w.le32(0); break;
        default: return error(kEnoprotoopt);
      }
    } else {
      return error(kEnoprotoopt);
    }
  }
  const uint64_t n = room < w.position() ? room : w.position();
  if (n != 0 && !p.write_user(value, ConstByteView(raw).first(n))) return error(kEfault);
  return p.write_user_u32(length_at, uint32_t(n)) ? 0 : error(kEfault);
}

uint64_t packet_pending(Process& p, uint64_t link, uint64_t argument) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return error(kEbadf);
  uint32_t size = 0;
  {
    Locked locked(ipc->sync());
    Socket* socket = socket_of(*ipc, link);
    if (!socket) return error(kEbadf);
    pump_locked(*ipc, *socket, p.host().realtime_ns(), p.host().monotonic_ns());
    if (!socket->receive_block && fetch_locked(*ipc, *socket)) size = uint32_t(shown(*socket, *socket->held).size());
  }
  return p.write_user_u32(argument, size) ? 0 : error(kEfault);
}

bool packet_ready_locked(uint64_t link, uint64_t realtime_ns, uint64_t monotonic_ns) {
  LinuxIpcManager* ipc = existing_linux_ipc();
  if (!ipc) return false;
  Socket* socket = socket_of(*ipc, link);
  if (!socket) return false;
  if (!socket->receive_block) return fetch_locked(*ipc, *socket);
  pump_locked(*ipc, *socket, realtime_ns, monotonic_ns);
  return socket->receiver.readable(receive_ring(*socket));
}

uint32_t packet_rings(uint64_t link, BlockRef blocks[2], uint64_t sizes[2]) {
  LinuxIpcManager* ipc = system_linux_ipc();
  if (!ipc) return 0;
  Locked locked(ipc->sync());
  Socket* socket = socket_of(*ipc, link);
  if (!socket) return 0;
  uint32_t count = 0;
  for (int transmit = 0; transmit < 2; ++transmit) {
    BlockRef& block = transmit ? socket->transmit_block : socket->receive_block;
    if (!block) continue;
    block.get()->retain();      // (a use of its own for whoever asked)
    blocks[count] = BlockRef(block.get());
    sizes[count] = (transmit ? socket->transmit_request : socket->receive_request).bytes();
    ++count;
  }
  return count;
}

}  // namespace process::linuxabi
