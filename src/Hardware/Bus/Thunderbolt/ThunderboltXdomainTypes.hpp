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

// What a connection manager shows of another host on the cable, and the interface a service on top of it (ThunderboltIP) needs from the connection manager: the control channel and the
// host interface, the list of other hosts, and the opening of data paths to one of them. ThunderboltIcm (a firmware connection manager) and Usb4Domain (the software one) both have it.
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Thunderbolt/ThunderboltControl.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltRing.hpp"

namespace hardware {

// Another host on the cable.
struct TbXDomain {
  uint8_t remote_uuid[16] = {}, local_uuid[16] = {};
  uint8_t link = 0, depth = 0;           // Alpine Ridge: where it is
  uint64_t route = 0;                    // the route string that reaches it (the one the firmware names as local)
  bool discovered = false;               // the UUID and property exchange succeeded
  bool gave_up = false;                  // it did not, after the retries
  uint8_t attempts = 0;
  uint32_t vendor_id = 0, device_id = 0, generation = 0;
  char vendor_name[48] = {}, device_name[48] = {};
  uint32_t protocols[8] = {};            // the protocol ids of the services it offers (prtcid of each service directory; 1 is IP networking)
  uint32_t protocol_count = 0;
  bool has_network = false;              // it offers the network service (protocol 1) ...
  uint32_t network_flags = 0;            // ... with these flags (its prtcstns: ThunderboltNet's kFlag...)
  bool dual_lane = false, gen3 = false;  // how the firmware says the link runs
  bool tunnel = false;                   // a data path to it is approved (ICM_APPROVE_XDOMAIN)
};

// A service that talks to another host in XDomain messages of its own (ThunderboltIP): the messages that start with the service's UUID instead of the discovery protocol's. Returns true if the
// packet was a request of the service and was dealt with; the answers to the service's own requests are left to the request that waits for them.
class IThunderboltIpHandler {
 public:
  virtual bool handle_ip_packet(ConstByteView body) = 0;

 protected:
  constexpr IThunderboltIpHandler() = default;
  ~IThunderboltIpHandler() = default;
};

class IXdomainHost {
 public:
  virtual TbControl& control() = 0;
  virtual TbNhi& nhi() = 0;
  // Where the packets of the control channel go while a service waits for an answer (the connection manager itself, which handles the unasked ones).
  virtual IThunderboltEvents* event_sink() = 0;
  virtual uint64_t xdomain_count() const = 0;
  virtual const TbXDomain& xdomain(uint64_t index) const = 0;
  virtual void set_ip_handler(IThunderboltIpHandler* handler) = 0;
  // Offers the network service in the properties other hosts read, with these flags.
  virtual bool offer_network(uint32_t flags) = 0;
  // Opens the data path to the other host: we send on `transmit_path` from our ring `transmit_ring`, it sends on `receive_path` into our ring `receive_ring`. One path at a time.
  virtual bool approve_xdomain_paths(uint64_t index, uint16_t transmit_path, uint16_t transmit_ring, uint16_t receive_path, uint16_t receive_ring, const char** error) = 0;
  virtual bool disconnect_xdomain_paths(uint64_t index) = 0;
  // Routes a data ring's interrupt the way the control channel's is; nothing while interrupts are off.
  virtual void ring_interrupts(bool transmit, uint32_t hop, bool on) = 0;

 protected:
  IXdomainHost() = default;
  ~IXdomainHost() = default;
};

}  // namespace hardware
