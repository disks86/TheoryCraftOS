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

// An Intel chipset as the OS finds it on PCI bus 0 (devices 0x10..0x1F, where every PCH and SoC puts its functions): which hub it is, where its
// SMBus host is, whether the firmware locked the BIOS flash, and what the management engine reports. Reads configuration space only.
#pragma once
#include <stdint.h>
#include "Hardware/Chipset/IChipset.hpp"
#include "Hardware/Chipset/Intel/IntelPch.hpp"

namespace hardware {

// The BIOS flash protection bits (BIOS_CNTL / BIOS_SPI_BC at config 0xDC of the LPC bridge or, since Sunrise Point, the SPI function). With BLE
// set, turning on BIOSWE raises an SMI; with SMM_BWP set, only SMM code can write the flash at all.
struct IntelBiosProtection {
  bool known = false;
  bool write_enable = false;   // BIOSWE: the flash is writable now
  bool lock_enable = false;    // BLE
  bool smm_protect = false;    // SMM_BWP
  bool locked() const { return known && !write_enable && lock_enable; }
};

// The management engine's first firmware status register (HFSTS1 at config 0x40 of HECI 00:16.0), field layout as coreboot's me.h.
struct IntelMeStatus {
  bool present = false;
  uint32_t hfsts1 = 0;
  uint8_t working_state() const { return hfsts1 & 0xF; }
  bool manufacturing_mode() const { return (hfsts1 >> 4) & 1; }
  bool init_complete() const { return (hfsts1 >> 9) & 1; }
  uint8_t error_code() const { return (hfsts1 >> 12) & 0xF; }
  uint8_t operation_mode() const { return (hfsts1 >> 16) & 0xF; }
};
// Where the TCO watchdog's registers are: TCOBASE (0x50) of the SMBus function when TCOCTL (0x54) bit 8 says the firmware enabled the decode.
struct IntelTcoLocation {
  bool usable = false;
  uint16_t io_base = 0;
  IntelTcoKind kind = IntelTcoKind::None;
  const char* reason = "no TCO watchdog the OS can reach on this hub";
};

const char* intel_me_working_state_name(uint8_t state);
const char* intel_me_operation_mode_name(uint8_t mode);
IntelBiosProtection intel_bios_protection_decode(uint8_t bios_control);

class IntelPchChipset final : public IChipset {
 public:
  const char* name() const override { return "Intel chipset"; }
  bool initialize() override { return found(); }
  void shutdown() override {}

  // Looks at bus 0. `cpu` names the processor, used when the SMBus function is hidden or unknown and to check the pairing.
  void probe(const IPciBus& pci, const ChipsetCpu& cpu);
  bool found() const { return info_.pch != IntelPch::Unknown || inventory_.count() != 0; }

  ChipsetVendor vendor() const override { return ChipsetVendor::Intel; }
  const char* codename() const override { return info_.codename; }
  const char* products() const override { return info_.products; }
  const ChipsetInventory& inventory() const override { return inventory_; }
  ChipsetSmbus smbus() const override { return smbus_; }
  void summarize(FixedString<256>& out) const override;

  const IntelPchInfo& info() const { return info_; }
  // True when the hub was named from the CPU because its SMBus function was missing or not in the catalogue.
  bool identified_by_cpu() const { return by_cpu_; }
  // True when the SMBus function names a hub this CPU is not known to pair with (a virtual machine's emulated ICH9, an untested board).
  bool unusual_pairing() const { return unusual_pairing_; }
  const IntelPchCandidates& expected() const { return expected_; }
  const IntelBiosProtection& bios_protection() const { return bios_; }
  const IntelMeStatus& me() const { return me_; }
  const IntelTcoLocation& tco() const { return tco_; }
  // The PCH thermal sensor function (00:12.0 on the hubs that have one); false when there is none.
  bool thermal_function(PciAddress* out) const;

 private:
  IntelPchInfo info_;
  IntelPchCandidates expected_;
  bool by_cpu_ = false, unusual_pairing_ = false;
  ChipsetInventory inventory_;
  ChipsetSmbus smbus_;
  IntelBiosProtection bios_;
  IntelMeStatus me_;
  IntelTcoLocation tco_;
};

}  // namespace hardware
