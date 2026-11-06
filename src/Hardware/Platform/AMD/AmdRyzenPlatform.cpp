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

#include "Hardware/Platform/AMD/AmdRyzenPlatform.hpp"

namespace hardware {

namespace {
using S = AmdRyzenSoc;
using Z = AmdZenCore;
using A = AmdGpuArchitecture;
using N = AmdNpuGeneration;

struct ModelRange {
  uint8_t family, model_lo, model_hi;   // inclusive
};

struct Row {
  S soc;
  const char* codename;
  const char* products;
  ModelRange cpu[2];   // up to two ranges; family 0 ends the list
  A graphics;
  const char* graphics_name;
  AmdIpVersion gc, dcn, vcn;
  uint8_t vcn_instances;
  ArrayView<const uint16_t> gpu_ids;
  N npu;
  uint16_t npu_device;
  uint8_t npu_revision;
};

// Integrated GPU IDs (amdgpu_drv.c for the pre-discovery APUs; the PCI ID repository for the rest).
constexpr uint16_t kRavenGpu[] = {0x15DD};
constexpr uint16_t kPicassoGpu[] = {0x15D8};   // 15D8 is also Raven2 (Dali, Pollock): the PCI revision tells them apart (amd_vcn1_firmware_name)
constexpr uint16_t kRenoirGpu[] = {0x1636};
constexpr uint16_t kLucienneGpu[] = {0x164C};
constexpr uint16_t kCezanneGpu[] = {0x1638, 0x15E7};   // Cezanne, Barcelo
constexpr uint16_t kVanGoghGpu[] = {0x163F, 0x1435};   // Aerith (Steam Deck), Sephiroth (Steam Deck OLED)
constexpr uint16_t kMendocinoGpu[] = {0x1506};
constexpr uint16_t kRembrandtGpu[] = {0x1681};
constexpr uint16_t kRaphaelGpu[] = {0x164E};
constexpr uint16_t kPhoenixGpu[] = {0x15BF, 0x15C8};   // Phoenix1 (780M class), Phoenix2 (740M)
constexpr uint16_t kHawkPointGpu[] = {0x1900, 0x1901};
constexpr uint16_t kStrixGpu[] = {0x150E};
constexpr uint16_t kGraniteRidgeGpu[] = {0x13C0};
constexpr uint16_t kKrackanGpu[] = {0x1114, 0x1902};   // Krackan, Krackan2
constexpr uint16_t kStrixHaloGpu[] = {0x1586};

constexpr AmdIpVersion kNoIp = {0, 0, 0};
constexpr ArrayView<const uint16_t> kNoGpu;

// Family/model ranges: arch/x86/kernel/cpu/amd.c (Zen classes), drivers/hwmon/k10temp.c and drivers/edac (which model is which die). IP versions: amdgpu
// (amdgpu_discovery.c, amdgpu_ucode.c) for each ASIC. NPU IDs: drivers/accel/amdxdna/amdxdna_pci_drv.c.
constexpr Row kRows[] = {
    {S::SummitRidge, "Summit Ridge", "Ryzen 1000, Threadripper 1000", {{0x17, 0x00, 0x07}}, A::Unknown, nullptr, kNoIp, kNoIp, kNoIp, 0, kNoGpu, N::None, 0, 0},
    {S::PinnacleRidge, "Pinnacle Ridge", "Ryzen 2000, Threadripper 2000", {{0x17, 0x08, 0x08}}, A::Unknown, nullptr, kNoIp, kNoIp, kNoIp, 0, kNoGpu, N::None, 0, 0},
    {S::RavenRidge, "Raven Ridge", "Ryzen 2000 APUs (2200G, 2400G, 2x00U), Ryzen Embedded V1000", {{0x17, 0x10, 0x17}}, A::Vega,
     "Radeon Vega (GCN 5)", {9, 1, 0}, {1, 0, 0}, {1, 0, 0}, 1, kRavenGpu, N::None, 0, 0},
    {S::Picasso, "Picasso", "Ryzen 3000 APUs (3200G, 3400G, 3x00U/H)", {{0x17, 0x18, 0x18}}, A::Vega, "Radeon Vega (GCN 5)", {9, 1, 0}, {1, 0, 0}, {1, 0, 0}, 1,
     kPicassoGpu, N::None, 0, 0},
    {S::Dali, "Dali / Pollock", "Ryzen 3 3200U/3250U, Athlon 300U, Ryzen Embedded R1000", {{0x17, 0x20, 0x2F}}, A::Vega, "Radeon Vega (GCN 5, Raven2)", {9, 2, 2},
     {1, 0, 1}, {1, 0, 1}, 1, kPicassoGpu, N::None, 0, 0},
    {S::CastlePeak, "Castle Peak", "Threadripper 3000", {{0x17, 0x30, 0x3F}}, A::Unknown, nullptr, kNoIp, kNoIp, kNoIp, 0, kNoGpu, N::None, 0, 0},
    {S::CyanSkillfish, "Cyan Skillfish", "Ryzen 4700S/4800S desktop kit (graphics fused off)", {{0x17, 0x40, 0x4F}}, A::Unknown, nullptr, kNoIp, kNoIp, kNoIp, 0,
     kNoGpu, N::None, 0, 0},
    {S::Renoir, "Renoir", "Ryzen 4000 APUs, Ryzen Embedded V2000", {{0x17, 0x60, 0x67}}, A::Vega, "Radeon Vega (GCN 5)", {9, 3, 0}, {2, 1, 0}, {2, 2, 0}, 1,
     kRenoirGpu, N::None, 0, 0},
    {S::Lucienne, "Lucienne", "Ryzen 5000U on Zen 2 (5300U, 5500U, 5700U)", {{0x17, 0x68, 0x6F}}, A::Vega, "Radeon Vega (GCN 5)", {9, 3, 0}, {2, 1, 0}, {2, 2, 0},
     1, kLucienneGpu, N::None, 0, 0},
    {S::Matisse, "Matisse", "Ryzen 3000", {{0x17, 0x70, 0x7F}}, A::Unknown, nullptr, kNoIp, kNoIp, kNoIp, 0, kNoGpu, N::None, 0, 0},
    {S::VanGogh, "Van Gogh", "AMD Custom APU 0405/0932 (Steam Deck)", {{0x17, 0x90, 0x91}}, A::Rdna2, "Radeon (RDNA 2)", {10, 3, 1}, {3, 0, 1}, {3, 0, 2}, 1,
     kVanGoghGpu, N::None, 0, 0},
    {S::Mendocino, "Mendocino", "Ryzen 7020, Athlon Gold/Silver 7020", {{0x17, 0xA0, 0xAF}}, A::Rdna2, "Radeon 610M (RDNA 2)", {10, 3, 7}, {3, 1, 6}, {3, 1, 2},
     1, kMendocinoGpu, N::None, 0, 0},
    {S::Chagall, "Chagall", "Threadripper 5000", {{0x19, 0x08, 0x0F}}, A::Unknown, nullptr, kNoIp, kNoIp, kNoIp, 0, kNoGpu, N::None, 0, 0},
    {S::Vermeer, "Vermeer", "Ryzen 5000", {{0x19, 0x20, 0x2F}}, A::Unknown, nullptr, kNoIp, kNoIp, kNoIp, 0, kNoGpu, N::None, 0, 0},
    {S::Rembrandt, "Rembrandt", "Ryzen 6000, Ryzen 7035 (Rembrandt-R), Ryzen Embedded V3000", {{0x19, 0x40, 0x4F}}, A::Rdna2, "Radeon 660M/680M (RDNA 2)",
     {10, 3, 3}, {3, 1, 2}, {3, 1, 1}, 1, kRembrandtGpu, N::None, 0, 0},
    {S::Cezanne, "Cezanne / Barcelo", "Ryzen 5000 APUs, Ryzen 7030", {{0x19, 0x50, 0x5F}}, A::Vega, "Radeon Vega (GCN 5)", {9, 3, 0}, {2, 1, 0}, {2, 2, 0}, 1,
     kCezanneGpu, N::None, 0, 0},
    {S::StormPeak, "Storm Peak", "Threadripper 7000", {{0x19, 0x18, 0x1F}}, A::Unknown, nullptr, kNoIp, kNoIp, kNoIp, 0, kNoGpu, N::None, 0, 0},
    {S::Raphael, "Raphael / Dragon Range", "Ryzen 7000, Ryzen 7045HX", {{0x19, 0x60, 0x6F}}, A::Rdna2, "Radeon 610M (RDNA 2)", {10, 3, 6}, {3, 1, 5}, {3, 1, 2},
     1, kRaphaelGpu, N::None, 0, 0},
    {S::Phoenix, "Phoenix", "Ryzen 7040, Ryzen 8000G, Ryzen Z1", {{0x19, 0x70, 0x7F}}, A::Rdna3, "Radeon 740M/760M/780M (RDNA 3)", {11, 0, 1}, {3, 1, 4},
     {4, 0, 2}, 1, kPhoenixGpu, N::Xdna, 0x1502, 0x00},
    {S::HawkPoint, "Hawk Point", "Ryzen 8040, Ryzen AI 200", {{0x19, 0x70, 0x7F}}, A::Rdna3, "Radeon 740M/760M/780M (RDNA 3)", {11, 0, 1}, {3, 1, 4}, {4, 0, 2},
     1, kHawkPointGpu, N::Xdna, 0x1502, 0x00},
    {S::TurinShimada, "Turin / Shimada Peak", "Threadripper 9000 (and EPYC 9005)", {{0x1A, 0x00, 0x1F}}, A::Unknown, nullptr, kNoIp, kNoIp, kNoIp, 0, kNoGpu, N::None, 0, 0},
    {S::StrixPoint, "Strix Point", "Ryzen AI 300 (HX 370, 365)", {{0x1A, 0x20, 0x2F}}, A::Rdna3_5, "Radeon 880M/890M (RDNA 3.5)", {11, 5, 0}, {3, 5, 0},
     {4, 0, 5}, 1, kStrixGpu, N::Xdna2, 0x17F0, 0x10},
    {S::GraniteRidge, "Granite Ridge / Fire Range", "Ryzen 9000, Ryzen 9000HX", {{0x1A, 0x40, 0x4F}}, A::Rdna2, "Radeon 610M (RDNA 2)", {10, 3, 6}, {3, 1, 5},
     {3, 1, 2}, 1, kGraniteRidgeGpu, N::None, 0, 0},
    {S::KrackanPoint, "Krackan Point", "Ryzen AI 5/7 300 (350, 340)", {{0x1A, 0x60, 0x6F}}, A::Rdna3_5, "Radeon 820M/840M/860M (RDNA 3.5)", {11, 5, 2},
     {3, 5, 1}, {4, 0, 5}, 1, kKrackanGpu, N::Xdna2, 0x17F0, 0x11},
    {S::StrixHalo, "Strix Halo", "Ryzen AI Max, Ryzen AI Max+", {{0x1A, 0x70, 0x77}}, A::Rdna3_5, "Radeon 8050S/8060S (RDNA 3.5)", {11, 5, 1}, {3, 5, 1},
     {4, 0, 6}, 2, kStrixHaloGpu, N::Xdna2, 0x17F0, 0x20},
    {S::Zen6Client, "Zen 6", "a Zen 6 processor", {{0x1A, 0x50, 0x5F}, {0x1A, 0x80, 0xAF}}, A::Unknown, nullptr, kNoIp, kNoIp, kNoIp, 0, kNoGpu, N::None, 0, 0},
};

bool in_range(const ModelRange& r, uint32_t family, uint32_t model) {
  return r.family != 0 && r.family == family && model >= r.model_lo && model <= r.model_hi;
}

bool cpu_matches(const Row& row, uint32_t family, uint32_t model) {
  for (const ModelRange& r : row.cpu)
    if (in_range(r, family, model)) return true;
  return false;
}

bool has_gpu(const Row& row, uint16_t id) {
  for (uint16_t g : row.gpu_ids)
    if (g == id) return true;
  return false;
}

bool is_npu_device(uint16_t id) {
  for (const Row& row : kRows)
    if (row.npu != N::None && row.npu_device == id) return true;
  return false;
}

bool has_gpu_id(const AmdRyzenSocInfo& soc, uint16_t id) {
  for (uint16_t g : soc.gpu_device_ids)
    if (g == id) return true;
  return false;
}

void append_ip(FixedString<256>& out, const char* label, const AmdIpVersion& v) {
  out.append(label);
  out.append_uint(v.major);
  out.push_back('.');
  out.append_uint(v.minor);
  out.push_back('.');
  out.append_uint(v.revision);
}

AmdRyzenSocInfo info_of(const Row& row) {
  AmdRyzenSocInfo info;
  info.soc = row.soc;
  info.codename = row.codename;
  info.products = row.products;
  info.core = row.cpu[0].family ? amd_zen_core(row.cpu[0].family, row.cpu[0].model_lo) : Z::Unknown;
  info.graphics = row.graphics;
  info.graphics_name = row.graphics_name;
  info.gc = row.gc;
  info.dcn = row.dcn;
  info.vcn = row.vcn;
  info.vcn_instances = row.vcn_instances;
  info.gpu_device_ids = row.gpu_ids;
  info.npu = row.npu;
  info.npu_device_id = row.npu_device;
  info.npu_revision = row.npu_revision;
  return info;
}
}  // namespace

AmdZenCore amd_zen_core(uint32_t family, uint32_t model) {
  if (family == 0x17) {
    if (model == 0x08 || model == 0x18) return Z::ZenPlus;   // Pinnacle Ridge, Picasso: the 12 nm shrink (Linux counts it as Zen 1)
    if (model <= 0x2F || (model >= 0x50 && model <= 0x5F)) return Z::Zen;
    if ((model >= 0x30 && model <= 0x4F) || (model >= 0x60 && model <= 0x7F) || model == 0x90 || model == 0x91 || (model >= 0xA0 && model <= 0xAF)) return Z::Zen2;
    return Z::Unknown;
  }
  if (family == 0x19) {
    if (model <= 0x0F || (model >= 0x20 && model <= 0x5F)) return Z::Zen3;
    if ((model >= 0x10 && model <= 0x1F) || (model >= 0x60 && model <= 0xAF)) return Z::Zen4;
    return Z::Unknown;
  }
  if (family == 0x1A) {
    if (model <= 0x2F || (model >= 0x40 && model <= 0x4F) || (model >= 0x60 && model <= 0x7F) || (model >= 0xD0 && model <= 0xD7)) return Z::Zen5;
    if ((model >= 0x50 && model <= 0x5F) || (model >= 0x80 && model <= 0xAF) || (model >= 0xC0 && model <= 0xCF) || (model >= 0xD8 && model <= 0xEF)) return Z::Zen6;
    return Z::Unknown;
  }
  return Z::Unknown;
}

const char* amd_zen_core_name(AmdZenCore core) {
  switch (core) {
    case Z::Zen: return "Zen";
    case Z::ZenPlus: return "Zen+";
    case Z::Zen2: return "Zen 2";
    case Z::Zen3: return "Zen 3";
    case Z::Zen4: return "Zen 4";
    case Z::Zen5: return "Zen 5";
    case Z::Zen6: return "Zen 6";
    case Z::Unknown: break;
  }
  return "not a Zen core";
}

AmdVcnInfo AmdRyzenSocInfo::video_engine() const {
  if (!has_video_engine()) return AmdVcnInfo();
  const AmdVcnFirmwareHint hint = soc == S::Renoir || soc == S::Lucienne ? AmdVcnFirmwareHint::Renoir :
                                  soc == S::Cezanne ? AmdVcnFirmwareHint::GreenSardine : AmdVcnFirmwareHint::None;
  return amd_vcn_info(vcn, hint);
}

AmdRyzenSocInfo amd_ryzen_identify(uint32_t family, uint32_t model, uint16_t gpu_device_id) {
  const Row* first = nullptr;
  for (const Row& row : kRows) {
    if (!cpu_matches(row, family, model)) continue;
    if (!first) first = &row;
    if (gpu_device_id && has_gpu(row, gpu_device_id)) return info_of(row);
  }
  if (first) return info_of(*first);
  AmdRyzenSocInfo info;
  info.core = amd_zen_core(family, model);
  return info;
}

AmdRyzenSocInfo amd_ryzen_soc_of_gpu(uint16_t gpu_device_id) {
  if (gpu_device_id == 0) return AmdRyzenSocInfo();
  for (const Row& row : kRows)
    if (has_gpu(row, gpu_device_id)) return info_of(row);
  return AmdRyzenSocInfo();
}

AmdRyzenSocInfo amd_ryzen_soc_of_npu(uint16_t npu_device_id, uint8_t revision) {
  if (npu_device_id == 0) return AmdRyzenSocInfo();
  for (const Row& row : kRows)
    if (row.npu != N::None && row.npu_device == npu_device_id && row.npu_revision == revision) return info_of(row);
  return AmdRyzenSocInfo();
}

const char* amd_ryzen_soc_name(AmdRyzenSoc soc) {
  for (const Row& row : kRows)
    if (row.soc == soc) return row.codename;
  return "unknown AMD processor";
}

const char* amd_npu_generation_name(AmdNpuGeneration generation) {
  switch (generation) {
    case N::Xdna: return "XDNA";
    case N::Xdna2: return "XDNA 2";
    case N::None: break;
  }
  return "no NPU";
}

AmdRyzenMachine amd_ryzen_identify_machine(uint32_t family, uint32_t model, const IPciBus& pci) {
  constexpr uint16_t kAti = 0x1002, kAmd = 0x1022;
  AmdRyzenMachine machine;
  machine.soc = amd_ryzen_identify(family, model);
  PciDeviceInfo d;
  for (uint32_t i = 0; pci.pci_device(i, &d); ++i) {
    if (d.vendor_id == kAti && d.class_code == kPciClassDisplay && !machine.gpu_found()) {
      // Only the SoC's own graphics: a card in a slot is not part of the platform.
      const AmdRyzenSocInfo by_cpu = amd_ryzen_identify(family, model, d.device_id);
      if (by_cpu.soc != S::Unknown && has_gpu_id(by_cpu, d.device_id)) {
        machine.soc = by_cpu;
        machine.gpu_device_id = d.device_id;
        machine.gpu_address = d.address;
      }
    } else if (d.vendor_id == kAmd && !machine.npu_found() && is_npu_device(d.device_id)) {
      machine.npu_device_id = d.device_id;
      machine.npu_revision = static_cast<uint8_t>(pci.read32(d.address, 0x08));   // the revision ID is the low byte of the class/revision word
      machine.npu_address = d.address;
    }
  }
  return machine;
}

void amd_ryzen_describe(const AmdRyzenMachine& machine, FixedString<256>& out) {
  const AmdRyzenSocInfo& soc = machine.soc;
  out.clear();
  if (soc.soc == S::Unknown) {
    out.append("unknown AMD processor (");
    out.append(amd_zen_core_name(soc.core));
    out.append(")");
    return;
  }
  out.append(soc.codename);
  out.append(" (");
  out.append(soc.products);
  out.append("), ");
  out.append(amd_zen_core_name(soc.core));
  if (soc.has_graphics()) {
    out.append("; ");
    out.append(soc.graphics_name);
    if (!machine.gpu_found()) out.append(" not on the bus");
    append_ip(out, ", GC ", soc.gc);
    append_ip(out, ", DCN ", soc.dcn);
    append_ip(out, ", VCN ", soc.vcn);
    if (soc.vcn_instances > 1) {
      out.append(" x");
      out.append_uint(soc.vcn_instances);
    }
    const AmdVcnInfo vcn = soc.video_engine();
    if (vcn.firmware) {
      out.append(" (");
      out.append(vcn.firmware);
      out.append(")");
    }
  } else {
    out.append("; no integrated graphics");
  }
  if (soc.has_npu()) {
    out.append("; ");
    out.append(amd_npu_generation_name(soc.npu));
    out.append(" NPU");
    if (machine.npu_found()) {
      out.append(" rev ");
      out.append_uint(machine.npu_revision);
    } else {
      out.append(" not on the bus (not every part has it)");
    }
  }
}

uint32_t amd_ryzen_soc_count() { return sizeof(kRows) / sizeof(kRows[0]); }

AmdRyzenSocInfo amd_ryzen_soc_at(uint32_t index) {
  if (index >= amd_ryzen_soc_count()) return AmdRyzenSocInfo();
  return info_of(kRows[index]);
}

}  // namespace hardware
