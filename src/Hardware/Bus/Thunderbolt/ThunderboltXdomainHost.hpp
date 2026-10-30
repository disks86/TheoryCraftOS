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

// The XDomain side of a software connection manager: the other hosts on the cable (discovery of their UUID and properties with the XDomain protocol, the answers to what they ask of us, the
// ThunderboltIP messages handed on to the service), with the opening and closing of data paths left to the owner. ThunderboltIcm does the same with the firmware's help; here the manager has
// to find the host itself, since no event announces it.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltControl.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltXdomain.hpp"
#include "Hardware/Bus/Thunderbolt/ThunderboltXdomainTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

// What opens and closes the data paths to a host (the owner's tunnels).
class IXdomainPaths {
 public:
  virtual bool open_paths(const TbXDomain& xd, uint16_t transmit_path, uint16_t transmit_ring, uint16_t receive_path, uint16_t receive_ring, const char** error) = 0;
  virtual void close_paths(const TbXDomain& xd) = 0;

 protected:
  IXdomainPaths() = default;
  ~IXdomainPaths() = default;
};

class TbXdomainHost {
 public:
  TbXdomainHost(TbControl& control, allocators::IAllocator& allocator, IThunderboltEvents* sink, IXdomainPaths& paths) : control_(control), allocator_(allocator), sink_(sink), paths_(paths), list_(allocator), local_block_(allocator) {}

  // What this host says about itself, and the network service offered in it (the other host reads the properties after the next change of generation).
  bool set_identity(uint32_t vendor_id, uint32_t device_id, uint32_t device_revision, const char* vendor_name, const char* device_name);
  bool offer_network(uint32_t flags);
  void set_ip_handler(IThunderboltIpHandler* handler) { ip_handler_ = handler; }

  // A host was found behind the port that `route` names: its UUID is asked for on the next service() calls. `local_uuid` is ours (the host router's). Returns its index, or -1.
  int add(uint64_t route, const uint8_t local_uuid[16]);
  // It is gone (or the link went down): its path is closed and it is forgotten. The indexes of the ones after it do not change: its entry stays, empty.
  bool remove(uint64_t route);
  void clear();
  int find(uint64_t route) const;
  uint64_t count() const { return list_.count(); }
  const TbXDomain& at(uint64_t index) const { return list_[index]; }
  uint32_t live() const { uint32_t n = 0; for (uint64_t i = 0; i < list_.count(); ++i) n += live_[i]; return n; }
  bool present(uint64_t index) const { return index < list_.count() && live_[index]; }

  // One step of discovery for the first host that needs it (an exchange of up to a few requests); retries are spaced out.
  void service();
  // The packets that are XDomain: requests are answered from our identity, responses of the service go to its handler. True if taken.
  bool handle(TbPacketType type, ConstByteView body);

  bool approve(uint64_t index, uint16_t transmit_path, uint16_t transmit_ring, uint16_t receive_path, uint16_t receive_ring, const char** error);
  bool disconnect(uint64_t index);

 private:
  bool ensure_identity();
  bool discover(uint64_t index);
  void answer(ConstByteView body);

  TbControl& control_;
  allocators::IAllocator& allocator_;
  IThunderboltEvents* sink_;
  IXdomainPaths& paths_;
  List<TbXDomain> list_;
  bool live_[32] = {};
  List<uint32_t> local_block_;
  bool offer_network_ = false;
  uint32_t network_flags_ = 0, identity_[3] = {0, 1, 1};
  char vendor_name_[48] = "TheoryCraftOS", device_name_[48] = "TheoryCraftOS";
  uint32_t local_generation_ = 1, polls_ = 0;
  IThunderboltIpHandler* ip_handler_ = nullptr;
  static constexpr uint32_t kMaxHosts = 32;
};

}  // namespace hardware
