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
#include "Hardware/Bus/Virtio/VirtioPciDevice.hpp"
#include "Utility/AllocatedView.hpp"
#include "Hardware/StorageDevice/StorageDeviceBase.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

constexpr uint16_t kVirtioScsiDevice = 0x1048;             // 0x1040 + device type 8
constexpr uint16_t kVirtioScsiTransitionalDevice = 0x1004;

// A virtio-scsi host adapter (QEMU's virtio-scsi-pci). Owns the transport and the
// request queue; the logical units behind it (scsi-hd, scsi-cd) are
// VirtioScsiDevices that share it. One SCSI command is in flight at a time.
class VirtioScsiController {
 public:
  VirtioScsiController(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                       efi::SystemTable* st)
      : transport_(pci, device, allocator, st), allocator_(allocator), st_(st) {}

  bool initialize();
  void shutdown();  // safe to repeat
  efi::SystemTable* system_table() const { return st_; }
  uint32_t max_targets() const { return max_targets_; }

  struct Result {
    bool delivered = false;  // the device answered at all
    uint8_t response = 0;    // virtio-scsi response code (0 = OK)
    uint8_t status = 0;      // SCSI status
  };
  // Runs one command at target:lun0 with an optional data stage in one direction.
  Result command(uint32_t target, uint32_t lun, ConstByteView cdb, ByteView data, bool device_writes);

 private:
  struct RequestHeader {
    uint8_t lun[8];
    uint64_t id;
    uint8_t task_attribute, priority, crn;
    uint8_t cdb[32];
  } __attribute__((packed));
  struct ResponseHeader {
    uint32_t sense_length, residual;
    uint16_t status_qualifier;
    uint8_t status, response;
    uint8_t sense[96];
  } __attribute__((packed));

  kernel::Mutex mutex_;  // one command in flight on the shared queue
  VirtioPciDevice transport_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;
  Virtqueue control_, event_, request_;
  ByteView memory_;  // request and response headers
  uint32_t max_targets_ = 0;
  uint32_t cdb_size_ = 32;
  uint32_t sense_size_ = 96;
  bool started_ = false;
};

class VirtioScsiDevice final : public StorageDeviceBase {
 public:
  VirtioScsiDevice(SharedPointer<VirtioScsiController> controller, uint32_t target, allocators::IAllocator& allocator)
      : StorageDeviceBase(allocator, controller->system_table()), controller_(util::move(controller)), target_(target) {}

  static void discover(const SharedPointer<VirtioScsiController>& controller, allocators::IAllocator& allocator,
                       List<UniquePointer<IStorageDevice>>& out);

  const char* name() const override { return optical_ ? "virtio-scsi optical drive" : "virtio-scsi disk"; }
  bool initialize() override;
  void shutdown() override;

 protected:
  bool transfer(bool write, uint64_t lba, uint32_t blocks, ByteView dma) override;
  bool flush_cache() override;

 private:
  // Runs a command, retrying while the target reports a unit attention.
  bool run(ConstByteView cdb, ByteView data, bool device_writes);

  SharedPointer<VirtioScsiController> controller_;
  uint32_t target_;
  bool optical_ = false;
};

}  // namespace hardware
