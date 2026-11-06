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
#include "Hardware/StorageDevice/QueuedStorage.hpp"
#include "Hardware/StorageDevice/Scsi/ScsiCommands.hpp"
#include "Utility/AllocatedView.hpp"
#include "Hardware/StorageDevice/StorageDeviceBase.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

constexpr uint16_t kVirtioScsiDevice = 0x1048;             // 0x1040 + device type 8
constexpr uint16_t kVirtioScsiTransitionalDevice = 0x1004;

// A virtio-scsi host adapter (QEMU's virtio-scsi-pci). Owns the transport, the request queues and the command slots; the logical units
// behind it (scsi-hd, scsi-cd) are VirtioScsiDevices that share them. Like virtio-blk: a request queue per online core (the device's
// num_queues, QEMU gives one per vCPU; queue 0 is control, 1 events, requests from 2 on), up to 64 command slots shared by every unit, each
// with a 64 KiB DMA buffer and its request and response headers (slot s on descriptors 3s..3s+2 of any queue), and on x86 an MSI-X entry
// per request queue sent to its core, checked at start (else polled).
class VirtioScsiController {
 public:
  static constexpr uint32_t kMaxQueues = VirtioPciDevice::kMaxQueues - 2;
  static constexpr uint32_t kSlotBytes = 64 * 1024;

  VirtioScsiController(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                       efi::SystemTable* st)
      : transport_(pci, device, allocator, st), allocator_(allocator), st_(st) {}

  bool initialize();
  void shutdown();  // safe to repeat
  void stop_interrupts() { disable_interrupts(); }   // (shutdown, before the kernel stops: completions are polled from then on)
  efi::SystemTable* system_table() const { return st_; }
  uint32_t max_targets() const { return max_targets_; }
  uint32_t depth() const { return slot_count_; }
  void describe(IPropertySink& sink) const;

  struct Result {
    bool delivered = false;  // the device answered at all
    uint8_t response = 0;    // virtio-scsi response code (0 = OK)
    uint8_t status = 0;      // SCSI status
  };
  // Runs one command at target:lun with an optional data stage (at most kSlotBytes) in one direction.
  Result command(uint32_t target, uint32_t lun, ConstByteView cdb, ByteView data, bool device_writes);
  // Reads (`into`) or writes (`from`) blocks of `block_size` from `lba` on target:0, several 64 KiB chunks at once, retrying a chunk a unit
  // attention interrupted.
  bool transfer(uint32_t target, bool write, uint64_t lba, uint32_t block_size, ByteView into, ConstByteView from);

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
  struct Queue {
    VirtioScsiController* owner = nullptr;
    Virtqueue ring;                       // guarded by `lock`
    kernel::WaitList lock;                // only the lock word: an IRQ-masking spinlock
    uint16_t index = 0;                   // the virtqueue (2..)
    uint32_t core = 0;
    uint8_t vector = 0;
    int32_t io_device = -1;
    uint64_t interrupts = 0;              // atomic
  };
  struct Slot {
    ByteView buffer;                      // kSlotBytes of DMA memory
    ByteView request, response;           // its headers
    CommandCompletion completion;         // status: response << 8 | SCSI status
  };

  bool submit(Queue& q, uint32_t slot, uint32_t target, uint32_t lun, ConstByteView cdb, uint64_t bytes, bool device_writes);
  Result finish(Queue& q, uint32_t slot);
  void reap(Queue& q);
  static kernel::Frame* on_queue_interrupt(kernel::Frame* frame, void* queue);
  Queue& queue_for_this_core();
  bool enable_interrupts();
  bool check_interrupts();
  void disable_interrupts();

  VirtioPciDevice transport_;
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;
  Virtqueue control_, event_;
  Queue queues_[kMaxQueues];
  uint32_t queue_count_ = 0;
  uint8_t core_queue_[64] = {};
  Slot slots_[SlotPool::kMaxSlots];
  uint32_t slot_count_ = 0;
  SlotPool pool_;
  ByteView slot_memory_, header_memory_;
  uint32_t max_targets_ = 0;
  uint32_t cdb_size_ = 32;
  uint32_t sense_size_ = 96;
  bool started_ = false;
  bool interrupts_ = false;
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
  void describe(IPropertySink& sink) const override { controller_->describe(sink); }

  bool read(uint64_t lba, ByteView buffer) override;
  bool write(uint64_t lba, ConstByteView buffer) override;
  bool flush() override;
  bool discard(uint64_t lba, uint64_t blocks) override;
  void stop_interrupts() override { controller_->stop_interrupts(); }

 protected:
  bool transfer(bool, uint64_t, uint32_t, ByteView) override { return false; }   // (read/write above do the work)

 private:
  // Runs a command, retrying while the target reports a unit attention.
  bool run(ConstByteView cdb, ByteView data, bool device_writes);

  SharedPointer<VirtioScsiController> controller_;
  uint32_t target_;
  bool optical_ = false;
  scsi::UnmapSupport unmap_;   // UNMAP when the unit is thin provisioned and says it takes it
};

}  // namespace hardware
