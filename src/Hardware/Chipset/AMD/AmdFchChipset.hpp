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

// An AMD chipset as the OS finds it: the FCH at 00:14 (its generation, where the firmware put the SMBus host and which of its ports is selected),
// the Promontory chip(s) on the board, and the Data Fabric nodes (one per CPU die with its own I/O, devices 00:18..00:1F). Reads only: configuration
// space and three FCH power-management registers.
#pragma once
#include <stdint.h>
#include "Hardware/Chipset/AMD/AmdChipsets.hpp"
#include "Hardware/Chipset/IChipset.hpp"

namespace hardware {

// The FCH's power-management register block (PMIO): through the index/data ports 0xCD6/0xCD7, or MMIO at 0xFED80300 on the newer FCHs.
class IAmdPmRegisters {
 public:
  virtual uint8_t read8(uint8_t offset) = 0;

 protected:
  constexpr IAmdPmRegisters() = default;
  ~IAmdPmRegisters() = default;
};

class AmdFchChipset final : public IChipset {
 public:
  const char* name() const override { return "AMD chipset"; }
  bool initialize() override { return found(); }
  void shutdown() override {}

  // `pm_ports` and `pm_mmio` are the two ways to the power-management registers; either may be null (the MMIO one when nothing can map it).
  void probe(const IPciBus& pci, IAmdPmRegisters* pm_ports, IAmdPmRegisters* pm_mmio);
  bool found() const { return fch_.fch != AmdFch::Unknown || chipset_.chipset != AmdChipset::None; }

  ChipsetVendor vendor() const override { return ChipsetVendor::Amd; }
  // The board's chipset when there is one (Promontory 21), else the FCH.
  const char* codename() const override { return chipset_.chipset != AmdChipset::None ? chipset_.codename : fch_.codename; }
  const char* products() const override;
  const ChipsetInventory& inventory() const override { return inventory_; }
  ChipsetSmbus smbus() const override { return smbus_; }
  void summarize(FixedString<256>& out) const override;

  const AmdFchInfo& fch() const { return fch_; }
  uint8_t fch_revision() const { return fch_revision_; }
  const AmdChipsetInfo& board_chipset() const { return chipset_; }
  uint8_t chipset_chips() const { return chips_; }
  uint8_t data_fabric_nodes() const { return df_nodes_; }
  // The SMBus port the FCH routes the host to (KERNCZ: PM 0x02 bits 4:3); 0xFF when not known. SPD EEPROMs are on port 0.
  uint8_t smbus_port() const { return smbus_port_; }
  bool pm_through_mmio() const { return pm_mmio_used_; }

 private:
  void find_smbus(const IPciBus& pci, PciAddress address, IAmdPmRegisters* pm);

  AmdFchInfo fch_;
  uint8_t fch_revision_ = 0;
  AmdChipsetInfo chipset_;
  uint8_t chips_ = 0;
  uint8_t df_nodes_ = 0;
  uint8_t smbus_port_ = 0xFF;
  bool pm_mmio_used_ = false;
  ChipsetInventory inventory_;
  ChipsetSmbus smbus_;
};

}  // namespace hardware
