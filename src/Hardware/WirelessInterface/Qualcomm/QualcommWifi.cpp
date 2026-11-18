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

#include "Hardware/WirelessInterface/Qualcomm/QualcommWifi.hpp"

namespace hardware {

UniquePointer<IWifiChip> make_qualcomm_wifi_chip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator) {
  if (model.vendor != WifiVendor::Qualcomm || model.bus != WifiBus::Pci) return UniquePointer<IWifiChip>();
  switch (model.family) {
    case WifiDriverFamily::Ath10k: return UniquePointer<IWifiChip>::make_as<ath::Ath10kChip>(allocator, model, bus, allocator);
    case WifiDriverFamily::Ath11k:
    case WifiDriverFamily::Ath12k: return UniquePointer<IWifiChip>::make_as<ath::Ath11kChip>(allocator, model, bus, allocator);
    default: return UniquePointer<IWifiChip>();
  }
}

}  // namespace hardware
