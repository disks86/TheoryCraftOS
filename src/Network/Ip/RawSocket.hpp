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
#pragma once

#include "Allocators/IAllocator.hpp"
#include "Kernel/Sync.hpp"
#include "Network/Ip/IIpLayer.hpp"
#include "Network/Udp/IUdpSocket.hpp"
#include "ObjectContainer/List.hpp"
#include "ObjectContainer/Queue.hpp"
#include "Utility/SharedPointer.hpp"

namespace network {

// What a raw socket carries.
enum class RawKind : uint8_t {
  Packets,      // every arriving packet of one IP protocol, and whatever the program builds of it (SOCK_RAW)
  Echo,         // ICMP echo only: requests go out with the socket's identifier, and the replies to them come back ("ping sockets", SOCK_DGRAM)
};

// A socket below UDP and TCP: IP packets of one protocol. It has the shape of a datagram socket (IUdpSocket: a port means nothing to it, except that an
// echo socket's local port is its identifier), so that what holds datagram sockets can hold these.
//
// What is received: of IPv4 the whole packet, IP header first (as raw sockets hand it out); of IPv6 what follows the headers; an echo socket's replies
// without any IP header. What is sent is the protocol's bytes (an ICMP message, say); with set_header_included() it is a whole IPv4 packet, of which the
// destination, the protocol, the time to live and what follows the header are used, and the rest is written anew.
class IRawSocket : public IUdpSocket {
 public:
  virtual void set_header_included(bool on) = 0;
  // The time to live (hop limit) packets go out with; 0: the usual one.
  virtual void set_hop_limit(uint8_t hops) = 0;
  // ICMP messages not to be received, by type: bit (type & 31) of word (type >> 5), for types 0..255 (ICMP_FILTER and ICMP6_FILTER).
  static constexpr uint32_t kTypeWords = 8;
  virtual void set_blocked_types(ArrayView<const uint32_t> blocked) = 0;

 protected:
  IRawSocket() = default;
  ~IRawSocket() = default;
};

// The default IRawSocket. Thread-safe the way UdpSocket is: arriving packets wait in a queue under a leaf lock, and what decides which packets are
// taken is fixed at creation or atomic.
class RawSocket final : public IRawSocket {
 public:
  static constexpr uint32_t kQueueLimit = 32;
  static constexpr uint8_t kProtocolRaw = 255;      // IPPROTO_RAW: only sends, whole packets

  RawSocket(IIpLayer& ip, allocators::IAllocator& allocator, bool v6, uint8_t protocol, RawKind kind, uint16_t identifier)
      : ip_(ip), v6_(v6), protocol_(protocol), kind_(kind), identifier_(identifier), queue_(allocator) {}

  uint16_t local_port() const override { return kind_ == RawKind::Echo ? identifier_ : 0; }
  bool send_to(Ipv4Address address, uint16_t port, ConstByteView data) override { return send_to_ip(IpAddress(address), port, data); }
  bool receive_from(ByteView buffer, Ipv4Address* from_address, uint16_t* from_port, uint32_t* length) override;
  bool send_to_ip(const IpAddress& address, uint16_t port, ConstByteView data) override;
  bool receive_from_ip(ByteView buffer, IpAddress* from_address, uint16_t* from_port, uint32_t* length) override;
  uint32_t pending() const override;
  void close() override;
  void set_header_included(bool on) override { __atomic_store_n(&header_included_, on, __ATOMIC_RELEASE); }
  void set_hop_limit(uint8_t hops) override { __atomic_store_n(&hop_limit_, hops, __ATOMIC_RELEASE); }
  void set_blocked_types(ArrayView<const uint32_t> blocked) override;

  // The raw manager hands every arriving packet in here; the socket takes the ones that are for it. `packet` is the IPv4 packet with its header of
  // `header` bytes, or (header 0) what follows an IPv6 packet's headers.
  void deliver(bool v6, uint8_t protocol, const IpAddress& from, ConstByteView packet, uint32_t header);
  bool closed() const { return __atomic_load_n(&closed_, __ATOMIC_ACQUIRE); }
  bool v6() const { return v6_; }
  RawKind kind() const { return kind_; }

 private:
  struct Packet {
    IpAddress from;
    uint16_t length;
    uint8_t data[kMtu];
  };

  IIpLayer& ip_;
  const bool v6_;
  const uint8_t protocol_;
  const RawKind kind_;
  const uint16_t identifier_;
  bool closed_ = false;
  bool header_included_ = false;
  uint8_t hop_limit_ = 0;
  uint32_t blocked_[kTypeWords] = {};
  Queue<Packet> queue_;
  mutable kernel::Mutex queue_lock_;      // guards the queue (a leaf: nothing is taken while it is held)
};

// Owns the raw sockets of a stack and shows each arriving packet to all of them. With no raw socket open (nearly always) a packet costs one atomic read.
class RawManager {
 public:
  RawManager(IIpLayer& ip, allocators::IAllocator& allocator) : ip_(ip), allocator_(allocator), sockets_(allocator) {}

  // `identifier`: an echo socket's (0: one nobody uses). Null if the identifier is in use or memory ran out.
  SharedPointer<RawSocket> open(bool v6, uint8_t protocol, RawKind kind, uint16_t identifier);
  void deliver(bool v6, uint8_t protocol, const IpAddress& from, ConstByteView packet, uint32_t header);
  // Forgets sockets that were closed or that nobody holds any more.
  void reap();

 private:
  bool identifier_in_use(bool v6, uint16_t identifier) const;

  IIpLayer& ip_;
  allocators::IAllocator& allocator_;
  mutable kernel::Mutex mutex_;      // guards the list (held while a packet is shown to the sockets; a socket's queue lock is below it)
  List<SharedPointer<RawSocket>> sockets_;
  uint32_t open_ = 0;                // how many are in the list (atomic: read without the lock)
  uint16_t next_identifier_ = 0x4000;
};

}  // namespace network
