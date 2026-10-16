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

  // Queues one frame for sending, copying it before returning. Frames shorter
  // than kEthernetMinFrame are padded. False if the frame is empty or longer
  // than kEthernetMaxFrame, or there is no room in the transmit queue right now
  // (try again after receiving or a short wait).
  virtual bool transmit(ConstByteView frame) = 0;
  // Copies the oldest received frame into `buffer` (as much as fits, the rest is
  // dropped) and stores its full length in *length. False when no
  // frame is waiting.
  virtual bool receive(ByteView buffer, uint32_t* length) = 0;

  virtual NetworkStatistics statistics() const = 0;

 protected:
  constexpr INetworkInterface() = default;
  ~INetworkInterface() = default;
};

}  // namespace hardware
