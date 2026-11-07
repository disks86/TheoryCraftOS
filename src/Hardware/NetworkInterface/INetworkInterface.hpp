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
#include "Hardware/IHardware.hpp"
#include "Hardware/NetworkInterface/NetworkTypes.hpp"

namespace hardware {

enum class LinkDuplex : uint8_t { Unknown, Half, Full };

class IWirelessInterface;

// An Ethernet network interface. It moves raw frames - destination MAC, source
// MAC, EtherType, payload - and knows nothing about protocols above that.
// Polled: nothing blocks, and received frames queue up inside the interface
// until receive() collects them. Implementations live in subfolders by
// vendor/transport (Virtio/, Intel/, Realtek/) plus a UEFI fallback.
class INetworkInterface : public IHardware {
 public:
  // The interface's own hardware address.
  virtual MacAddress mac_address() const = 0;
  // True while a cable is connected / the link is up.
  virtual bool link_up() const = 0;
  // The speed the link runs at, in megabits per second (10, 100, 1000, ...); 0 when the link is down or the adapter does not say. And whether
  // it is full duplex (Unknown when the adapter does not say).
  virtual uint32_t link_speed_mbps() const { return 0; }
  virtual LinkDuplex link_duplex() const { return LinkDuplex::Unknown; }

  // Queues one frame for sending, copying it before returning. Frames shorter
  // than kEthernetMinFrame are padded. False if the frame is empty or longer
  // than kEthernetMaxFrame, or there is no room in the transmit queue right now
  // (try again after receiving or a short wait).
  virtual bool transmit(ConstByteView frame) = 0;
  // Copies the oldest received frame into `buffer` (as much as fits, the rest is
  // dropped) and stores its full length in *length. False when no
  // frame is waiting.
  virtual bool receive(ByteView buffer, uint32_t* length) = 0;

  // Asks the interface to deliver multicast frames too (they are dropped by default on hardware that filters them), for as long as
  // `enable` is true. True if frames to any multicast address will now arrive; false if the interface cannot say so.
  virtual bool accept_all_multicast(bool enable) {
    (void)enable;
    return false;
  }

  // True for an interface whose link comes and goes with a connection (Wi-Fi, a cable to another host) rather than with a cable to a network: its network stack then asks for an address
  // when the link comes up and gives it up when it goes down.
  virtual bool link_comes_and_goes() const { return false; }

  // True for an interface that is a cable to one other host: there may be no DHCP server on it, so its network stack makes up a link-local address (169.254.x.y, RFC 3927) when DHCP fails.
  virtual bool direct_link() const { return false; }

  // The Wi-Fi side of this interface (scanning, joining a network), or nullptr when it is not a wireless adapter.
  virtual IWirelessInterface* wireless() { return nullptr; }

  // --- Hardware help. Interfaces without it keep these defaults, which do everything in software.
  virtual NetworkOffloads offloads() const { return NetworkOffloads{}; }
  // receive() with what the hardware found about the frame's checksums.
  virtual bool receive_frame(ByteView buffer, ReceivedFrame* frame) {
    *frame = ReceivedFrame{};
    return receive(buffer, &frame->length);
  }
  // Receive queues one at a time, for a stack that empties them on several threads at once (RSS spreads flows over them, so a flow's frames
  // stay in one queue, in order). An interface with one queue (the default) has only queue 0, which receive_frame() reads; one with more
  // must make receive_frame_from() on different queues safe to call at the same time (and with transmit), each queue from one thread at a time.
  virtual uint32_t receive_queue_count() const { return 1; }
  virtual bool receive_frame_from(uint32_t queue, ByteView buffer, ReceivedFrame* frame) { return queue == 0 && receive_frame(buffer, frame); }

  // transmit() with a transport checksum to finish (done in software here: the frame is copied first).
  virtual bool transmit_frame(ConstByteView frame, TransmitChecksum checksum) {
    if (!checksum.requested()) return transmit(frame);
    uint8_t copy[kEthernetMaxFrame];
    const ByteView writable = ByteView(copy).first(frame.size() < sizeof copy ? frame.size() : sizeof copy);
    if (frame.size() > sizeof copy || !writable.copy_from(frame) || !finish_transmit_checksum(writable, checksum)) return false;
    return transmit(writable);
  }

  // TCP segmentation offload: `frame` is one IPv4 TCP segment of up to offloads().max_segmentation_bytes bytes, its checksum requested as for
  // transmit_frame() (the seed covers the whole segment's length). The interface sends it as frames of at most `mss` bytes of payload, each
  // with its own IP length, identification and checksum, sequence number and TCP checksum; FIN and PSH only on the last. False (nothing
  // sent) if it cannot now; only called on an interface whose offloads() say it can.
  virtual bool transmit_segmented(ConstByteView frame, TransmitChecksum checksum, uint16_t mss) {
    (void)frame;
    (void)checksum;
    (void)mss;
    return false;
  }

  // Interrupts: an interface that can interrupt calls `wake(context)` from its interrupt handler whenever it has something new, so whoever polls
  // it may sleep in between. True once an interrupt has been seen to arrive (the interface tests it); false leaves the interface polled.
  // disable_interrupts() is called before `context` goes away.
  virtual bool enable_interrupts(NetworkWake wake, void* context) {
    (void)wake;
    (void)context;
    return false;
  }
  virtual void disable_interrupts() {}
  virtual bool interrupts_enabled() const { return false; }
  // Interrupt mitigation (as Linux's NAPI): an interrupt masks the interface's interrupt until whoever polls it has taken everything there
  // was and calls this, so a burst of frames costs one interrupt. The interface also picks its interrupt rate from the traffic it saw.
  virtual void rearm_interrupts() {}

  virtual NetworkStatistics statistics() const = 0;

 protected:
  constexpr INetworkInterface() = default;
  ~INetworkInterface() = default;
};

}  // namespace hardware
