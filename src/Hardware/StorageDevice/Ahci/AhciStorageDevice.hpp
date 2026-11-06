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
#include "Hardware/Bus/Pci/PciMsi.hpp"
#include "Hardware/StorageDevice/QueuedStorage.hpp"
#include "Hardware/StorageDevice/StorageDeviceBase.hpp"
#include "Kernel/Interrupts.hpp"
#include "Utility/AllocatedView.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

class AhciStorageDevice;

// An AHCI SATA host bus adapter (QEMU's ich9-ahci, the disk controller of the q35
// machine). Owns the PCI function and the controller-wide registers; the disks
// and optical drives on its ports are AhciStorageDevices that share it.
class AhciController {
 public:
  AhciController(const IPciBus& pci, const PciDeviceInfo& device, efi::SystemTable* st)
      : pci_(pci), device_(device), st_(st), msi_(pci) {}

  // PCI class 01, subclass 06, programming interface 01.
  static bool supports(const PciDeviceInfo& d) { return d.class_code == 0x01 && d.subclass == 0x06 && d.prog_if == 0x01; }

  // Takes the controller from the firmware and switches it to AHCI mode.
  bool initialize();
  // Stops the ports and gives the controller back to the firmware. Safe to repeat.
  void shutdown();
  void stop_interrupts() { disable_interrupts(); }   // (shutdown, before the kernel stops: the HBA raises no more interrupts)

  uint32_t ports_implemented() const { return ports_; }
  bool addresses_64_bit() const { return addresses_64_; }
  // Native Command Queuing: whether the HBA does it (CAP.SNCQ), and how many command slots each port has (CAP.NCS + 1).
  bool supports_ncq() const { return ncq_; }
  uint32_t command_slots() const { return command_slots_; }

  // The HBA's one MSI, to a core the scheduler picks: the handler goes through the ports that asked and lets each drive take its completions.
  // Done once, by the first drive that wants it; true while it is on. `device` is called for the interrupts of `port` from then on.
  bool enable_interrupts();
  void attach_port(uint32_t port, AhciStorageDevice* device);
  void detach_port(uint32_t port);
  uint64_t interrupts() const { return __atomic_load_n(&interrupt_count_, __ATOMIC_ACQUIRE); }
  uint32_t interrupt_core() const { return interrupt_core_; }
  volatile uint32_t& port_register(uint32_t port, uint32_t offset) const {
    return *reinterpret_cast<volatile uint32_t*>(registers_ + 0x100 + 0x80 * port + offset);
  }
  efi::SystemTable* system_table() const { return st_; }

 private:
  const IPciBus& pci_;
  PciDeviceInfo device_;
  efi::SystemTable* st_;
  void disable_interrupts();
  static kernel::Frame* on_interrupt(kernel::Frame* frame, void* controller);

  uint64_t registers_ = 0;
  uint32_t ports_ = 0;
  uint32_t command_slots_ = 1;
  bool addresses_64_ = false;
  bool ncq_ = false;
  bool owns_device_ = false;
  PciMsi msi_;
  uint8_t vector_ = 0;
  int32_t io_device_ = -1;
  uint32_t interrupt_core_ = 0;
  bool interrupts_ = false;
  uint64_t interrupt_count_ = 0;          // atomic
  AhciStorageDevice* port_devices_[32] = {};   // read by the interrupt handler (atomic loads)
};

// One ATA disk or ATAPI drive behind an AHCI port. A disk that does Native Command Queuing (and an HBA that does) keeps up to 32 commands
// on the drive at once: each tag has its own command table and 64 KiB DMA buffer, commands are READ/WRITE FPDMA QUEUED, and the completions
// come as Set Device Bits FISes (PxSACT bits clearing), taken by the HBA's interrupt or, without one, by polling. StorageInfo::queue_depth
// is the tag count. Commands that cannot be queued (flush, IDENTIFY, ATAPI packets) wait until the queue is empty and keep it empty while
// they run (slot 0, polled). A task-file error during NCQ fails every outstanding command and restarts the port. Optical drives and disks
// without NCQ use slot 0 for everything, one command at a time.
class AhciStorageDevice final : public StorageDeviceBase {
 public:
  static constexpr uint32_t kMaxTags = 32;
  static constexpr uint32_t kSlotBytes = 64 * 1024;

