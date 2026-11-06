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
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/NetworkInterface/INetworkInterface.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"
#include "uefi.hpp"

namespace hardware {

// Finds every network interface the machine has and brings them up. Each PCI
// network controller gets a native driver where this OS has one (virtio-net,
// Intel e1000/e1000e/igb/igc, Realtek RTL8139 and RTL8111/8168/8125, VMware
// vmxnet3, AMD PCnet); USB adapters plugged in at boot (CDC-ECM, CDC-NCM, Realtek
// RTL815x through their CDC configuration, ASIX AX88179) come next; any other NIC
// the firmware can drive is used through the firmware's driver instead.
class SystemNetworkInterfaces {
 public:
  SystemNetworkInterfaces(IPciBus& pci, allocators::IAllocator& allocator, efi::SystemTable* st)
      : pci_(pci), allocator_(allocator), st_(st), interfaces_(allocator) {}

  // The drivers of the newer controllers map their registers through this (uncached, above 4 GiB too); without one they use the firmware's mapping.
  void set_register_mapper(IRegisterMapper* mapper) { mapper_ = mapper; }
  // The USB adapters are looked for on these controllers' root ports (none without it).
  void set_usb(IUsbBus* usb) { usb_ = usb; }

  void initialize();
  // Stops every interface and frees them.
  void shutdown();

  // Takes an interface that was made elsewhere (it needs more than the PCI bus to be made: a file system for firmware, say). It is stopped and freed with the others.
  bool adopt(UniquePointer<INetworkInterface> interface) { return interface && interfaces_.push_back(util::move(interface)); }

  uint32_t count() const { return static_cast<uint32_t>(interfaces_.count()); }
  INetworkInterface* at(uint32_t index) const {
    return index < interfaces_.count() ? interfaces_[index].get() : nullptr;
  }

 private:
  void find_native();
  void find_usb();
  void find_firmware();

  IPciBus& pci_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;
  IRegisterMapper* mapper_ = nullptr;
  IUsbBus* usb_ = nullptr;
  List<UniquePointer<INetworkInterface>> interfaces_;
};

}  // namespace hardware
