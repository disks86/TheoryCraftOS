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

// The MediaTek chip drivers by family: Mt7921Chip for MT7921/MT7922/MT7920 on PCIe; MtUsbChip (identification and firmware parsing) for the USB adapters of the
// MT7921, MT7925, MT76x2U, MT76x0U and MT7601U families. MT7925 on PCIe and MT7615/MT7663 have no chip driver yet.
#include "Hardware/WirelessInterface/MediaTek/Mt7921Chip.hpp"
#include "Hardware/WirelessInterface/MediaTek/MtUsb.hpp"
#include "Hardware/WirelessInterface/WifiChipFactory.hpp"

namespace hardware {

UniquePointer<IWifiChip> make_mediatek_wifi_chip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator) {
  if (model.vendor != WifiVendor::MediaTek) return UniquePointer<IWifiChip>();
  if (model.bus == WifiBus::Pci && model.family == WifiDriverFamily::Mt7921) return UniquePointer<IWifiChip>::make_as<Mt7921Chip>(allocator, model, bus, allocator);
  if (model.bus == WifiBus::Usb) {
    switch (model.family) {
      case WifiDriverFamily::Mt7921:
      case WifiDriverFamily::Mt7925:
      case WifiDriverFamily::Mt76x2u:
      case WifiDriverFamily::Mt76x0u:
      case WifiDriverFamily::Mt7601u:
        return UniquePointer<IWifiChip>::make_as<MtUsbChip>(allocator, model, bus, allocator);
      default: break;
    }
  }
  return UniquePointer<IWifiChip>();
}

}  // namespace hardware
