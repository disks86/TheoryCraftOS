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
#include "Hardware/NetworkInterface/NetworkTypes.hpp"
#include "Kernel/Sync.hpp"
#include "Network/NetworkTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "ObjectContainer/Queue.hpp"
#include "Utility/SharedPointer.hpp"

namespace network {

// How a frame came by (the numbers Linux gives a packet socket: PACKET_HOST, ...).
enum class FrameKind : uint8_t { Host = 0, Broadcast = 1, Multicast = 2, OtherHost = 3, Outgoing = 4 };

struct FrameInfo {
  FrameKind kind = FrameKind::Host;
  bool loopback = false;      // a packet this machine sent to itself (it never was a frame: it is shown with an Ethernet header of zero addresses)
  uint32_t length = 0;        // the whole frame's, also when less was copied
};

// What the link layer offers a frame tap.
class ILinkLayer {
 public:
  // Puts a whole Ethernet frame on the wire as it is. `origin` is the tap that sends it (which is not shown its own frame).
  virtual bool send_link_frame(ConstByteView frame, const void* origin) = 0;
  virtual hardware::MacAddress link_address() const = 0;
  // Hands an IP packet to this machine itself, as one sent to the loopback is.
  virtual bool loop_ip_packet(ConstByteView packet) = 0;

 protected:
  ILinkLayer() = default;
  ~ILinkLayer() = default;
};

// A tap on a network interface: the Ethernet frames it receives and sends, as they are, and a way to send frames of one's own making. What packet
// capture and the protocols that live beside IP (a DHCP client of a program's own, ARP tools) are built on.
class IFrameTap {
 public:
  // What to take from now on: frames of `ether_type`, or of every type (`all`); from the wire and/or the loopback; the interface's own outgoing frames
  // too (only with `all`). A new tap takes nothing.
  virtual void listen(uint16_t ether_type, bool all, bool wire, bool loopback, bool outgoing) = 0;
  // Takes the oldest frame: copies as much as fits `buffer` and says what it was. False if none is waiting.
  virtual bool receive(ByteView buffer, FrameInfo* info) = 0;
  virtual uint32_t pending() const = 0;
  // How many frames were lost to a full queue since this was last asked.
  virtual uint32_t take_dropped() = 0;
  // Sends a whole frame (14 to 1514 bytes). False if it cannot go now.
  virtual bool send(ConstByteView frame) = 0;
  // The same on the loopback: the IPv4 or IPv6 packet in the frame comes back to this machine. False for anything else.
  virtual bool send_looped(ConstByteView frame) = 0;
  virtual hardware::MacAddress mac_address() const = 0;
  virtual void close() = 0;

 protected:
  IFrameTap() = default;
  ~IFrameTap() = default;
};

// The default IFrameTap. Thread-safe as the sockets are: a queue under a leaf lock, and what is taken is one atomic word.
class FrameTap final : public IFrameTap {
 public:
  static constexpr uint32_t kQueueLimit = 64;
  static constexpr uint32_t kMaxFrame = hardware::kEthernetMaxFrame;

  FrameTap(ILinkLayer& link, allocators::IAllocator& allocator) : link_(link), queue_(allocator) {}

  void listen(uint16_t ether_type, bool all, bool wire, bool loopback, bool outgoing) override;
  bool receive(ByteView buffer, FrameInfo* info) override;
  uint32_t pending() const override;
  uint32_t take_dropped() override { return __atomic_exchange_n(&dropped_, 0u, __ATOMIC_ACQ_REL); }
  bool send(ConstByteView frame) override;
  bool send_looped(ConstByteView frame) override;
  hardware::MacAddress mac_address() const override { return link_.link_address(); }
  void close() override;

  // The tap manager shows every frame to every tap; a tap takes the ones it listens for. `header` and `payload` together are the frame (a looped
  // packet's header is made up on the way here).
  void deliver(FrameKind kind, bool loopback, ConstByteView header, ConstByteView payload);
  bool closed() const { return __atomic_load_n(&closed_, __ATOMIC_ACQUIRE); }

 private:
  struct Frame {
    FrameInfo info;
    uint8_t data[kMaxFrame];
  };
  static constexpr uint32_t kAll = 1u << 16, kWire = 1u << 17, kLoopback = 1u << 18, kOutgoing = 1u << 19, kListening = 1u << 20;

  ILinkLayer& link_;
  bool closed_ = false;
  uint32_t wanted_ = 0;      // the ether type and the flags above
  uint32_t dropped_ = 0;
  Queue<Frame> queue_;
  mutable kernel::Mutex queue_lock_;      // guards the queue (a leaf)
};

// Owns the frame taps of a stack. With none open (nearly always) a frame costs one atomic read.
class FrameTapManager {
 public:
  FrameTapManager(ILinkLayer& link, allocators::IAllocator& allocator) : link_(link), allocator_(allocator), taps_(allocator) {}

  SharedPointer<FrameTap> open();
  bool any() const { return __atomic_load_n(&open_, __ATOMIC_ACQUIRE) != 0; }
  // An Ethernet frame the interface received (its kind is worked out from where it was sent to), or sent (`origin`: the tap that sent it, if one did).
  void received(ConstByteView frame);
  void sent(ConstByteView frame, const void* origin);
  // An IP packet this machine sent to itself.
  void looped(ConstByteView ip_packet);
  void reap();

 private:
  void deliver(FrameKind kind, bool loopback, ConstByteView header, ConstByteView payload, const void* except);

  ILinkLayer& link_;
  allocators::IAllocator& allocator_;
  mutable kernel::Mutex mutex_;      // guards the list (held while a frame is shown to the taps; a tap's queue lock is below it)
  List<SharedPointer<FrameTap>> taps_;
  uint32_t open_ = 0;
};

}  // namespace network
