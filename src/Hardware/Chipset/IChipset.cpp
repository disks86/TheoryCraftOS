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

#include "Hardware/Chipset/IChipset.hpp"

namespace hardware {

const char* chipset_function_name(ChipsetFunction function) {
  switch (function) {
    case ChipsetFunction::LpcBridge: return "LPC/eSPI";
    case ChipsetFunction::Smbus: return "SMBus";
    case ChipsetFunction::SpiFlash: return "SPI flash";
    case ChipsetFunction::PowerManagement: return "PMC";
    case ChipsetFunction::HdAudio: return "HD Audio";
    case ChipsetFunction::Usb: return "USB";
    case ChipsetFunction::Sata: return "SATA";
    case ChipsetFunction::ManagementEngine: return "ME";
    case ChipsetFunction::SecurityProcessor: return "PSP";
    case ChipsetFunction::Thermal: return "thermal";
    case ChipsetFunction::SerialIo: return "serial IO";
    case ChipsetFunction::Ethernet: return "Ethernet";
    case ChipsetFunction::Wireless: return "Wi-Fi";
    case ChipsetFunction::PciePort: return "PCIe port";
    case ChipsetFunction::Other: break;
  }
  return "other";
}

ChipsetFunction chipset_function_of(const PciDeviceInfo& d) {
  using F = ChipsetFunction;
  const uint16_t cls = static_cast<uint16_t>((d.class_code << 8) | d.subclass);
  switch (cls) {
    case 0x0601: return F::LpcBridge;
    case 0x0C05: return F::Smbus;
    case 0x0C03: return F::Usb;
    case 0x0403:
    case 0x0401: return F::HdAudio;
    case 0x0106:
    case 0x0104: return F::Sata;
    case 0x0604: return F::PciePort;
    case 0x0200: return F::Ethernet;
    case 0x0280: return F::Wireless;
    case 0x1180: return F::Thermal;
    case 0x1080: return F::SecurityProcessor;
    case 0x0580: return d.address.bus == 0 && d.address.device == 0x1F ? F::PowerManagement : F::Other;
    // Intel puts the SPI controller (1F.5) and the LPSS serial controllers in the same "other serial bus" class.
    case 0x0C80: return d.address.bus == 0 && d.address.device == 0x1F ? F::SpiFlash : F::SerialIo;
    case 0x0780: return F::ManagementEngine;   // HECI; the AMT serial port (07/00) stays Other
    default: return F::Other;
  }
}

bool ChipsetInventory::add(const PciDeviceInfo& device) {
  if (count_ >= kCapacity) return false;
  functions_[count_] = {chipset_function_of(device), device.address, device.vendor_id, device.device_id};
  ++count_;
  return true;
}

bool ChipsetInventory::at(uint32_t index, ChipsetFunctionInfo* out) const {
  if (index >= count_) return false;
  *out = functions_[index];
  return true;
}

bool ChipsetInventory::find(ChipsetFunction function, ChipsetFunctionInfo* out) const {
  for (uint32_t i = 0; i < count_; ++i) {
    if (functions_[i].function != function) continue;
    *out = functions_[i];
    return true;
  }
  return false;
}

uint32_t ChipsetInventory::count_of(ChipsetFunction function) const {
  uint32_t n = 0;
  for (uint32_t i = 0; i < count_; ++i)
    if (functions_[i].function == function) ++n;
  return n;
}

void ChipsetInventory::summarize(FixedString<256>& out) const {
  using F = ChipsetFunction;
  constexpr F kOrder[] = {F::LpcBridge, F::Smbus, F::SpiFlash, F::PowerManagement, F::Usb, F::Sata, F::HdAudio, F::ManagementEngine,
                          F::SecurityProcessor, F::Thermal, F::SerialIo, F::Ethernet, F::Wireless, F::PciePort};
  out.clear();
  for (F function : kOrder) {
    const uint32_t n = count_of(function);
    if (n == 0) continue;
    if (!out.empty()) out.append(", ");
    if (n > 1) {
      out.append_uint(n);
      out.push_back(' ');
    }
    out.append(chipset_function_name(function));
  }
}

}  // namespace hardware
