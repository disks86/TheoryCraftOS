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
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/StorageDevice/StorageDeviceBase.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// VMware's paravirtual SCSI adapter, PVSCSI (PCI 15AD:07C0; ESXi, Workstation, QEMU's `pvscsi`). Commands go through a request ring of 128-byte
// entries and come back on a completion ring; the device is told where the rings are through its command register. Polled, one command in flight.
// The logical units behind it (disks, optical drives) are PvscsiDevices that share the controller.
class PvscsiController {
 public:
  PvscsiController(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator, efi::SystemTable* st)
      : pci_(pci), device_(device), allocator_(allocator), st_(st) {}

  static bool supports(const PciDeviceInfo& d) { return d.vendor_id == 0x15AD && d.device_id == 0x07C0; }

  bool initialize();
  void shutdown();   // safe to repeat
  efi::SystemTable* system_table() const { return st_; }
  uint32_t max_targets() const { return 16; }

  struct Result {
    bool delivered = false;   // the adapter answered at all
    uint16_t host_status = 0; // 0: the command reached the unit
    uint8_t status = 0;       // SCSI status
  };
  Result command(uint32_t target, uint32_t lun, ConstByteView cdb, ByteView data, bool device_writes);

 private:
  void write_command(uint32_t command, const uint32_t* data, uint32_t words) const;
  uint32_t reg_read(uint32_t offset) const;
  void reg_write(uint32_t offset, uint32_t value) const;

  const IPciBus& pci_;
  PciDeviceInfo device_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;
  uint64_t registers_ = 0;
  ArrayView<uint8_t> memory_;   // ring state, request ring, completion ring, sense buffer: one page each
  kernel::Mutex mutex_;
  uint64_t next_context_ = 1;
  bool owns_device_ = false;
  bool started_ = false;
};

class PvscsiDevice final : public StorageDeviceBase {
 public:
  PvscsiDevice(SharedPointer<PvscsiController> controller, uint32_t target, allocators::IAllocator& allocator)
      : StorageDeviceBase(allocator, controller->system_table()), controller_(util::move(controller)), target_(target) {}

  static void discover(const SharedPointer<PvscsiController>& controller, allocators::IAllocator& allocator, List<UniquePointer<IStorageDevice>>& out);

  const char* name() const override { return optical_ ? "PVSCSI optical drive" : "PVSCSI disk"; }
  bool initialize() override;
  void shutdown() override;

 protected:
  bool transfer(bool write, uint64_t lba, uint32_t blocks, ByteView dma) override;
  bool flush_cache() override;

 private:
  bool run(ConstByteView cdb, ByteView data, bool device_writes);

  SharedPointer<PvscsiController> controller_;
  uint32_t target_;
  bool optical_ = false;
};

}  // namespace hardware
