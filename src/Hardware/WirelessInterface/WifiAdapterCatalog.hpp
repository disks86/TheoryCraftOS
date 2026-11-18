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

// Looks a PCI function or USB device up in every vendor's table of Wi-Fi adapters.
#pragma once
#include <stdint.h>
#include "Hardware/WirelessInterface/WifiAdapterModel.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// The vendor tables (one per vendor folder: Intel/IntelWifiModels.cpp, Realtek/RealtekWifiModels.cpp, ...). Entries are unique per (bus, vendor_id, device_id).
ArrayView<const WifiAdapterModel> intel_wifi_models();
ArrayView<const WifiAdapterModel> realtek_wifi_models();
ArrayView<const WifiAdapterModel> mediatek_wifi_models();
ArrayView<const WifiAdapterModel> qualcomm_wifi_models();
ArrayView<const WifiAdapterModel> broadcom_wifi_models();

// The adapter on that bus with these IDs, or nullptr.
const WifiAdapterModel* wifi_find_adapter(WifiBus bus, uint16_t vendor_id, uint16_t device_id);
inline const WifiAdapterModel* wifi_find_pci_adapter(uint16_t vendor_id, uint16_t device_id) { return wifi_find_adapter(WifiBus::Pci, vendor_id, device_id); }
inline const WifiAdapterModel* wifi_find_usb_adapter(uint16_t vendor_id, uint16_t product_id) { return wifi_find_adapter(WifiBus::Usb, vendor_id, product_id); }
// How many adapters the tables hold, and the n-th (vendor by vendor, in table order); nullptr past the end.
uint32_t wifi_adapter_count();
const WifiAdapterModel* wifi_adapter_at(uint32_t index);

}  // namespace hardware
