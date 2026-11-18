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

// Makes the chip driver for an adapter of the catalogue: the vendor folders each have a make_<vendor>_wifi_chip, and this picks one by the model's vendor.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/WirelessInterface/IWifiChip.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// Null when the model's driver family has no chip driver here (or memory ran out). The bus must outlive the chip.
UniquePointer<IWifiChip> make_wifi_chip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator);

// The vendors' (Intel/IntelWifiChip.hpp, Realtek/RealtekWifi.hpp, MediaTek/MediaTekWifi.hpp, Qualcomm/QualcommWifi.hpp, Broadcom/BroadcomWifi.hpp).
UniquePointer<IWifiChip> make_intel_wifi_chip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator);
UniquePointer<IWifiChip> make_realtek_wifi_chip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator);
UniquePointer<IWifiChip> make_mediatek_wifi_chip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator);
UniquePointer<IWifiChip> make_qualcomm_wifi_chip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator);
UniquePointer<IWifiChip> make_broadcom_wifi_chip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator);

}  // namespace hardware
