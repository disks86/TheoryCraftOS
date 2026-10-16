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
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "hardware/Bus/Usb/IUsbBus.hpp"
#include "hardware/StorageDevice/IStorageDevice.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/UniquePointer.hpp"
#include "uefi.hpp"

namespace hardware {

// Finds every storage device the machine has and brings them up. PCI storage
// controllers get a native driver where this OS has one (virtio-blk, virtio-scsi,
// NVMe, AHCI/SATA, legacy IDE), as do USB mass-storage devices on controllers the
// USB bus drives; everything else the firmware can drive - USB on controllers
// without a native driver, SCSI adapters, SD/MMC, floppy - is
// used through the firmware's Block I/O driver instead. Partitions are not
// listed: only whole devices.
class SystemStorageDevices {
 public:
  // `usb` may be null; without it USB mass storage is left to the firmware (which
  // cannot see devices on a controller this OS has taken over).
  SystemStorageDevices(IPciBus& pci, IUsbBus* usb, allocators::IAllocator& allocator, efi::SystemTable* st)
      : pci_(pci), usb_(usb), allocator_(allocator), st_(st), devices_(allocator) {}

  void initialize();
  // Stops every device and frees them.
  void shutdown();

  uint32_t count() const { return static_cast<uint32_t>(devices_.count()); }
  IStorageDevice* at(uint32_t index) const { return index < devices_.count() ? devices_[index].get() : nullptr; }

 private:
  void find_native();
  void find_firmware();

  IPciBus& pci_;
  IUsbBus* usb_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;
  List<UniquePointer<IStorageDevice>> devices_;
};

}  // namespace hardware
