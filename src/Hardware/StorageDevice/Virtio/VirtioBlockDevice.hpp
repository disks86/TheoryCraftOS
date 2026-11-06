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
#include "Hardware/StorageDevice/StorageDeviceBase.hpp"

namespace hardware {

constexpr uint16_t kVirtioBlockDevice = 0x1042;             // 0x1040 + device type 2
constexpr uint16_t kVirtioBlockTransitionalDevice = 0x1001;

// A virtio-blk disk (QEMU's virtio-blk-pci), modern virtio 1.x, with many requests in flight:
//
// - A request queue per online core when the device offers several (VIRTIO_BLK_F_MQ; QEMU gives one per vCPU), up to kMaxQueues: a
//   thread posts on its own core's queue under that queue's lock only.
// - A pool of up to 64 command slots, each with a 64 KiB DMA buffer and its header and status byte. Slot s always uses descriptors 3s..3s+2
//   of whichever queue it goes on, so any slot fits on any queue and the used ring's id names the slot. StorageInfo::queue_depth is the slot
//   count.
// - On x86 each queue's MSI-X entry goes to its core with a vector of its own; delivery is checked per queue at start, else waiters poll.
class VirtioBlockDevice final : public StorageDeviceBase {
 public:
  static constexpr uint32_t kMaxQueues = 16;
  static constexpr uint32_t kSlotBytes = 64 * 1024;

  VirtioBlockDevice(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                    efi::SystemTable* st)
      : StorageDeviceBase(allocator, st), transport_(pci, device, allocator, st) {}

  const char* name() const override { return "virtio-blk"; }
  bool initialize() override;
  void shutdown() override;
  void describe(IPropertySink& sink) const override;

  bool read(uint64_t lba, ByteView buffer) override;
  bool write(uint64_t lba, ConstByteView buffer) override;
  bool flush() override;
  bool discard(uint64_t lba, uint64_t blocks) override;
  StorageStatistics statistics() const override;

  uint32_t queue_count() const { return queue_count_; }
  bool uses_interrupts() const { return interrupts_; }

 protected:
  bool transfer(bool, uint64_t, uint32_t, ByteView) override { return false; }   // (read/write above do the work)

 private:
  struct Request {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;  // always in 512-byte units
  };
  struct Queue {
    VirtioBlockDevice* owner = nullptr;   // (for the interrupt handler)
    Virtqueue ring;                       // guarded by `lock`
    kernel::WaitList lock;                // only the lock word: an IRQ-masking spinlock (kernel::detail::lock_list)
    uint16_t index = 0;
    uint32_t core = 0;
    uint8_t vector = 0;
    int32_t io_device = -1;
    uint64_t interrupts = 0;              // atomic
  };
  struct Slot {
    ByteView buffer;                      // kSlotBytes of DMA memory
    ByteView header;                      // the Request the device reads, then
    ByteView status;                      // the status byte it writes
    CommandCompletion completion;
  };

  bool set_up_slots(uint16_t ring_size);
  bool enable_interrupts();
  bool check_interrupts();
  void disable_interrupts();
  bool run(bool write, uint64_t lba, ByteView into, ConstByteView from);
  Queue& queue_for_this_core();
  bool execute(Queue& q, uint32_t slot, uint32_t type, uint64_t sector, uint64_t bytes);
  // The two halves of execute(): post the request, then wait for it.
  bool submit(Queue& q, uint32_t slot, uint32_t type, uint64_t sector, uint64_t bytes);
  bool finish(Queue& q, uint32_t slot);
  void reap(Queue& q);
  static kernel::Frame* on_queue_interrupt(kernel::Frame* frame, void* queue);
  uint64_t read_config64(uint32_t offset) const;
  uint16_t read_config16(uint32_t offset) const;

  VirtioPciDevice transport_;
  Queue queues_[kMaxQueues];
  uint32_t queue_count_ = 0;
  uint8_t core_queue_[64] = {};
  Slot slots_[SlotPool::kMaxSlots];
  uint32_t slot_count_ = 0;
  SlotPool pool_;
  ByteView slot_memory_;                  // every slot's buffer
  ByteView header_memory_;                // every slot's header (16 bytes) and status byte, 32 bytes apart
  bool has_flush_ = false;
  uint32_t max_discard_sectors_ = 0;
  bool started_ = false;
  bool interrupts_ = false;
};

}  // namespace hardware
