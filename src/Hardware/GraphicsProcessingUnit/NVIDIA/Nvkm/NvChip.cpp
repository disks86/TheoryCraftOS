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

// Chip identification as nouveau's nvkm/engine/device/base.c (nvkm_device_ctor) does it, with the chip list of its nvXX_chipset tables, and
// BOOT_42 for Turing and later as NVIDIA's open-gpu-kernel-modules and the nova driver read it.
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"

namespace hardware {
namespace nv {

namespace {

enum ChipFlags : uint8_t {
  kNone = 0,
  kIgp = 1 << 0,        // chipset graphics
  kTegra = 1 << 1,      // a Tegra GPU (implies kIgp and kNoDisplay)
  kNoDisplay = 1 << 2,  // nouveau's chipset table has no display engine for it
};

struct KnownChip {
  uint16_t chipset;
  const char* name;
  uint8_t flags;
};

// Every chipset nouveau's device/base.c has a table for (nv04_chipset .. nv197_chipset), and the Hopper/Blackwell ones it runs through GSP-RM.
constexpr KnownChip kChips[] = {
    // NV04 family (Riva TNT, TNT2)
    {0x004, "NV04", kNone}, {0x005, "NV05", kNone},
    // Celsius (GeForce 256, GeForce 2/4 MX) and the nForce IGPs
    {0x010, "NV10", kNone}, {0x011, "NV11", kNone}, {0x015, "NV15", kNone}, {0x017, "NV17", kNone}, {0x018, "NV18", kNone},
    {0x01a, "nForce", kIgp}, {0x01f, "nForce2", kIgp},
    // Kelvin (GeForce 3/4 Ti, the Xbox's NV2A)
    {0x020, "NV20", kNone}, {0x025, "NV25", kNone}, {0x028, "NV28", kNone}, {0x02a, "NV2A", kNone},
    // Rankine (GeForce FX)
    {0x030, "NV30", kNone}, {0x031, "NV31", kNone}, {0x034, "NV34", kNone}, {0x035, "NV35", kNone}, {0x036, "NV36", kNone},
    // Curie (GeForce 6/7) and its IGPs
    {0x040, "NV40", kNone}, {0x041, "NV41", kNone}, {0x042, "NV42", kNone}, {0x043, "NV43", kNone}, {0x044, "NV44", kNone},
    {0x045, "NV45", kNone}, {0x046, "G72", kNone}, {0x047, "G70", kNone}, {0x049, "G71", kNone}, {0x04a, "NV44A", kNone},
    {0x04b, "G73", kNone}, {0x04c, "C61", kIgp}, {0x04e, "C51", kIgp}, {0x063, "C73", kIgp}, {0x067, "C67", kIgp}, {0x068, "C68", kIgp},
    // Tesla
    {0x050, "G80", kNone}, {0x084, "G84", kNone}, {0x086, "G86", kNone}, {0x092, "G92", kNone}, {0x094, "G94", kNone}, {0x096, "G96", kNone},
    {0x098, "G98", kNone}, {0x0a0, "GT200", kNone}, {0x0a3, "GT215", kNone}, {0x0a5, "GT216", kNone}, {0x0a8, "GT218", kNone},
    {0x0aa, "MCP77/MCP78", kIgp}, {0x0ac, "MCP79/MCP7A", kIgp}, {0x0af, "MCP89", kIgp},
    // Fermi
    {0x0c0, "GF100", kNone}, {0x0c1, "GF108", kNone}, {0x0c3, "GF106", kNone}, {0x0c4, "GF104", kNone}, {0x0c8, "GF110", kNone},
    {0x0ce, "GF114", kNone}, {0x0cf, "GF116", kNone}, {0x0d7, "GF117", kNone}, {0x0d9, "GF119", kNone},
    // Kepler
    {0x0e4, "GK104", kNone}, {0x0e6, "GK106", kNone}, {0x0e7, "GK107", kNone}, {0x0ea, "GK20A", kTegra}, {0x0f0, "GK110", kNone},
    {0x0f1, "GK110B", kNone}, {0x106, "GK208B", kNone}, {0x108, "GK208", kNone},
    // Maxwell
    {0x117, "GM107", kNone}, {0x118, "GM108", kNone}, {0x120, "GM200", kNone}, {0x124, "GM204", kNone}, {0x126, "GM206", kNone},
    {0x12b, "GM20B", kTegra},
    // Pascal
    {0x130, "GP100", kNone}, {0x132, "GP102", kNone}, {0x134, "GP104", kNone}, {0x136, "GP106", kNone}, {0x137, "GP107", kNone},
    {0x138, "GP108", kNone}, {0x13b, "GP10B", kTegra},
    // Volta
    {0x140, "GV100", kNone},
    // Turing
    {0x162, "TU102", kNone}, {0x164, "TU104", kNone}, {0x166, "TU106", kNone}, {0x167, "TU117", kNone}, {0x168, "TU116", kNone},
    // Ampere (GA100 is compute only)
    {0x170, "GA100", kNoDisplay}, {0x172, "GA102", kNone}, {0x173, "GA103", kNone}, {0x174, "GA104", kNone}, {0x176, "GA106", kNone},
    {0x177, "GA107", kNone},
    // Hopper (no display)
    {0x180, "GH100", kNoDisplay},
    // Ada
    {0x192, "AD102", kNone}, {0x193, "AD103", kNone}, {0x194, "AD104", kNone}, {0x196, "AD106", kNone}, {0x197, "AD107", kNone},
    // Blackwell. UNSURE: GB102's number (0x1a2) is from memory of nouveau's r570 GSP tables; GB100 and GB20x match NvidiaGpuGenerations.
    {0x1a0, "GB100", kNoDisplay}, {0x1a2, "GB102", kNoDisplay},
    {0x1b2, "GB202", kNone}, {0x1b3, "GB203", kNone}, {0x1b5, "GB205", kNone}, {0x1b6, "GB206", kNone}, {0x1b7, "GB207", kNone},
};

// The family boundaries of nvkm_device_ctor's switch on (chipset & 0x1f0), with nouveau's card_type.
struct FamilyRange {
  uint16_t architecture;   // chipset & 0x1f0
  Family family;
  uint16_t card_type;
};
constexpr FamilyRange kFamilies[] = {
    {0x010, Family::Nv10, 0x10},  // (NV_10 or NV_11: see below)
    {0x020, Family::Nv20, 0x20},   {0x030, Family::Nv30, 0x30},
    {0x040, Family::Nv40, 0x40},   {0x060, Family::Nv40, 0x40},
    {0x050, Family::Tesla, 0x50},  {0x080, Family::Tesla, 0x50},  {0x090, Family::Tesla, 0x50},  {0x0a0, Family::Tesla, 0x50},
    {0x0c0, Family::Fermi, 0xc0},  {0x0d0, Family::Fermi, 0xc0},
    {0x0e0, Family::Kepler, 0xe0}, {0x0f0, Family::Kepler, 0xe0}, {0x100, Family::Kepler, 0xe0},
    {0x110, Family::Maxwell, 0x110}, {0x120, Family::Maxwell, 0x110},
    {0x130, Family::Pascal, 0x130},
    {0x140, Family::Volta, 0x140},
    {0x160, Family::Turing, 0x160},
    {0x170, Family::Ampere, 0x170},
    {0x180, Family::Hopper, 0x180},
    {0x190, Family::Ada, 0x190},
    {0x1a0, Family::Blackwell, 0x1a0}, {0x1b0, Family::Blackwell, 0x1a0},
};

const KnownChip* find_chip(uint32_t chipset) {
  for (const KnownChip& c : kChips)
    if (c.chipset == chipset) return &c;
  return nullptr;
}

const FamilyRange* find_family(uint32_t chipset) {
  for (const FamilyRange& f : kFamilies)
    if (f.architecture == (chipset & 0x1f0)) return &f;
  return nullptr;
}

// BOOT_42 (Turing and later; NV_PMC_BOOT_42 in open-gpu-kernel-modules' dev_boot.h): architecture 29:24, implementation 23:20, major revision
// 19:16, minor revision 15:12. A register that reads 0 or all ones is not there.
bool boot42_usable(uint32_t boot42) { return boot42 != 0 && boot42 != 0xFFFFFFFFu && ((boot42 >> 24) & 0x3f) != 0; }

}  // namespace

Chip identify_chip(uint32_t boot0, uint32_t boot42) {
  Chip chip;
  chip.boot0 = boot0;
  if (boot0 == 0xFFFFFFFFu || boot0 == 0) return chip;   // powered down / the BAR does not answer, or nothing there

  if ((boot0 & 0x1f000000) > 0) {
    chip.chipset = (boot0 & 0x1ff00000) >> 20;
    chip.revision = static_cast<uint8_t>(boot0 & 0xff);
  } else if ((boot0 & 0xff00fff0) == 0x20004000) {
    // The NV04/NV05 quirk: these chips have no architecture field; bit 20 (and up) tells the TNT2 from the TNT.
    chip.chipset = (boot0 & 0x00f00000) ? 0x05 : 0x04;
    chip.revision = static_cast<uint8_t>(boot0 & 0x0f);
    chip.card_type = 0x04;
  } else {
    return chip;   // an NV01/NV03 or garbage: nouveau does not know it
  }

  // Turing and later identify themselves precisely in BOOT_42 (BOOT_0 has run out of room for new IDs past Blackwell).
  if (chip.chipset >= 0x160 && boot42_usable(boot42)) {
    chip.chipset = (boot42 >> 20) & 0x3ff;
    chip.revision = static_cast<uint8_t>(((boot42 >> 16) & 0xf) << 4 | ((boot42 >> 12) & 0xf));
  }

  if (chip.card_type != 0x04) {
    const FamilyRange* range = find_family(chip.chipset);
    if (!range) return Chip{boot0, chip.chipset, chip.revision};
    chip.family = range->family;
    chip.card_type = range->card_type;
    if (range->family == Family::Nv10) {
      // nouveau: NV_10 when (0x461 >> (chipset & 0xf)) & 1 (NV10, NV15, NV1A), NV_11 otherwise; the revision is not kept for these.
      chip.card_type = (0x461u & (1u << (chip.chipset & 0xf))) ? 0x10 : 0x11;
      chip.revision = 0;
    }
  } else {
    chip.family = Family::Nv04;
  }

  const KnownChip* known = find_chip(chip.chipset);
  if (!known) {
    // nouveau refuses an unknown chipset ("unknown chipset (%08x)") even inside a known family.
    Chip unknown;
    unknown.boot0 = boot0;
    unknown.chipset = chip.chipset;
    unknown.revision = chip.revision;
    unknown.card_type = chip.card_type;
    return unknown;
  }
  chip.name = known->name;
  chip.tegra = (known->flags & kTegra) != 0;
  chip.integrated = chip.tegra || (known->flags & kIgp) != 0;
  chip.has_display = !chip.tegra && (known->flags & kNoDisplay) == 0;
  chip.needs_gsp = chip.family >= Family::Ampere;
  chip.display_native = chip.has_display && chip.family >= Family::Tesla && chip.family <= Family::Turing;
  return chip;
}

const char* family_name(Family family) {
  switch (family) {
    case Family::Unknown: return "unknown";
    case Family::Nv04: return "NV04 (Fahrenheit)";
    case Family::Nv10: return "NV10 (Celsius)";
    case Family::Nv20: return "NV20 (Kelvin)";
    case Family::Nv30: return "NV30 (Rankine)";
    case Family::Nv40: return "NV40 (Curie)";
    case Family::Tesla: return "Tesla";
    case Family::Fermi: return "Fermi";
    case Family::Kepler: return "Kepler";
    case Family::Maxwell: return "Maxwell";
    case Family::Pascal: return "Pascal";
    case Family::Volta: return "Volta";
    case Family::Turing: return "Turing";
    case Family::Ampere: return "Ampere";
    case Family::Hopper: return "Hopper";
    case Family::Ada: return "Ada";
    case Family::Blackwell: return "Blackwell";
  }
  return "unknown";
}

}  // namespace nv
}  // namespace hardware
