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

#include "Hardware/Chipset/AMD/AmdFchChipset.hpp"

namespace hardware {

namespace {
constexpr uint16_t kAti = 0x1002, kAmd = 0x1022, kHygon = 0x1D94;
constexpr uint8_t kFchDevice = 0x14, kFirstDataFabricDevice = 0x18;
constexpr uint16_t kPciRevision = 0x08, kSb700SmbusBase = 0x90, kSb700HostConfig = 0xD0;
constexpr uint8_t kPmDecodeEn = 0x00, kPmSmbusBaseHigh = 0x01, kPmPortSelect = 0x02, kPmLegacySmbusLow = 0x2C, kPmLegacySmbusHigh = 0x2D;
constexpr uint8_t kDecodeEnSmbus = 1u << 4;

bool fch_function(const PciDeviceInfo& d) {
  return d.address.bus == 0 && d.address.device == kFchDevice && (d.vendor_id == kAmd || d.vendor_id == kAti || d.vendor_id == kHygon);
}

// The SoC's own I/O functions behind its internal bridges: USB, SATA, audio and the security processor (the "FCH" part that is not at 00:14).
bool soc_io_function(const PciDeviceInfo& d) {
  if (d.vendor_id != kAmd && d.vendor_id != kHygon) return false;
  const ChipsetFunction f = chipset_function_of(d);
  return f == ChipsetFunction::Usb || f == ChipsetFunction::Sata || f == ChipsetFunction::HdAudio || f == ChipsetFunction::SecurityProcessor;
}
}  // namespace

void AmdFchChipset::find_smbus(const IPciBus& pci, PciAddress address, IAmdPmRegisters* pm) {
  smbus_.address = address;
  smbus_.flavor = SmbusHostFlavor::AmdFch;
  uint16_t base = 0;
  bool enabled = false;
  switch (fch_.decode) {
    case AmdSmbusDecode::PciConfig: {
      base = static_cast<uint16_t>(pci.read32(address, kSb700SmbusBase) & 0xFFF0);
      enabled = (pci.read32(address, kSb700HostConfig) >> 16) & 1;   // SMBHSTCFG (0xD2) bit 0
      break;
    }
    case AmdSmbusDecode::PmLegacy:
    case AmdSmbusDecode::PmDecodeEn: {
      if (!pm) {
        smbus_.reason = "FCH power-management registers not reachable";
        return;
      }
      if (fch_.decode == AmdSmbusDecode::PmDecodeEn) {
        const uint8_t decode = pm->read8(kPmDecodeEn);
        const uint8_t high = pm->read8(kPmSmbusBaseHigh);
        if (decode == 0xFF && high == 0xFF) {   // nothing answered: the index ports are off on this FCH
          smbus_.reason = "FCH power-management registers read as 0xFF";
          return;
        }
        enabled = decode & kDecodeEnSmbus;
        base = static_cast<uint16_t>(high << 8);
        smbus_port_ = static_cast<uint8_t>((pm->read8(kPmPortSelect) >> 3) & 3);
      } else {
        const uint8_t low = pm->read8(kPmLegacySmbusLow);
        const uint8_t high = pm->read8(kPmLegacySmbusHigh);
        enabled = low & 1;
        base = static_cast<uint16_t>(((high << 8) | low) & 0xFFE0);
      }
      break;
    }
    case AmdSmbusDecode::None: smbus_.reason = "unknown FCH"; return;
  }
  if (!enabled) {
    smbus_.reason = "SMBus host disabled by the firmware";
  } else if (base == 0) {
    smbus_.reason = "SMBus has no I/O base";
  } else {
    smbus_.usable = true;
    smbus_.io_base = base;
    smbus_.reason = "";
  }
}

void AmdFchChipset::probe(const IPciBus& pci, IAmdPmRegisters* pm_ports, IAmdPmRegisters* pm_mmio) {
  fch_ = AmdFchInfo();
  fch_revision_ = 0;
  chipset_ = AmdChipsetInfo();
  chips_ = df_nodes_ = 0;
  smbus_port_ = 0xFF;
  pm_mmio_used_ = false;
  inventory_.clear();
  smbus_ = ChipsetSmbus();

  PciAddress smbus_address{};
  bool smbus_found = false;
  PciDeviceInfo d;
  for (uint32_t i = 0; pci.pci_device(i, &d); ++i) {
    if (d.address.bus == 0 && d.address.device >= kFirstDataFabricDevice && d.address.function == 0 && (d.vendor_id == kAmd || d.vendor_id == kHygon) &&
        d.class_code == 0x06 && d.subclass == 0x00)
      ++df_nodes_;
    const AmdChipsetInfo board = d.vendor_id == kAmd ? amd_chipset_of_device(d.device_id) : AmdChipsetInfo();
    if (board.chipset != AmdChipset::None) {
      if (chipset_.chipset == AmdChipset::None) chipset_ = board;
      if (board.chipset == chipset_.chipset && board.upstream_port_id == d.device_id) ++chips_;
      inventory_.add(d);
      continue;
    }
    if (fch_function(d)) {
      inventory_.add(d);
      if (chipset_function_of(d) == ChipsetFunction::Smbus && !smbus_found) {
        smbus_found = true;
        smbus_address = d.address;
        fch_revision_ = static_cast<uint8_t>(pci.read32(d.address, kPciRevision));
        fch_ = amd_fch_of_smbus(d.vendor_id, d.device_id, fch_revision_);
      }
      continue;
    }
    if (soc_io_function(d)) inventory_.add(d);
  }
  if (chipset_.chipset != AmdChipset::None && chips_ == 0) chips_ = 1;   // a chipset without a switch ID in the table (300/400 series)

  if (!smbus_found) return;
  IAmdPmRegisters* pm = pm_ports;
  if (fch_.pm_needs_mmio && pm_mmio) {
    pm = pm_mmio;
    pm_mmio_used_ = true;
  } else if (!pm) {
    pm = pm_mmio;
    pm_mmio_used_ = pm != nullptr;
  }
  find_smbus(pci, smbus_address, pm);
}

const char* AmdFchChipset::products() const {
  if (chipset_.chipset == AmdChipset::None) return fch_.products;
  return chips_ > 1 && chipset_.products_two_chips ? chipset_.products_two_chips : chipset_.products;
}

void AmdFchChipset::summarize(FixedString<256>& out) const {
  out.clear();
  if (chipset_.chipset != AmdChipset::None) {
    out.append(chipset_.codename);
    if (chips_ > 1) {
      out.append(" x");
      out.append_uint(chips_);
    }
    out.append(" (");
    out.append(products());
    out.append("), ");
  } else if (fch_.fch == AmdFch::Kerncz) {
    out.append("no chipset on the board (SoC only: a laptop, X300/A300, or a server); ");
  }
  out.append(fch_.codename);
  if (fch_.fch != AmdFch::Unknown) {
    out.append(" rev 0x");
    constexpr char kHex[] = "0123456789ABCDEF";
    out.push_back(kHex[fch_revision_ >> 4]);
    out.push_back(kHex[fch_revision_ & 0xF]);
  }
  if (df_nodes_) {
    out.append(", ");
    out.append_uint(df_nodes_);
    out.append(df_nodes_ == 1 ? " Data Fabric node" : " Data Fabric nodes");
  }
}

}  // namespace hardware
