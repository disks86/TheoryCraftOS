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

// AMD chipsets in two parts. The fusion controller hub (FCH: SMBus, LPC/eSPI, power management, GPIO, the legacy timers) moved into the processor with
// the Zen SoCs; every Ryzen, Threadripper and EPYC has one at PCI 00:14. A desktop board adds a Promontory chipset on PCIe for more USB, SATA and
// PCIe lanes (300 to 800 series); laptops and the X300/A300 boards use the SoC alone.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

// The FCH generations, by their SMBus function (Linux drivers/i2c/busses/i2c-piix4.c).
enum class AmdFch : uint8_t {
  Unknown,
  Sb7x0,    // SB700/SB710/SB750: ATI 0x4385 revision < 0x40
  Sb8x0,    // SB800 series: ATI 0x4385 revision >= 0x40
  Hudson,   // Hudson-2/3, Bolton, the Kabini/Beema/Carrizo SoCs: AMD 0x780B
  Kerncz,   // every Zen SoC: AMD 0x790B
};

// How the SMBus host's I/O base is found.
enum class AmdSmbusDecode : uint8_t {
  None,
  PciConfig,   // SB700: SMBBA at config 0x90, the enable bit in config 0xD2
  PmLegacy,    // SB800 to the first Zen FCHs: power-management registers 0x2C (enable bit 0) and 0x2D, base = word & 0xFFE0
  PmDecodeEn,  // Zen FCH revision >= 0x49: power-management register 0x00 (DecodeEn, bit 4 SmbusAsfIoEn) and 0x01 (base >> 8)
};

struct AmdFchInfo {
  AmdFch fch = AmdFch::Unknown;
  const char* codename = "unknown AMD FCH";
  const char* products = "";
  AmdSmbusDecode decode = AmdSmbusDecode::None;
  // Revision 0x51 on can have the 0xCD6/0xCD7 index ports turned off: the power-management registers must be read through MMIO (0xFED80300).
  bool pm_needs_mmio = false;
};
AmdFchInfo amd_fch_of_smbus(uint16_t vendor_id, uint16_t device_id, uint8_t revision);

// The Promontory chipsets of the AM4 and AM5 desktop boards (PCI vendor 0x1022).
enum class AmdChipset : uint8_t {
  None,
  Promontory300,   // ASMedia-built Promontory: 300 series
  Promontory400,   // the 400 series refresh of the same chip
  Bixby,           // X570: AMD's own chipset (a Matisse I/O die)
  Promontory19,    // 500 series
  Promontory21,    // 600 and 800 series; the X boards carry two
};

struct AmdChipsetInfo {
  AmdChipset chipset = AmdChipset::None;
  const char* codename = "no chipset";
  const char* products = "";
  const char* products_two_chips = nullptr;   // Promontory 21: the boards with a second chip daisy-chained
  ArrayView<const uint16_t> device_ids;       // the PCI device IDs of its functions (bridges, USB, SATA)
  uint16_t upstream_port_id = 0;              // the upstream port of its PCIe switch, one per chip
};
AmdChipsetInfo amd_chipset_of_device(uint16_t device_id);
AmdChipsetInfo amd_chipset_info(AmdChipset chipset);

uint32_t amd_chipset_count();
AmdChipsetInfo amd_chipset_at(uint32_t index);

}  // namespace hardware