  AhciStorageDevice(SharedPointer<AhciController> controller, uint32_t port, allocators::IAllocator& allocator)
      : StorageDeviceBase(allocator, controller->system_table()), controller_(util::move(controller)), port_(port) {}

  // Finds the drives behind a controller's ports and adds the ones that work.
  static void discover(const SharedPointer<AhciController>& controller, allocators::IAllocator& allocator,
                       List<UniquePointer<IStorageDevice>>& out);

  const char* name() const override { return atapi_ ? "SATA optical drive" : "SATA disk"; }
  bool initialize() override;
  void shutdown() override;
  void describe(IPropertySink& sink) const override;

  bool read(uint64_t lba, ByteView buffer) override;
  bool write(uint64_t lba, ConstByteView buffer) override;
  bool flush() override;
  void stop_interrupts() override;
  bool discard(uint64_t lba, uint64_t blocks) override;
  StorageStatistics statistics() const override;

  // The HBA's interrupt for this port (AhciController::on_interrupt).
  void on_port_interrupt();

 protected:
  bool transfer(bool write, uint64_t lba, uint32_t blocks, ByteView dma) override;
  bool flush_cache() override;

 private:
  void release_resources();  // this drive only; the controller stays
  bool start_port();
  bool stop_port();
  bool issue(ConstByteView fis, ConstByteView packet, bool write, ByteView data);
  bool identify_ata();
  bool identify_atapi();
  bool atapi_command(ConstByteView cdb, ByteView data);

  // --- Native Command Queuing.
  struct Tag {
    ByteView buffer;                       // kSlotBytes of DMA memory
    CommandCompletion completion;
  };
  bool set_up_ncq(const ArrayView<uint16_t>& identify_words);
  bool check_interrupts();
  bool run_ncq(bool write, uint64_t lba, ByteView into, ConstByteView from);
  // Queues one FPDMA command on `tag` and waits for it. False on an error; *retry when the port was being restarted (nothing was sent).
  bool execute_ncq(uint32_t tag, bool write, uint64_t lba, uint32_t blocks, bool* retry);
  // Its halves: send (false, with nothing sent, while the port needs a restart), then wait (false on an error or a timeout).
  bool submit_ncq(uint32_t tag, bool write, uint64_t lba, uint32_t blocks);
  bool finish_ncq(uint32_t tag);
  void reap();
  bool restart_after_error();
  // The gate between queued commands (shared) and the others (exclusive: they wait until no queued command is on the drive).
  void enter_queued();
  void leave_queued();
  void enter_exclusive();
  void leave_exclusive();

  SharedPointer<AhciController> controller_;
  uint32_t port_;
  ByteView memory_;  // command list (1 KiB), received FIS (256 B), IDENTIFY data, then a 256-byte command table per tag from 4 KiB on
  bool atapi_ = false;
  bool lba48_ = true;
  bool started_ = false;
  bool ncq_ = false;
  bool interrupts_ = false;
  Tag tags_[kMaxTags];
  uint32_t tag_count_ = 0;
  SlotPool pool_;
  ByteView tag_memory_;
  kernel::WaitList lock_;                  // only the lock word: guards issued_, the port's SACT/CI writes and reaping
  uint32_t issued_ = 0;                    // tags on the drive and not yet reaped
  bool needs_restart_ = false;             // a task-file error stopped the queue
  uint32_t gate_ = 0;                      // atomic: bit 31 an exclusive command waits or runs, the rest queued commands in flight
  uint64_t errors_recovered_ = 0;
  // From the NCQ Command Error log (log 10h) read after the last error: the tag and LBA the drive reports failed (kNoTag: none).
  static constexpr uint32_t kNoTag = 0xFF;
  uint32_t last_error_tag_ = kNoTag;
  uint64_t last_error_lba_ = 0;
  bool read_ncq_error_log();
  bool general_purpose_logging_ = false;   // READ LOG EXT works (IDENTIFY word 84/87 bit 5)
};

}  // namespace hardware
