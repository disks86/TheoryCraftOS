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
#include "Hardware/Bus/Usb/UsbPortWatcher.hpp"
#include "Hardware/StorageDevice/IRemovableMediaSource.hpp"
#include "Hardware/StorageDevice/IStorageDevice.hpp"
#include "ObjectContainer/List.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/SharedPointer.hpp"
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
// A change to the set of devices (see SystemStorageDevices::poll).
struct StorageEvent {
  enum class Kind : uint8_t { Added, Removed };
  Kind kind = Kind::Added;
  uint32_t id = 0;                         // the device's id, which stays the same for as long as it is plugged in
  SharedPointer<IStorageDevice> device;    // a removed device is shut down and present() is false; it lives while anyone holds this
  bool ejected = false;                    // Removed: it was ejected (safe to unplug), not pulled out
};

enum class EjectResult : uint8_t {
  Ok,            // the device has been stopped and taken out of the list: safe to unplug
  NotFound,      // no such device (any more)
  NotSupported,  // a device that cannot be removed
  Failed,        // it could not be flushed or stopped; it is as it was
};

class SystemStorageDevices {
 public:
  // `usb` may be null; without it USB mass storage is left to the firmware (which
  // cannot see devices on a controller this OS has taken over).
  SystemStorageDevices(IPciBus& pci, IUsbBus* usb, allocators::IAllocator& allocator, efi::SystemTable* st)
      : pci_(pci), usb_(usb), allocator_(allocator), st_(st), devices_(allocator), pending_(allocator), watcher_(usb) {}

  void initialize();
  // A source of removable media outside the buses watched here (the SD slot). poll() asks it for new devices; one that is not present any more is taken out like a pulled USB stick.
  // The source must outlive this object. False when the list (kMaxSources) is full.
  bool add_source(IRemovableMediaSource* source);
  // Stops every device and frees them.
  void shutdown();

  // The devices there are now. at() hands out a plain pointer, which is only good while the device stays plugged in: a program that
  // keeps one while hot-plug is running holds share() instead (and checks present()).
  uint32_t count() const;
  IStorageDevice* at(uint32_t index) const;
  uint32_t id(uint32_t index) const;
  SharedPointer<IStorageDevice> share(uint32_t index) const;

  // Hot-plug: USB mass-storage devices plugged in or pulled out since start-up. Returns one change per call (false when there is none);
  // call it every few hundred milliseconds from one thread. A new device is already initialized and in the list; a removed one has been
  // taken out of the list and shut down (requests to it fail), and is freed once everyone has let go of the event's pointer.
  // Only devices on host controllers that report hot-plug (xHCI) are seen; everything else is fixed from start-up on.
  bool poll(StorageEvent* event);

  // Safe removal of the USB device with this id: flushes it, stops it and takes it out of the list (reported as a Removed event with `ejected`).
  // Whoever has volumes on it unmounts them first (SystemFileSystem::eject does all of it).
  EjectResult eject(uint32_t id);
  bool can_eject(uint32_t id) const;

 private:
  struct Entry {
    uint32_t id = 0;
    SharedPointer<IStorageDevice> device;
  };

  void find_native(List<UniquePointer<IStorageDevice>>& found);
  void find_firmware(List<UniquePointer<IStorageDevice>>& found);
  void adopt(List<UniquePointer<IStorageDevice>>& found, bool report);
  SharedPointer<IStorageDevice> find(uint32_t id) const;
  void sweep_removed();

  IPciBus& pci_;
  IUsbBus* usb_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;
  mutable kernel::Mutex mutex_;   // the list of devices
  List<Entry> devices_;
  List<StorageEvent> pending_;    // changes found but not yet handed out
  uint32_t next_id_ = 1;
  UsbPortWatcher watcher_;        // the USB ports as last looked at
  static constexpr uint32_t kMaxSources = 4;
  IRemovableMediaSource* sources_[kMaxSources] = {};
};

}  // namespace hardware
