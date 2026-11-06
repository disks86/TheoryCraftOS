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

#include "Hardware/Chipset/AMD/AmdChipsets.hpp"

namespace hardware {

namespace {
constexpr uint16_t kAti = 0x1002, kAmd = 0x1022, kHygon = 0x1D94;

// Device IDs: Linux (xhci-pci.c PROMONTORYA_1..4, ahci.c) and the PCI ID repository. The X570's own USB (0x149C) and SATA are the same functions
// as the Matisse CPU's, so only its switch ports identify it.
constexpr uint16_t kPromontory300[] = {0x43B4, 0x43B7, 0x43B9, 0x43BA, 0x43BB, 0x43BC};
constexpr uint16_t kPromontory400[] = {0x43C6, 0x43C7, 0x43C8, 0x43D0, 0x43D5};
constexpr uint16_t kBixby[] = {0x57AD, 0x57A3, 0x57A4};
constexpr uint16_t kPromontory19[] = {0x43E9, 0x43EA, 0x43EB, 0x43EE};
constexpr uint16_t kPromontory21[] = {0x43F4, 0x43F5, 0x43F6, 0x43F7};

constexpr AmdChipsetInfo kChipsets[] = {
    {AmdChipset::Promontory300, "Promontory", "300 series (X370, B350, A320)", nullptr, kPromontory300, 0},
    {AmdChipset::Promontory400, "Promontory (400 series)", "400 series (X470, B450)", nullptr, kPromontory400, 0},
    {AmdChipset::Bixby, "Bixby", "X570", nullptr, kBixby, 0x57AD},
    {AmdChipset::Promontory19, "Promontory 19", "500 series (B550, A520), B840", nullptr, kPromontory19, 0x43E9},
    {AmdChipset::Promontory21, "Promontory 21", "A620, B650, B650E, B850, X870", "X670, X670E, X870E (two chips)", kPromontory21, 0x43F4},
};
}  // namespace

AmdFchInfo amd_fch_of_smbus(uint16_t vendor_id, uint16_t device_id, uint8_t revision) {
  AmdFchInfo info;
  if (vendor_id == kAti && device_id == 0x4385) {
    if (revision < 0x40) {
      info = {AmdFch::Sb7x0, "SB700", "SB700/SB710/SB750 (AMD 7xx chipsets)", AmdSmbusDecode::PciConfig, false};
    } else {
      info = {AmdFch::Sb8x0, "SB800", "SB810/SB850 (AMD 8xx chipsets)", AmdSmbusDecode::PmLegacy, false};
    }
  } else if (vendor_id == kAmd && device_id == 0x780B) {
    info = {AmdFch::Hudson, "Hudson", "Hudson/Bolton FCH (A55..A88X), Kabini/Beema/Carrizo SoCs", AmdSmbusDecode::PmLegacy, false};
  } else if ((vendor_id == kAmd || vendor_id == kHygon) && device_id == 0x790B) {
    info = {AmdFch::Kerncz, "FCH (KERNCZ)", "the FCH of the Zen SoCs (Ryzen, Threadripper, EPYC)",
            revision >= 0x49 ? AmdSmbusDecode::PmDecodeEn : AmdSmbusDecode::PmLegacy, revision >= 0x51};
  }
  return info;
}

AmdChipsetInfo amd_chipset_of_device(uint16_t device_id) {
  for (const AmdChipsetInfo& chipset : kChipsets)
    for (uint16_t id : chipset.device_ids)
      if (id == device_id) return chipset;
  return AmdChipsetInfo();
}

AmdChipsetInfo amd_chipset_info(AmdChipset kind) {
  for (const AmdChipsetInfo& chipset : kChipsets)
    if (chipset.chipset == kind) return chipset;
  return AmdChipsetInfo();
}

uint32_t amd_chipset_count() { return sizeof(kChipsets) / sizeof(kChipsets[0]); }

AmdChipsetInfo amd_chipset_at(uint32_t index) {
  if (index >= amd_chipset_count()) return AmdChipsetInfo();
  return kChipsets[index];
}

}  // namespace hardware
