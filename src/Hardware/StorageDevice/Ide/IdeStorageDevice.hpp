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
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// A legacy parallel-ATA (IDE) controller, the disk controller of QEMU's i440fx
// "pc" machine (piix3-ide / piix4-ide). Owns the PCI function; the drives on its
// two channels are IdeStorageDevices that share it.
class IdeController {
 public:
  struct Channel {
    uint16_t command_base = 0;  // eight task-file registers
    uint16_t control_base = 0;  // alternate status / device control
  };

  IdeController(const IPciBus& pci, const PciDeviceInfo& device, efi::SystemTable* st)
      : pci_(pci), device_(device), st_(st) {}

  // PCI class 01, subclass 01 (any programming interface).
  static bool supports(const PciDeviceInfo& d) { return d.class_code == 0x01 && d.subclass == 0x01; }

  bool initialize();
  void shutdown();  // safe to repeat

  Channel channel(uint32_t index) const { return channels_[index]; }
  // The master and slave drive of a channel share its registers: one command at a time.
  kernel::Mutex& channel_mutex(uint32_t index) { return channel_mutex_[index]; }
  efi::SystemTable* system_table() const { return st_; }

 private:
  uint16_t io_base(uint8_t bar_index, uint16_t legacy, bool native) const;

  const IPciBus& pci_;
  PciDeviceInfo device_;
  efi::SystemTable* st_;
  Channel channels_[2];
  kernel::Mutex channel_mutex_[2];
  bool owns_device_ = false;
};

// One ATA disk or ATAPI drive on an IDE channel, moved with programmed I/O
// (the data goes through the CPU word by word), polled, interrupts masked.
class IdeStorageDevice final : public StorageDeviceBase {
 public:
  IdeStorageDevice(SharedPointer<IdeController> controller, uint32_t channel, uint32_t drive,
                   allocators::IAllocator& allocator)
      : StorageDeviceBase(allocator, controller->system_table()),
        controller_(util::move(controller)), channel_(controller_->channel(channel)), channel_index_(channel), drive_(drive) {}

  // Probes both channels of a controller and adds the drives that answer.
  static void discover(const SharedPointer<IdeController>& controller, allocators::IAllocator& allocator,
                       List<UniquePointer<IStorageDevice>>& out);

  const char* name() const override { return atapi_ ? "IDE optical drive" : "IDE disk"; }
  bool initialize() override;
  void shutdown() override;

 protected:
  bool transfer(bool write, uint64_t lba, uint32_t blocks, ByteView dma) override;
  bool flush_cache() override;

 private:
  void release_resources() { release_bounce(); }
  uint8_t reg_read(uint32_t reg) const;
  void reg_write(uint32_t reg, uint8_t value) const;
  uint8_t status() const;
  void select(uint8_t lba_high_bits) const;
  bool wait_not_busy(uint32_t timeout_ms);
  bool wait_data_ready();
  bool identify(uint8_t command, ArrayView<uint16_t> data);
  bool atapi_command(ConstByteView cdb, ByteView data);

  SharedPointer<IdeController> controller_;
  IdeController::Channel channel_;
  uint32_t channel_index_;
  uint32_t drive_;
  bool atapi_ = false;
  bool lba48_ = false;
};

}  // namespace hardware
