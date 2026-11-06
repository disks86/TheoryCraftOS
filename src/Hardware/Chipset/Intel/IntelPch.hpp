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
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

// Every Intel I/O hub the catalogue knows: the ICHs of the Core 2 era, the discrete PCHs (5 series to 800 series and the server C600..C740), the PCHs
// in the package of the U/Y processors, and the PCH part of the SoCs (Atom, Meteor Lake on).
enum class IntelPch : uint8_t {
  Unknown,
  Ich8, Ich9, Ich10,
  IbexPeak,         // 5 series
  CougarPoint,      // 6 series
  Patsburg,         // X79, C600
  PantherPoint,     // 7 series
  LynxPoint,        // 8 series
  LynxPointLp,      // Haswell-U/Y package
  Wellsburg,        // X99, C610
  WildcatPoint,     // 9 series
  WildcatPointLp,   // Broadwell-U/Y package
  SunrisePointH,    // 100 series
  SunrisePointLp,   // Skylake-U/Y and Kaby Lake-U/Y package
  UnionPoint,       // 200 series
  Lewisburg,        // C620
  CannonPointH,     // 300 series
  CannonPointLp,    // Whiskey Lake-U, Cannon Lake-U package
  CometLakeH,       // 400 series
  CometLakeV,       // the 22 nm B460/H410
  CometLakeLp,      // Comet Lake-U package
  IceLakeLp,        // Ice Lake-U/Y package
  IceLakeN,
  TigerLakeLp,      // Tiger Lake-UP3/UP4 package
  TigerLakeH,       // 500 series
  AlderLakeS,       // 600 series
  AlderLakeP,       // Alder Lake-P/H/U and Raptor Lake-P/H/U package
  AlderLakeN,       // Alder Lake-N and -M (the "M" PCH)
  RaptorLakeS,      // 700 series
  MeteorLakeP,      // the SoC tile of Meteor Lake-U/H and Arrow Lake-U
  MeteorLakeSocS,   // the SoC tile of the desktop parts
  ArrowLakeS,       // 800 series (the Meteor Lake PCH-S die)
  ArrowLakeH,       // the SoC tile of Arrow Lake-H
  LunarLake,
  PantherLakeH,
  PantherLakeP,
  WildcatLake,
  NovaLakeS,        // the PCH of Nova Lake-S
  Emmitsburg,       // C740
  DiamondRapids,    // the Xeon platform after Birch Stream
  BirchStream,      // the Xeon 6 platform (no separate PCH; the functions are in the I/O die)
  Denverton,        // Atom C3000 SoC
  CedarFork,        // Snow Ridge (Atom P5900) SoC
  ApolloLake,       // Broxton SoC
  GeminiLake,
  ElkhartLake,
  JasperLake,
};

// Where the hub lives.
enum class IntelPchForm : uint8_t { Discrete, OnPackage, Soc };

// The TCO watchdog of the hub as the OS can reach it (Linux i2c-i801's FEATURE_TCO_SPT/CNL): its I/O base is in the SMBus function (TCOBASE 0x50,
// enabled by TCOCTL 0x54 bit 8). Version 4 (Sunrise Point era) keeps NO_REBOOT behind the hidden P2SB bridge, so it can be halted but not armed
// here; version 6 (Cannon Lake on) has NO_REBOOT in TCO1_CNT. The ICHs and 5..9 series put it in the LPC bridge's ACPI block (not covered).
enum class IntelTcoKind : uint8_t { None, Version4, Version6 };

struct IntelPchInfo {
  IntelPch pch = IntelPch::Unknown;
  const char* codename = "unknown Intel chipset";
  const char* products = "";
  IntelPchForm form = IntelPchForm::Discrete;
  ArrayView<const uint16_t> smbus_ids;   // PCI device IDs of its SMBus function (vendor 0x8086)
  // Since Sunrise Point the SPI flash controller is its own function (00:1F.5, where the BIOS protection bits are); before, they are in the LPC
  // bridge (00:1F.0). Both keep BIOS_CNTL at config offset 0xDC.
  bool spi_function = false;
  IntelTcoKind tco = IntelTcoKind::None;
};

IntelPchInfo intel_pch_of_smbus(uint16_t smbus_device_id);
IntelPchInfo intel_pch_info(IntelPch pch);
const char* intel_pch_name(IntelPch pch);

// The hubs a processor is paired with, most likely first (a Kaby Lake-S runs on 100 and 200 series boards; a Raptor Lake-S on 600 and 700 series).
struct IntelPchCandidates {
  IntelPch pch[3] = {};
  uint8_t count = 0;
  bool contains(IntelPch p) const {
    for (uint8_t i = 0; i < count; ++i)
      if (pch[i] == p) return true;
    return false;
  }
};
IntelPchCandidates intel_pch_for_cpu(uint32_t family, uint32_t model, uint32_t stepping);

uint32_t intel_pch_count();
IntelPchInfo intel_pch_at(uint32_t index);

}  // namespace hardware
