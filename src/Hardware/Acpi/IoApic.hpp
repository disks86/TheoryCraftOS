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

// The I/O APIC route of the ACPI system control interrupt (SCI). The MADT lists the I/O APICs (type 1: id, address, first global system interrupt) and the interrupt source overrides (type 2: the
// ISA interrupt the FADT names for the SCI may be wired to another GSI, with its own polarity and trigger). The redirection entry is then written: the vector, fixed delivery, physical
// destination, the polarity and trigger the MADT gave (the SCI is level triggered and active low on every PC chipset that has an override at all).
#pragma once
#include <stdint.h>
#include "Hardware/Mmio.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

struct IoApicInfo {
  uint8_t id = 0;
  uint32_t address = 0;
  uint32_t gsi_base = 0;
};

struct SciRoute {
  uint32_t gsi = 0;
  bool level = false, active_low = false;
};

// All I/O APICs of the MADT, up to the room in `out`; returns how many were there (the ones that do not fit are not stored but counted).
uint32_t madt_ioapics(ConstByteView madt, ArrayView<IoApicInfo> out);
// Where the SCI the FADT names (`sci_irq`, an ISA interrupt number) ends up: its override if the MADT has one, else the same number as an edge triggered, active high ISA interrupt.
SciRoute madt_sci_route(ConstByteView madt, uint32_t sci_irq);

// The two registers of an I/O APIC window: the index register at +0 and the data register at +0x10.
class IIoApicWindow {
 public:
  virtual uint32_t read(uint8_t index) = 0;
  virtual void write(uint8_t index, uint32_t value) = 0;

 protected:
  IIoApicWindow() = default;
  ~IIoApicWindow() = default;
};

class MmioIoApicWindow final : public IIoApicWindow {
 public:
  explicit MmioIoApicWindow(const MmioRegion& region) : region_(region) {}
  uint32_t read(uint8_t index) override { region_.write32(0, index); return region_.read32(0x10); }
  void write(uint8_t index, uint32_t value) override { region_.write32(0, index); region_.write32(0x10, value); }

 private:
  MmioRegion region_;
};

class IoApic {
 public:
  static constexpr uint8_t kVersion = 0x01, kRedirectionBase = 0x10;
  static constexpr uint32_t kMasked = 1u << 16, kLevel = 1u << 15, kActiveLow = 1u << 13;

  IoApic(IIoApicWindow& window, uint32_t gsi_base) : window_(window), gsi_base_(gsi_base) {}
  // How many interrupt inputs it has (the version register's maximum redirection entry plus one).
  uint32_t inputs();
  bool contains(uint32_t gsi) { return gsi >= gsi_base_ && gsi - gsi_base_ < inputs(); }
  // Writes the entry of `gsi` masked, then the destination, then the rest with the mask as asked: the interrupt goes as `vector` to the local APIC `apic_id` (physical mode, fixed).
  bool route(uint32_t gsi, uint8_t vector, uint32_t apic_id, bool level, bool active_low, bool masked);
  // Masks or unmasks the input. The caller keeps interrupts off around it (the index and data registers are a pair).
  bool mask(uint32_t gsi, bool masked);
  bool masked(uint32_t gsi);

 private:
  IIoApicWindow& window_;
  uint32_t gsi_base_;
};

}  // namespace hardware
