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
#include "ObjectContainer/List.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// An AHCI SATA host bus adapter (QEMU's ich9-ahci, the disk controller of the q35
// machine). Owns the PCI function and the controller-wide registers; the disks
// and optical drives on its ports are AhciStorageDevices that share it.
class AhciController {
 public:
  AhciController(const IPciBus& pci, const PciDeviceInfo& device, efi::SystemTable* st)
      : pci_(pci), device_(device), st_(st) {}

  // PCI class 01, subclass 06, programming interface 01.
  static bool supports(const PciDeviceInfo& d) { return d.class_code == 0x01 && d.subclass == 0x06 && d.prog_if == 0x01; }

  // Takes the controller from the firmware and switches it to AHCI mode.
  bool initialize();
  // Stops the ports and gives the controller back to the firmware. Safe to repeat.
  void shutdown();

  uint32_t ports_implemented() const { return ports_; }
  bool addresses_64_bit() const { return addresses_64_; }
  volatile uint32_t& port_register(uint32_t port, uint32_t offset) const {
    return *reinterpret_cast<volatile uint32_t*>(registers_ + 0x100 + 0x80 * port + offset);
  }
  efi::SystemTable* system_table() const { return st_; }

 private:
  const IPciBus& pci_;
  PciDeviceInfo device_;
  efi::SystemTable* st_;
  uint64_t registers_ = 0;
  uint32_t ports_ = 0;
  bool addresses_64_ = false;
  bool owns_device_ = false;
};

// One ATA disk or ATAPI drive behind an AHCI port, driven with DMA commands
// through command slot 0 (one command at a time, polled).
class AhciStorageDevice final : public StorageDeviceBase {
 public:
  AhciStorageDevice(SharedPointer<AhciController> controller, uint32_t port, allocators::IAllocator& allocator)
      : StorageDeviceBase(allocator, controller->system_table()), controller_(util::move(controller)), port_(port) {}

  // Finds the drives behind a controller's ports and adds the ones that work.
  static void discover(const SharedPointer<AhciController>& controller, allocators::IAllocator& allocator,
                       List<UniquePointer<IStorageDevice>>& out);

  const char* name() const override { return atapi_ ? "SATA optical drive" : "SATA disk"; }
  bool initialize() override;
  void shutdown() override;

 protected:
  bool transfer(bool write, uint64_t lba, uint32_t blocks, void* dma) override;
  bool flush_cache() override;

 private:
  void release_resources();  // this drive only; the controller stays
  bool start_port();
  bool stop_port();
  bool issue(const uint8_t* fis, uint32_t fis_dwords, const uint8_t* packet, bool write, void* data, uint32_t bytes);
  bool identify_ata();
  bool identify_atapi();
  bool atapi_command(const uint8_t* cdb, void* data, uint32_t bytes);

  SharedPointer<AhciController> controller_;
  uint32_t port_;
  uint8_t* memory_ = nullptr;  // command list, received-FIS area and command table in one page
  bool atapi_ = false;
  bool lba48_ = true;
  bool started_ = false;
};

}  // namespace hardware
