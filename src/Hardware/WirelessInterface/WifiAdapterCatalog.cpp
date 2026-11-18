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

#include "Hardware/WirelessInterface/WifiAdapterCatalog.hpp"

namespace hardware {

namespace {

using Table = ArrayView<const WifiAdapterModel> (*)();
constexpr Table kTables[] = {&intel_wifi_models, &realtek_wifi_models, &mediatek_wifi_models, &qualcomm_wifi_models, &broadcom_wifi_models};

}  // namespace

const char* wifi_vendor_name(WifiVendor vendor) {
  switch (vendor) {
    case WifiVendor::Intel: return "Intel";
    case WifiVendor::Realtek: return "Realtek";
    case WifiVendor::MediaTek: return "MediaTek";
    case WifiVendor::Qualcomm: return "Qualcomm";
    case WifiVendor::Broadcom: return "Broadcom";
  }
  return "?";
}

const char* wifi_bus_name(WifiBus bus) {
  switch (bus) {
    case WifiBus::Pci: return "PCIe";
    case WifiBus::Usb: return "USB";
    case WifiBus::Sdio: return "SDIO";
  }
  return "?";
}

const char* wifi_standard_name(WifiStandard standard) {
  switch (standard) {
    case WifiStandard::Wifi4: return "Wi-Fi 4 (802.11n)";
    case WifiStandard::Wifi5: return "Wi-Fi 5 (802.11ac)";
    case WifiStandard::Wifi6: return "Wi-Fi 6 (802.11ax)";
    case WifiStandard::Wifi6E: return "Wi-Fi 6E (802.11ax, 6 GHz)";
    case WifiStandard::Wifi7: return "Wi-Fi 7 (802.11be)";
  }
  return "?";
}

const char* wifi_driver_family_name(WifiDriverFamily family) {
  switch (family) {
    case WifiDriverFamily::IwlDvm: return "iwlwifi (dvm)";
    case WifiDriverFamily::IwlMvm7000: return "iwlwifi (mvm, 7000)";
    case WifiDriverFamily::IwlMvm8000: return "iwlwifi (mvm, 8000)";
    case WifiDriverFamily::IwlMvm9000: return "iwlwifi (mvm, 9000)";
    case WifiDriverFamily::IwlMvm22000: return "iwlwifi (mvm, 22000)";
    case WifiDriverFamily::IwlMvmAx210: return "iwlwifi (mvm, AX210)";
    case WifiDriverFamily::IwlMldBe200: return "iwlwifi (mld, BE200)";
    case WifiDriverFamily::Rtw88: return "rtw88";
    case WifiDriverFamily::Rtw89: return "rtw89";
    case WifiDriverFamily::Rtl8xxxu: return "rtl8xxxu";
    case WifiDriverFamily::Rtl8812au: return "rtl8812au";
    case WifiDriverFamily::Rtl8192ce: return "rtlwifi";
    case WifiDriverFamily::Mt7921: return "mt7921";
    case WifiDriverFamily::Mt7925: return "mt7925";
    case WifiDriverFamily::Mt76x2u: return "mt76x2u";
    case WifiDriverFamily::Mt76x0u: return "mt76x0u";
    case WifiDriverFamily::Mt7601u: return "mt7601u";
    case WifiDriverFamily::Mt7615: return "mt7615";
    case WifiDriverFamily::Ath9k: return "ath9k";
    case WifiDriverFamily::Ath9kHtc: return "ath9k_htc";
    case WifiDriverFamily::Ath10k: return "ath10k";
    case WifiDriverFamily::Ath11k: return "ath11k";
    case WifiDriverFamily::Ath12k: return "ath12k";
    case WifiDriverFamily::Brcmfmac: return "brcmfmac";
    case WifiDriverFamily::Brcmsmac: return "brcmsmac";
    case WifiDriverFamily::B43: return "b43";
    case WifiDriverFamily::BroadcomWl: return "wl (proprietary)";
  }
  return "?";
}

WifiMacSplit wifi_mac_split(WifiDriverFamily family) {
  switch (family) {
    case WifiDriverFamily::Brcmfmac:
    case WifiDriverFamily::BroadcomWl:
      return WifiMacSplit::FullMac;
    case WifiDriverFamily::IwlDvm:
    case WifiDriverFamily::IwlMvm7000:
    case WifiDriverFamily::IwlMvm8000:
    case WifiDriverFamily::IwlMvm9000:
    case WifiDriverFamily::IwlMvm22000:
    case WifiDriverFamily::IwlMvmAx210:
    case WifiDriverFamily::IwlMldBe200:
    case WifiDriverFamily::Ath10k:
    case WifiDriverFamily::Ath11k:
    case WifiDriverFamily::Ath12k:
    case WifiDriverFamily::Ath9kHtc:
    case WifiDriverFamily::Mt7921:
    case WifiDriverFamily::Mt7925:
    case WifiDriverFamily::Mt7615:
      return WifiMacSplit::Offload;
    default:
      return WifiMacSplit::SoftMac;
  }
}

WifiVendor wifi_driver_family_vendor(WifiDriverFamily family) {
  switch (family) {
    case WifiDriverFamily::IwlDvm:
    case WifiDriverFamily::IwlMvm7000:
    case WifiDriverFamily::IwlMvm8000:
    case WifiDriverFamily::IwlMvm9000:
    case WifiDriverFamily::IwlMvm22000:
    case WifiDriverFamily::IwlMvmAx210:
    case WifiDriverFamily::IwlMldBe200:
      return WifiVendor::Intel;
    case WifiDriverFamily::Rtw88:
    case WifiDriverFamily::Rtw89:
    case WifiDriverFamily::Rtl8xxxu:
    case WifiDriverFamily::Rtl8812au:
    case WifiDriverFamily::Rtl8192ce:
      return WifiVendor::Realtek;
    case WifiDriverFamily::Mt7921:
    case WifiDriverFamily::Mt7925:
    case WifiDriverFamily::Mt76x2u:
    case WifiDriverFamily::Mt76x0u:
    case WifiDriverFamily::Mt7601u:
    case WifiDriverFamily::Mt7615:
      return WifiVendor::MediaTek;
    case WifiDriverFamily::Ath9k:
    case WifiDriverFamily::Ath9kHtc:
    case WifiDriverFamily::Ath10k:
    case WifiDriverFamily::Ath11k:
    case WifiDriverFamily::Ath12k:
      return WifiVendor::Qualcomm;
    case WifiDriverFamily::Brcmfmac:
    case WifiDriverFamily::Brcmsmac:
    case WifiDriverFamily::B43:
    case WifiDriverFamily::BroadcomWl:
      return WifiVendor::Broadcom;
  }
  return WifiVendor::Intel;
}

const WifiAdapterModel* wifi_find_adapter(WifiBus bus, uint16_t vendor_id, uint16_t device_id) {
  for (Table table : kTables)
    for (const WifiAdapterModel& model : table())
      if (model.bus == bus && model.vendor_id == vendor_id && model.device_id == device_id) return &model;
  return nullptr;
}

uint32_t wifi_adapter_count() {
  uint64_t total = 0;
  for (Table table : kTables) total += table().size();
  return static_cast<uint32_t>(total);
}

const WifiAdapterModel* wifi_adapter_at(uint32_t index) {
  uint64_t at = index;
  for (Table table : kTables) {
    const ArrayView<const WifiAdapterModel> models = table();
    if (at < models.size()) return &models[at];
    at -= models.size();
  }
  return nullptr;
}

}  // namespace hardware
