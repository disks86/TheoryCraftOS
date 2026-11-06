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

#include "Hardware/Chipset/Intel/IntelPch.hpp"

namespace hardware {

namespace {
using P = IntelPch;
using F = IntelPchForm;

struct Row {
  P pch;
  const char* codename;
  const char* products;
  F form;
  ArrayView<const uint16_t> smbus;
  bool spi_function;
  IntelTcoKind tco;
};

// SMBus device IDs: Linux drivers/i2c/busses/i2c-i801.c (and include/linux/pci_ids.h).
constexpr uint16_t kIch8[] = {0x283E};
constexpr uint16_t kIch9[] = {0x2930};
constexpr uint16_t kIch10[] = {0x3A30, 0x3A60};
constexpr uint16_t kIbexPeak[] = {0x3B30};
constexpr uint16_t kCougarPoint[] = {0x1C22};
constexpr uint16_t kPatsburg[] = {0x1D22, 0x1D70, 0x1D71, 0x1D72};   // the main controller and the three IDF controllers
constexpr uint16_t kPantherPoint[] = {0x1E22};
constexpr uint16_t kLynxPoint[] = {0x8C22};
constexpr uint16_t kLynxPointLp[] = {0x9C22};
constexpr uint16_t kWellsburg[] = {0x8D22, 0x8D7D, 0x8D7E, 0x8D7F};
constexpr uint16_t kWildcatPoint[] = {0x8CA2};
constexpr uint16_t kWildcatPointLp[] = {0x9CA2};
constexpr uint16_t kSunrisePointH[] = {0xA123};
constexpr uint16_t kSunrisePointLp[] = {0x9D23};
constexpr uint16_t kUnionPoint[] = {0xA2A3};
constexpr uint16_t kLewisburg[] = {0xA1A3, 0xA223};
constexpr uint16_t kCannonPointH[] = {0xA323};
constexpr uint16_t kCannonPointLp[] = {0x9DA3};
constexpr uint16_t kCometLakeH[] = {0x06A3};
constexpr uint16_t kCometLakeV[] = {0xA3A3};
constexpr uint16_t kCometLakeLp[] = {0x02A3};
constexpr uint16_t kIceLakeLp[] = {0x34A3};
constexpr uint16_t kIceLakeN[] = {0x38A3};
constexpr uint16_t kTigerLakeLp[] = {0xA0A3};
constexpr uint16_t kTigerLakeH[] = {0x43A3};
constexpr uint16_t kAlderLakeS[] = {0x7AA3};
constexpr uint16_t kAlderLakeP[] = {0x51A3};
constexpr uint16_t kAlderLakeN[] = {0x54A3};
constexpr uint16_t kRaptorLakeS[] = {0x7A23};
constexpr uint16_t kMeteorLakeP[] = {0x7E22};
constexpr uint16_t kMeteorLakeSocS[] = {0xAE22};
constexpr uint16_t kArrowLakeS[] = {0x7F23};
constexpr uint16_t kArrowLakeH[] = {0x7722};
constexpr uint16_t kPantherLakeH[] = {0xE322};
constexpr uint16_t kPantherLakeP[] = {0xE422};
constexpr uint16_t kWildcatLake[] = {0x4D22};
constexpr uint16_t kNovaLakeS[] = {0x6E23};
constexpr uint16_t kDiamondRapids[] = {0x5827};
constexpr uint16_t kEmmitsburg[] = {0x1BC9};
constexpr uint16_t kBirchStream[] = {0x5796};
constexpr uint16_t kDenverton[] = {0x19DF};
constexpr uint16_t kCedarFork[] = {0x18DF};
constexpr uint16_t kApolloLake[] = {0x5AD4};
constexpr uint16_t kGeminiLake[] = {0x31D4};
constexpr uint16_t kElkhartLake[] = {0x4B23};
constexpr uint16_t kJasperLake[] = {0x4DA3};
constexpr ArrayView<const uint16_t> kNone;

constexpr Row kRows[] = {
    {P::Ich8, "ICH8", "965 series (P965, G965, Q965; GM965 laptops)", F::Discrete, kIch8, false, IntelTcoKind::None},
    {P::Ich9, "ICH9", "3 series (P35, G33, X38, Q35), 4 series laptops (GM45, PM45)", F::Discrete, kIch9, false, IntelTcoKind::None},
    {P::Ich10, "ICH10", "4 series (P45, G45, X48, Q45), X58, 5520", F::Discrete, kIch10, false, IntelTcoKind::None},
    {P::IbexPeak, "Ibex Peak", "5 series (P55, H55, H57, Q57; HM55/HM57), 3400 series", F::Discrete, kIbexPeak, false, IntelTcoKind::None},
    {P::CougarPoint, "Cougar Point", "6 series (Z68, P67, H67, H61, B65, Q67; HM65/HM67), C200", F::Discrete, kCougarPoint, false, IntelTcoKind::None},
    {P::Patsburg, "Patsburg", "X79, C600", F::Discrete, kPatsburg, false, IntelTcoKind::None},
    {P::PantherPoint, "Panther Point", "7 series (Z77, Z75, H77, B75, Q77; HM76/HM77), C216", F::Discrete, kPantherPoint, false, IntelTcoKind::None},
    {P::LynxPoint, "Lynx Point", "8 series (Z87, H87, H81, B85, Q87; HM86/HM87), C220", F::Discrete, kLynxPoint, false, IntelTcoKind::None},
    {P::LynxPointLp, "Lynx Point-LP", "the PCH in Haswell-U/Y packages", F::OnPackage, kLynxPointLp, false, IntelTcoKind::None},
    {P::Wellsburg, "Wellsburg", "X99, C610", F::Discrete, kWellsburg, false, IntelTcoKind::None},
    {P::WildcatPoint, "Wildcat Point", "9 series (Z97, H97)", F::Discrete, kWildcatPoint, false, IntelTcoKind::None},
    {P::WildcatPointLp, "Wildcat Point-LP", "the PCH in Broadwell-U/Y packages", F::OnPackage, kWildcatPointLp, false, IntelTcoKind::None},
    {P::SunrisePointH, "Sunrise Point-H", "100 series (Z170, H170, B150, H110, Q170, Q150; CM236, HM170), C230", F::Discrete, kSunrisePointH, true, IntelTcoKind::Version4},
    {P::SunrisePointLp, "Sunrise Point-LP", "the PCH in Skylake-U/Y and Kaby Lake-U/Y/R packages", F::OnPackage, kSunrisePointLp, true, IntelTcoKind::Version4},
    {P::UnionPoint, "Union Point", "200 series (Z270, H270, B250, Q270, Q250), Z370, B365, X299", F::Discrete, kUnionPoint, true, IntelTcoKind::Version4},
    {P::Lewisburg, "Lewisburg", "C620 series (C621..C628, C621A..C627A)", F::Discrete, kLewisburg, true, IntelTcoKind::Version4},
    {P::CannonPointH, "Cannon Point-H", "300 series (Z390, H370, B360, H310, Q370; HM370, QM370), C240", F::Discrete, kCannonPointH, true, IntelTcoKind::Version6},
    {P::CannonPointLp, "Cannon Point-LP", "the PCH in Whiskey Lake-U and Cannon Lake-U packages", F::OnPackage, kCannonPointLp, true, IntelTcoKind::Version6},
    {P::CometLakeH, "Comet Lake PCH-H", "400 series (Z490, H470, B460, H410, Q470, W480; HM470), C250", F::Discrete, kCometLakeH, true, IntelTcoKind::Version6},
    {P::CometLakeV, "Comet Lake PCH-V", "B460, H410 (the 22 nm parts)", F::Discrete, kCometLakeV, true, IntelTcoKind::Version4},
    {P::CometLakeLp, "Comet Lake PCH-LP", "the PCH in Comet Lake-U packages", F::OnPackage, kCometLakeLp, true, IntelTcoKind::Version6},
    {P::IceLakeLp, "Ice Lake PCH-LP", "the PCH in Ice Lake-U/Y packages", F::OnPackage, kIceLakeLp, true, IntelTcoKind::Version6},
    {P::IceLakeN, "Ice Lake PCH-N", "Ice Lake-N", F::OnPackage, kIceLakeN, true, IntelTcoKind::Version6},
    {P::TigerLakeLp, "Tiger Lake PCH-LP", "the PCH in Tiger Lake-UP3/UP4 packages", F::OnPackage, kTigerLakeLp, true, IntelTcoKind::Version6},
    {P::TigerLakeH, "Tiger Lake PCH-H", "500 series (Z590, H570, B560, H510, Q570, W580; HM570, WM590)", F::Discrete, kTigerLakeH, true, IntelTcoKind::Version6},
    {P::AlderLakeS, "Alder Lake PCH-S", "600 series (Z690, H670, B660, H610, Q670, W680)", F::Discrete, kAlderLakeS, true, IntelTcoKind::Version6},
    {P::AlderLakeP, "Alder Lake PCH-P", "the PCH in Alder Lake-P/H/U and Raptor Lake-P/H/U packages", F::OnPackage, kAlderLakeP, true, IntelTcoKind::Version6},
    {P::AlderLakeN, "Alder Lake PCH-N", "the PCH in Alder Lake-N (N100..N305, i3-N) and Alder Lake-M packages", F::OnPackage, kAlderLakeN, true, IntelTcoKind::Version6},
    {P::RaptorLakeS, "Raptor Lake PCH-S", "700 series (Z790, H770, B760)", F::Discrete, kRaptorLakeS, true, IntelTcoKind::Version6},
    {P::MeteorLakeP, "Meteor Lake SoC", "the SoC tile of Core Ultra 100 (Meteor Lake-U/H) and Core Ultra 200U (Arrow Lake-U)", F::Soc, kMeteorLakeP, true, IntelTcoKind::Version6},
    {P::MeteorLakeSocS, "Meteor Lake SoC-S", "the SoC tile of the Meteor Lake-S and Arrow Lake-S packages", F::Soc, kMeteorLakeSocS, true, IntelTcoKind::Version6},
    {P::ArrowLakeS, "Arrow Lake PCH-S", "800 series (Z890, B860, H810, Q870, W880)", F::Discrete, kArrowLakeS, true, IntelTcoKind::Version6},
    {P::ArrowLakeH, "Arrow Lake-H SoC", "the SoC tile of Core Ultra 200H (Arrow Lake-H)", F::Soc, kArrowLakeH, true, IntelTcoKind::Version6},
    {P::LunarLake, "Lunar Lake SoC", "the platform controller of Core Ultra 200V (Lunar Lake)", F::Soc, kNone, true, IntelTcoKind::None},
    {P::PantherLakeH, "Panther Lake-H SoC", "the platform controller of Core Ultra 300H (Panther Lake-H)", F::Soc, kPantherLakeH, true, IntelTcoKind::Version6},
    {P::PantherLakeP, "Panther Lake-P SoC", "the platform controller of Core Ultra 300U/P (Panther Lake-U/P)", F::Soc, kPantherLakeP, true, IntelTcoKind::Version6},
    {P::WildcatLake, "Wildcat Lake SoC", "the platform controller of Wildcat Lake", F::Soc, kWildcatLake, true, IntelTcoKind::Version6},
    {P::NovaLakeS, "Nova Lake PCH-S", "the PCH of Nova Lake-S desktops", F::Discrete, kNovaLakeS, true, IntelTcoKind::Version6},
    {P::Emmitsburg, "Emmitsburg", "C740 series (C741)", F::Discrete, kEmmitsburg, true, IntelTcoKind::Version6},
    {P::DiamondRapids, "Diamond Rapids platform", "the Xeon platform of Diamond Rapids", F::Soc, kDiamondRapids, true, IntelTcoKind::None},
    {P::BirchStream, "Birch Stream", "the Xeon 6 platform (Granite Rapids, Sierra Forest, Clearwater Forest)", F::Soc, kBirchStream, true, IntelTcoKind::None},
    {P::Denverton, "Denverton", "Atom C3000", F::Soc, kDenverton, true, IntelTcoKind::Version4},
    {P::CedarFork, "Cedar Fork", "the PCH of Snow Ridge (Atom P5900)", F::Soc, kCedarFork, true, IntelTcoKind::Version6},
    {P::ApolloLake, "Apollo Lake (Broxton)", "Pentium/Celeron N4200/J3455, Atom E3900", F::Soc, kApolloLake, true, IntelTcoKind::None},
    {P::GeminiLake, "Gemini Lake", "Pentium Silver/Celeron N4000/J4105", F::Soc, kGeminiLake, true, IntelTcoKind::None},
    {P::ElkhartLake, "Elkhart Lake", "Atom x6000E, Pentium/Celeron J6000/N6000 embedded", F::Soc, kElkhartLake, true, IntelTcoKind::Version6},
    {P::JasperLake, "Jasper Lake", "Pentium Silver/Celeron N5000/N6000", F::Soc, kJasperLake, true, IntelTcoKind::Version6},
};

IntelPchInfo info_of(const Row& row) {
  IntelPchInfo info;
  info.pch = row.pch;
  info.codename = row.codename;
  info.products = row.products;
  info.form = row.form;
  info.smbus_ids = row.smbus;
  info.spi_function = row.spi_function;
  info.tco = row.tco;
  return info;
}

// Processor (family, model, stepping range) -> the hubs it pairs with. Models: arch/x86/include/asm/intel-family.h; pairings: Intel's platform
// briefs. Atom parts that are their own SoC name their SoC.
struct CpuRow {
  uint8_t family, model, stepping_lo, stepping_hi;
  P pch[3];
};
constexpr CpuRow kCpus[] = {
    {6, 0x0F, 0x0, 0xF, {P::Ich8, P::Ich9, P::Ich10}},           // Conroe/Merom
    {6, 0x16, 0x0, 0xF, {P::Ich8, P::Ich9}},                      // Conroe-L
    {6, 0x17, 0x0, 0xF, {P::Ich9, P::Ich10, P::Ich8}},           // Penryn/Wolfdale
    {6, 0x1A, 0x0, 0xF, {P::Ich10}},                              // Bloomfield/Nehalem-EP (X58, 5520)
    {6, 0x1E, 0x0, 0xF, {P::IbexPeak}},                           // Lynnfield/Clarksfield
    {6, 0x1F, 0x0, 0xF, {P::IbexPeak}},
    {6, 0x25, 0x0, 0xF, {P::IbexPeak}},                           // Clarkdale/Arrandale
    {6, 0x2C, 0x0, 0xF, {P::Ich10}},                              // Gulftown/Westmere-EP
    {6, 0x2E, 0x0, 0xF, {P::Ich10}},                              // Nehalem-EX
    {6, 0x2F, 0x0, 0xF, {P::Ich10}},                              // Westmere-EX
    {6, 0x2A, 0x0, 0xF, {P::CougarPoint, P::PantherPoint}},       // Sandy Bridge
    {6, 0x2D, 0x0, 0xF, {P::Patsburg}},                           // Sandy Bridge-E/EP
    {6, 0x3A, 0x0, 0xF, {P::PantherPoint, P::CougarPoint}},       // Ivy Bridge
    {6, 0x3E, 0x0, 0xF, {P::Patsburg}},                           // Ivy Bridge-E/EP
    {6, 0x3C, 0x0, 0xF, {P::LynxPoint, P::WildcatPoint}},         // Haswell
    {6, 0x46, 0x0, 0xF, {P::LynxPoint}},                          // Haswell GT3e
    {6, 0x45, 0x0, 0xF, {P::LynxPointLp}},                        // Haswell-U/Y
    {6, 0x3F, 0x0, 0xF, {P::Wellsburg}},                          // Haswell-E/EP
    {6, 0x3D, 0x0, 0xF, {P::WildcatPointLp}},                     // Broadwell-U/Y
    {6, 0x47, 0x0, 0xF, {P::WildcatPoint}},                       // Broadwell-C/H
    {6, 0x4F, 0x0, 0xF, {P::Wellsburg}},                          // Broadwell-E/EP
    {6, 0x4E, 0x0, 0xF, {P::SunrisePointLp}},                     // Skylake-U/Y
    {6, 0x5E, 0x0, 0xF, {P::SunrisePointH, P::UnionPoint}},       // Skylake-S/H
    {6, 0x55, 0x0, 0xF, {P::Lewisburg}},                          // Skylake-SP, Cascade Lake, Cooper Lake (X299 boards: Union Point)
    {6, 0x8E, 0x0, 0xA, {P::SunrisePointLp}},                     // Kaby Lake-U/Y, Amber Lake, Kaby Lake-R
    {6, 0x8E, 0xB, 0xB, {P::CannonPointLp}},                      // Whiskey Lake-U
    {6, 0x8E, 0xC, 0xD, {P::CometLakeLp}},                        // Comet Lake-U (and Amber Lake-Y 4.2W)
    {6, 0x9E, 0x0, 0x9, {P::SunrisePointH, P::UnionPoint}},       // Kaby Lake-S/H
    {6, 0x9E, 0xA, 0xD, {P::CannonPointH, P::UnionPoint}},        // Coffee Lake (Z370, B365 boards carry Union Point)
    {6, 0x66, 0x0, 0xF, {P::CannonPointLp}},                      // Cannon Lake-U
    {6, 0xA5, 0x0, 0xF, {P::CometLakeH, P::CometLakeV}},          // Comet Lake-S/H
    {6, 0xA6, 0x0, 0xF, {P::CometLakeLp}},                        // Comet Lake-U
    {6, 0x7D, 0x0, 0xF, {P::IceLakeLp}},                          // Ice Lake-U/Y
    {6, 0x7E, 0x0, 0xF, {P::IceLakeLp}},
    {6, 0x6A, 0x0, 0xF, {P::Lewisburg}},                          // Ice Lake-SP (C620A)
    {6, 0x6C, 0x0, 0xF, {P::Lewisburg}},                          // Ice Lake-D
    {6, 0x8C, 0x0, 0xF, {P::TigerLakeLp}},                        // Tiger Lake-UP3/UP4
    {6, 0x8D, 0x0, 0xF, {P::TigerLakeH}},                         // Tiger Lake-H
    {6, 0xA7, 0x0, 0xF, {P::TigerLakeH, P::CometLakeH}},          // Rocket Lake (500 and 400 series boards)
    {6, 0x97, 0x0, 0xF, {P::AlderLakeS, P::RaptorLakeS}},         // Alder Lake-S
    {6, 0x9A, 0x0, 0xF, {P::AlderLakeP, P::AlderLakeN}},          // Alder Lake-P/H/U (and -M)
    {6, 0xBE, 0x0, 0xF, {P::AlderLakeN}},                         // Alder Lake-N
    {6, 0xB7, 0x0, 0xF, {P::RaptorLakeS, P::AlderLakeS}},         // Raptor Lake-S
    {6, 0xBF, 0x0, 0xF, {P::RaptorLakeS, P::AlderLakeS}},         // Raptor Lake-S refresh
    {6, 0xBA, 0x0, 0xF, {P::AlderLakeP}},                         // Raptor Lake-P/H/U
    {6, 0xD7, 0x0, 0xF, {P::RaptorLakeS, P::AlderLakeS}},         // Bartlett Lake
    {6, 0xAA, 0x0, 0xF, {P::MeteorLakeP}},                        // Meteor Lake-U/H
    {6, 0xAC, 0x0, 0xF, {P::MeteorLakeSocS}},                     // Meteor Lake-S
    {6, 0xB5, 0x0, 0xF, {P::MeteorLakeP}},                        // Arrow Lake-U
    {6, 0xC5, 0x0, 0xF, {P::ArrowLakeH}},                         // Arrow Lake-H
    {6, 0xC6, 0x0, 0xF, {P::ArrowLakeS, P::MeteorLakeSocS}},      // Arrow Lake-S
    {6, 0xBD, 0x0, 0xF, {P::LunarLake}},                          // Lunar Lake
    {6, 0xCC, 0x0, 0xF, {P::PantherLakeH, P::PantherLakeP}},      // Panther Lake
    {6, 0xD5, 0x0, 0xF, {P::WildcatLake}},                        // Wildcat Lake
    {6, 0x8F, 0x0, 0xF, {P::Emmitsburg}},                         // Sapphire Rapids
    {6, 0xCF, 0x0, 0xF, {P::Emmitsburg}},                         // Emerald Rapids
    {6, 0xAD, 0x0, 0xF, {P::BirchStream}},                        // Granite Rapids-SP
    {6, 0xAE, 0x0, 0xF, {P::BirchStream}},                        // Granite Rapids-D
    {6, 0xAF, 0x0, 0xF, {P::BirchStream}},                        // Sierra Forest
    {6, 0xDD, 0x0, 0xF, {P::BirchStream}},                        // Clearwater Forest
    {6, 0x5F, 0x0, 0xF, {P::Denverton}},                          // Denverton
    {6, 0x86, 0x0, 0xF, {P::CedarFork}},                          // Snow Ridge
    {6, 0x5C, 0x0, 0xF, {P::ApolloLake}},                         // Goldmont
    {6, 0x7A, 0x0, 0xF, {P::GeminiLake}},                         // Goldmont Plus
    {6, 0x96, 0x0, 0xF, {P::ElkhartLake}},                        // Elkhart Lake
    {6, 0x9C, 0x0, 0xF, {P::JasperLake}},                         // Jasper Lake
    {18, 0x01, 0x0, 0xF, {P::NovaLakeS}},                         // Nova Lake-S
    {18, 0x03, 0x0, 0xF, {P::NovaLakeS}},
    {19, 0x01, 0x0, 0xF, {P::DiamondRapids}},                     // Diamond Rapids
};
}  // namespace

IntelPchInfo intel_pch_of_smbus(uint16_t smbus_device_id) {
  for (const Row& row : kRows)
    for (uint16_t id : row.smbus)
      if (id == smbus_device_id) return info_of(row);
  return IntelPchInfo();
}

IntelPchInfo intel_pch_info(IntelPch pch) {
  for (const Row& row : kRows)
    if (row.pch == pch) return info_of(row);
  return IntelPchInfo();
}

const char* intel_pch_name(IntelPch pch) { return intel_pch_info(pch).codename; }

IntelPchCandidates intel_pch_for_cpu(uint32_t family, uint32_t model, uint32_t stepping) {
  IntelPchCandidates out;
  for (const CpuRow& row : kCpus) {
    if (row.family != family || row.model != model || stepping < row.stepping_lo || stepping > row.stepping_hi) continue;
    for (P pch : row.pch)
      if (pch != P::Unknown) out.pch[out.count++] = pch;
    break;
  }
  return out;
}

uint32_t intel_pch_count() { return sizeof(kRows) / sizeof(kRows[0]); }

IntelPchInfo intel_pch_at(uint32_t index) {
  if (index >= intel_pch_count()) return IntelPchInfo();
  return info_of(kRows[index]);
}

}  // namespace hardware
