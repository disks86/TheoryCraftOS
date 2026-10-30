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

  // The Wi-Fi side of this interface (scanning, joining a network), or nullptr when it is not a wireless adapter.
  virtual IWirelessInterface* wireless() { return nullptr; }

  virtual NetworkStatistics statistics() const = 0;

 protected:
  constexpr INetworkInterface() = default;
  ~INetworkInterface() = default;
};

}  // namespace hardware
