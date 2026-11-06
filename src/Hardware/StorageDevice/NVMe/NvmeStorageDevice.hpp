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
#include "Hardware/Bus/Pci/PciMsix.hpp"
#include "Hardware/StorageDevice/QueuedStorage.hpp"
#include "Hardware/StorageDevice/StorageDeviceBase.hpp"
#include "Kernel/Interrupts.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// An NVM Express SSD: the first active namespace of the controller, used the way NVMe is meant to be on a machine with many cores.
//
// - One I/O queue pair per core (up to kMaxQueues and what the controller grants): a thread submits on its own core's queue, under that queue's
//   lock only, so cores never contend for a doorbell.
// - Real depth: a pool of kMaxSlots command slots, each with its own DMA buffer and PRP list, so that many requests are on the device at once
//   (StorageInfo::queue_depth tells the I/O scheduler how many). A request holds one slot; a big one goes through it 64 KiB at a time.
// - Interrupts: on x86 each queue's MSI-X entry goes to the core that submits on it, with a vector of its own (kVectorStorageFirst...); the
//   completion handler there wakes the waiting thread, which the scheduler then runs where the data just arrived. Interrupt delivery is
//   checked on every queue at start; without MSI-X, before the scheduler runs, or if a check fails, the waiting threads poll their queue
//   instead (still in parallel). A waiter also looks at its queue itself whenever its wait times out, so a lost interrupt costs time, not the
//   request.
// - The admin queue is used one command at a time, polled, during start-up only.
class NvmeStorageDevice final : public StorageDeviceBase {
 public:
  static constexpr uint32_t kAdminEntries = 16;
  static constexpr uint32_t kMaxQueues = 16;
  static constexpr uint32_t kMaxQueueEntries = 128;
  static constexpr uint32_t kMaxSlots = SlotPool::kMaxSlots;
  static constexpr uint32_t kSlotBytes = 64 * 1024;

  NvmeStorageDevice(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                    efi::SystemTable* st)
      : StorageDeviceBase(allocator, st), pci_(pci), device_(device) {}

  // PCI class 01, subclass 08, programming interface 02.
  static bool supports(const PciDeviceInfo& d) { return d.class_code == 0x01 && d.subclass == 0x08 && d.prog_if == 0x02; }

  const char* name() const override { return "NVMe"; }
  bool initialize() override;
  void shutdown() override;
  void describe(IPropertySink& sink) const override;

  bool read(uint64_t lba, ByteView buffer) override;
  bool write(uint64_t lba, ConstByteView buffer) override;
  bool flush() override;
  StorageStatistics statistics() const override;

  uint32_t queue_count() const { return queue_count_; }
  uint32_t slot_count() const { return slot_count_; }
  bool uses_interrupts() const { return interrupts_; }
  // Completion interrupts taken on the queue (0 when polled).
  uint64_t queue_interrupts(uint32_t queue) const;

 protected:
  bool transfer(bool, uint64_t, uint32_t, ByteView) override { return false; }   // (read/write above do the work)

 private:
  struct Command {  // submission queue entry, 64 bytes
    uint32_t cdw0;  // opcode, command id
    uint32_t nsid;
    uint64_t reserved;
    uint64_t metadata;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
  };
  struct Completion {  // completion queue entry, 16 bytes
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head, sq_id;
    uint16_t command_id;
    uint16_t status;  // bit 0 phase tag, bits 15:1 status
  };
  struct AdminQueue {
    ArrayView<Command> submission;    // kAdminEntries entries (the rest of the page is unused)
    ArrayView<Completion> completion;
    uint32_t tail = 0, head = 0;
    uint16_t phase = 1;
  };
  struct IoQueue {
    NvmeStorageDevice* owner = nullptr;   // (for the interrupt handler)
    ArrayView<Command> submission;
    ArrayView<Completion> completion;
    uint32_t entries = 0;
    uint32_t tail = 0, head = 0;          // guarded by `lock`
    uint16_t phase = 1;
    uint16_t id = 0;                      // the queue id (1..), also its MSI-X entry
    uint32_t core = 0;                    // the core its interrupt goes to
    kernel::WaitList lock;                // only the lock word: an IRQ-masking spinlock (kernel::detail::lock_list)
    int32_t io_device = -1;
    uint8_t vector = 0;                   // 0: none
    uint64_t interrupts = 0;              // atomic
  };
  struct Slot {
    ByteView buffer;                      // kSlotBytes of DMA memory
    ArrayView<uint64_t> prp;              // the PRP list for the pages after the first
    CommandCompletion completion;
  };

  uint32_t read32(uint32_t reg) const;
  void write32(uint32_t reg, uint32_t value) const;
  uint64_t read64(uint32_t reg) const;
  void write64(uint32_t reg, uint64_t value) const;
  bool wait_ready(bool ready);
  bool disable_controller();
  bool admin(Command& command, uint32_t* result = nullptr);
  bool identify();
  bool set_up_slots();
  bool set_up_queues();
  bool create_queue(IoQueue& q, bool interrupt);
  bool enable_interrupts();
  bool check_interrupts();
  void disable_interrupts();

  bool run(bool write, uint64_t lba, ByteView into, ConstByteView from);
  IoQueue& queue_for_this_core();
  // Runs one command through `slot` on `q` and waits for it. False on an error status or a timeout.
  bool execute(IoQueue& q, uint32_t slot, uint8_t opcode, uint64_t lba, uint32_t blocks, uint64_t bytes);
  void reap(IoQueue& q);
  static kernel::Frame* on_queue_interrupt(kernel::Frame* frame, void* queue);

  const IPciBus& pci_;
  PciDeviceInfo device_;
  uint64_t registers_ = 0;
  uint32_t doorbell_stride_ = 4;
  uint32_t timeout_ms_ = 5000;
  uint32_t max_queue_entries_ = 2;        // CAP.MQES + 1
  uint32_t chunk_bytes_ = kSlotBytes;     // what one command moves at most (MDTS)
  AdminQueue admin_;
  ByteView scratch_;                      // identify data (4 KiB) followed by an admin PRP list (4 KiB)
  kernel::Mutex admin_mutex_;
  uint32_t namespace_id_ = 1;
  uint16_t next_admin_id_ = 1;
  IoQueue queues_[kMaxQueues];
  uint32_t queue_count_ = 0;
  uint8_t core_queue_[64] = {};           // the queue each of the first 64 cores submits on (others: core % queue_count_)
  Slot slots_[kMaxSlots];
  uint32_t slot_count_ = 0;
  SlotPool pool_;
  ByteView slot_memory_;                  // every slot's buffer, then
  ByteView prp_memory_;                   // every slot's PRP list
  // The MSI-X table lies in one of the function's BARs, which (like the registers) are reachable at their physical address.
  struct BarMapper final : IRegisterMapper {
    bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
      if (physical == 0 || length == 0 || length > UINT64_MAX - physical) return false;
      *region = MmioRegion(physical, length);
      return true;
    }
  } bar_mapper_;
  UniquePointer<PciMsix> msix_;
  bool interrupts_ = false;
  bool owns_device_ = false;
  bool enabled_ = false;
};

}  // namespace hardware
