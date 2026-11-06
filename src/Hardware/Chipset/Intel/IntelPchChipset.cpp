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

#include "Hardware/Chipset/Intel/IntelPchChipset.hpp"

namespace hardware {

namespace {
constexpr uint16_t kIntel = 0x8086;
constexpr uint8_t kFirstChipsetDevice = 0x10;
constexpr uint16_t kPciCommand = 0x04, kSmbusBar = 0x20, kSmbusHostConfig = 0x40, kBiosControl = 0xDC, kHeciFirmwareStatus = 0x40;
constexpr uint32_t kCommandIoSpace = 1u << 0;
constexpr uint8_t kHostConfigEnable = 1u << 0;
}  // namespace

const char* intel_me_working_state_name(uint8_t state) {
  switch (state) {
    case 0: return "resetting";
    case 1: return "initializing";
    case 2: return "recovery";
    case 3: return "test";
    case 4: return "disabled";
    case 5: return "normal";
    case 6: return "waiting to be disabled";
    case 7: return "in transition";
    case 8: return "invalid CPU";
    default: return "unknown state";
  }
}

const char* intel_me_operation_mode_name(uint8_t mode) {
  switch (mode) {
    case 0: return "normal";
    case 1: return "IGNITION";
    case 2: return "debug";
    case 3: return "temporarily disabled";
    case 4: return "security override (jumper)";
    case 5: return "security override (HECI)";
    case 6: return "SPS";
    default: return "unknown mode";
  }
}

IntelBiosProtection intel_bios_protection_decode(uint8_t bios_control) {
  IntelBiosProtection p;
  p.known = true;
  p.write_enable = bios_control & 0x01;
  p.lock_enable = bios_control & 0x02;
  p.smm_protect = bios_control & 0x20;
  return p;
}

void IntelPchChipset::probe(const IPciBus& pci, const ChipsetCpu& cpu) {
  info_ = IntelPchInfo();
  expected_ = cpu.vendor == ChipsetVendor::Intel ? intel_pch_for_cpu(cpu.family, cpu.model, cpu.stepping) : IntelPchCandidates();
  by_cpu_ = unusual_pairing_ = false;
  inventory_.clear();
  smbus_ = ChipsetSmbus();
  bios_ = IntelBiosProtection();
  me_ = IntelMeStatus();

  PciDeviceInfo d;
  for (uint32_t i = 0; pci.pci_device(i, &d); ++i)
    if (d.vendor_id == kIntel && d.address.bus == 0 && d.address.device >= kFirstChipsetDevice) inventory_.add(d);

  ChipsetFunctionInfo smbus;
  const bool has_smbus = inventory_.find(ChipsetFunction::Smbus, &smbus);
  if (has_smbus) info_ = intel_pch_of_smbus(smbus.device_id);
  if (info_.pch == IntelPch::Unknown && expected_.count) {
    info_ = intel_pch_info(expected_.pch[0]);
    by_cpu_ = true;
  } else if (info_.pch != IntelPch::Unknown && expected_.count && !expected_.contains(info_.pch)) {
    unusual_pairing_ = true;
  }

  // The SMBus host: an I/O BAR at 0x20 (SMBASE), I/O decoding on, and HST_EN in HOSTC. The firmware decides; a host it left off stays off.
  if (has_smbus) {
    smbus_.address = smbus.address;
    smbus_.flavor = SmbusHostFlavor::IntelI801;
    const uint32_t bar = pci.read32(smbus.address, kSmbusBar);
    const uint32_t command = pci.read32(smbus.address, kPciCommand);
    const uint8_t host_config = static_cast<uint8_t>(pci.read32(smbus.address, kSmbusHostConfig));
    if (bar == 0xFFFFFFFFu || !(bar & 1) || (bar & 0xFFE0) == 0 || (bar >> 16) != 0) {   // x86 I/O space is 64 KiB
      smbus_.reason = "SMBus has no I/O base";
    } else if (!(command & kCommandIoSpace)) {
      smbus_.reason = "SMBus I/O decoding is off";
    } else if (!(host_config & kHostConfigEnable)) {
      smbus_.reason = "SMBus host disabled by the firmware";
    } else {
      smbus_.usable = true;
      smbus_.io_base = static_cast<uint16_t>(bar & 0xFFE0);
      smbus_.reason = "";
    }
  }

  // BIOS flash protection: the SPI function (00:1F.5) where there is one, else the LPC bridge (00:1F.0).
  ChipsetFunctionInfo where;
  bool have_where = inventory_.find(ChipsetFunction::SpiFlash, &where);
  if (!have_where && inventory_.find(ChipsetFunction::LpcBridge, &where) && where.address.device == 0x1F) have_where = true;
  if (have_where) {
    const uint32_t value = pci.read32(where.address, kBiosControl);
    if (value != 0xFFFFFFFFu) bios_ = intel_bios_protection_decode(static_cast<uint8_t>(value));
  }

  // The management engine's interface (HECI1 is function 0 of device 0x16).
  for (uint32_t i = 0; i < inventory_.count(); ++i) {
    ChipsetFunctionInfo f;
    inventory_.at(i, &f);
    if (f.function != ChipsetFunction::ManagementEngine || f.address.device != 0x16 || f.address.function != 0) continue;
    const uint32_t status = pci.read32(f.address, kHeciFirmwareStatus);
    if (status == 0xFFFFFFFFu) break;
    me_.present = true;
    me_.hfsts1 = status;
    break;
  }
}

void IntelPchChipset::summarize(FixedString<256>& out) const {
  out.clear();
  out.append(info_.codename);
  if (info_.pch != IntelPch::Unknown) {
    out.append(" (");
    out.append(info_.products);
    out.append(")");
  }
  if (by_cpu_) out.append(", named from the CPU");
  if (unusual_pairing_) {
    out.append("; this CPU usually pairs with ");
    out.append(intel_pch_name(expected_.pch[0]));
  }
}

}  // namespace hardware
