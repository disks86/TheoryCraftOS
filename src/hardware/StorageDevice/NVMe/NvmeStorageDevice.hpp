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
#include "hardware/Bus/Pci/IPciBus.hpp"
#include "hardware/StorageDevice/StorageDeviceBase.hpp"

namespace hardware {

// An NVM Express SSD (QEMU's `nvme` device): the first active namespace of the
// controller, through one admin and one I/O queue pair that are used one command
// at a time, polled, interrupts masked. Data moves by PRP, so the bounce buffer
// plus one PRP list page is all the memory it needs.
class NvmeStorageDevice final : public StorageDeviceBase {
 public:
  static constexpr uint32_t kQueueEntries = 16;

  NvmeStorageDevice(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                    efi::SystemTable* st)
      : StorageDeviceBase(allocator, st), pci_(pci), device_(device) {}

  // PCI class 01, subclass 08, programming interface 02.
  static bool supports(const PciDeviceInfo& d) { return d.class_code == 0x01 && d.subclass == 0x08 && d.prog_if == 0x02; }

  const char* name() const override { return "NVMe"; }
  bool initialize() override;
  void shutdown() override;

 protected:
  bool transfer(bool write, uint64_t lba, uint32_t blocks, void* dma) override;
  bool flush_cache() override;

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
  struct Queue {
    Command* submission = nullptr;
    Completion* completion = nullptr;
    uint32_t tail = 0, head = 0;
    uint16_t phase = 1;
    uint16_t id = 0;
  };

  uint32_t read32(uint32_t reg) const;
  void write32(uint32_t reg, uint32_t value) const;
  uint64_t read64(uint32_t reg) const;
  void write64(uint32_t reg, uint64_t value) const;
  bool wait_ready(bool ready);
  bool disable_controller();
  bool setup_queue(Queue* q, uint16_t id);
  bool submit(Queue& q, Command& command, uint32_t* result = nullptr);
  bool io(uint8_t opcode, uint64_t lba, uint32_t blocks, void* dma);
  bool identify();

  const IPciBus& pci_;
  PciDeviceInfo device_;
  uint64_t registers_ = 0;
  uint32_t doorbell_stride_ = 4;
  uint32_t timeout_ms_ = 5000;
  Queue admin_, io_;
  uint8_t* scratch_ = nullptr;  // identify data (4 KiB) followed by the PRP list (4 KiB)
  uint32_t namespace_id_ = 1;
  uint16_t next_command_id_ = 1;
  bool owns_device_ = false;
  bool enabled_ = false;
};

}  // namespace hardware
