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

// Intel's Wi-Fi adapters, after the PCI ID table of iwlwifi's pcie/drv.c. The 9000 series (IwlMvm9000) is run by the full driver in this folder (IwlWirelessInterface);
// the other generations by IntelWifiChip, which identifies the card and finds and checks its firmware. The CNVi IDs of Comet Lake, Ice Lake and Tiger Lake (02F0, 06F0,
// 34F0, 3DF0, 4DF0, 43F0, A0F0) carry either a Wireless-AC 9560/9462 (JF radio) or a Wi-Fi 6 AX201 (HR radio) companion module: the radio ID read from the card decides,
// so their entries name both and leave the firmware to the chip. Gen1 cards (7000/8000 series, older) name the newest firmware API in linux-firmware.
#include "Hardware/WirelessInterface/WifiAdapterCatalog.hpp"

namespace hardware {

namespace {

constexpr uint16_t kIntel = 0x8086;
using F = WifiDriverFamily;
using S = WifiStandard;

constexpr WifiAdapterModel model(uint16_t device, F family, S standard, uint8_t streams, bool bluetooth, const char* chip, const char* product, const char* firmware) {
  return WifiAdapterModel{WifiVendor::Intel, WifiBus::Pci, kIntel, device, family, standard, streams, bluetooth, chip, product, firmware};
}

constexpr WifiAdapterModel kModels[] = {
    // DVM: the older 802.11n cards (each one firmware file)
    model(0x4229, F::IwlDvm, S::Wifi4, 3, false, "4965", "Wireless WiFi Link 4965AGN", "iwlwifi-4965-2.ucode"),
    model(0x4230, F::IwlDvm, S::Wifi4, 3, false, "4965", "Wireless WiFi Link 4965AGN", "iwlwifi-4965-2.ucode"),
    model(0x4232, F::IwlDvm, S::Wifi4, 1, false, "5100", "WiFi Link 5100", "iwlwifi-5000-5.ucode"),
    model(0x4235, F::IwlDvm, S::Wifi4, 3, false, "5300", "Ultimate N WiFi Link 5300", "iwlwifi-5000-5.ucode"),
    model(0x4236, F::IwlDvm, S::Wifi4, 3, false, "5300", "Ultimate N WiFi Link 5300", "iwlwifi-5000-5.ucode"),
    model(0x4237, F::IwlDvm, S::Wifi4, 2, false, "5100", "WiFi Link 5100", "iwlwifi-5000-5.ucode"),
    model(0x423C, F::IwlDvm, S::Wifi4, 1, false, "5150", "WiMAX/WiFi Link 5150", "iwlwifi-5150-2.ucode"),
    model(0x422B, F::IwlDvm, S::Wifi4, 3, false, "6000", "Centrino Ultimate-N 6300", "iwlwifi-6000-4.ucode"),
    model(0x4238, F::IwlDvm, S::Wifi4, 3, false, "6000", "Centrino Ultimate-N 6300", "iwlwifi-6000-4.ucode"),
    model(0x0082, F::IwlDvm, S::Wifi4, 2, false, "6005", "Centrino Advanced-N 6205", "iwlwifi-6000g2a-6.ucode"),
    model(0x0085, F::IwlDvm, S::Wifi4, 2, false, "6005", "Centrino Advanced-N 6205", "iwlwifi-6000g2a-6.ucode"),
    model(0x0087, F::IwlDvm, S::Wifi4, 2, false, "6050", "Centrino Advanced-N + WiMAX 6250", "iwlwifi-6050-5.ucode"),
    model(0x0089, F::IwlDvm, S::Wifi4, 2, false, "6050", "Centrino Advanced-N + WiMAX 6250", "iwlwifi-6050-5.ucode"),
    model(0x0091, F::IwlDvm, S::Wifi4, 2, true, "6030", "Centrino Advanced-N 6230", "iwlwifi-6000g2b-6.ucode"),
    model(0x0090, F::IwlDvm, S::Wifi4, 2, true, "6030", "Centrino Advanced-N 6230", "iwlwifi-6000g2b-6.ucode"),
    model(0x088E, F::IwlDvm, S::Wifi4, 2, true, "6035", "Centrino Advanced-N 6235", "iwlwifi-6000g2b-6.ucode"),
    model(0x088F, F::IwlDvm, S::Wifi4, 2, true, "6035", "Centrino Advanced-N 6235", "iwlwifi-6000g2b-6.ucode"),
    model(0x0083, F::IwlDvm, S::Wifi4, 1, false, "1000", "Centrino Wireless-N 1000", "iwlwifi-1000-5.ucode"),
    model(0x0084, F::IwlDvm, S::Wifi4, 1, false, "1000", "Centrino Wireless-N 1000", "iwlwifi-1000-5.ucode"),
    model(0x0887, F::IwlDvm, S::Wifi4, 1, true, "2030", "Centrino Wireless-N 2230", "iwlwifi-2030-6.ucode"),
    model(0x0888, F::IwlDvm, S::Wifi4, 1, true, "2030", "Centrino Wireless-N 2230", "iwlwifi-2030-6.ucode"),
    model(0x0890, F::IwlDvm, S::Wifi4, 1, false, "2000", "Centrino Wireless-N 2200", "iwlwifi-2000-6.ucode"),
    model(0x0891, F::IwlDvm, S::Wifi4, 1, false, "2000", "Centrino Wireless-N 2200", "iwlwifi-2000-6.ucode"),
    model(0x0892, F::IwlDvm, S::Wifi4, 1, true, "135", "Centrino Wireless-N 135", "iwlwifi-135-6.ucode"),
    model(0x0893, F::IwlDvm, S::Wifi4, 1, true, "135", "Centrino Wireless-N 135", "iwlwifi-135-6.ucode"),
    model(0x0894, F::IwlDvm, S::Wifi4, 1, false, "105", "Centrino Wireless-N 105", "iwlwifi-105-6.ucode"),
    model(0x0895, F::IwlDvm, S::Wifi4, 1, false, "105", "Centrino Wireless-N 105", "iwlwifi-105-6.ucode"),
    model(0x08AE, F::IwlDvm, S::Wifi4, 1, false, "100", "Centrino Wireless-N 100", "iwlwifi-100-5.ucode"),
    model(0x08AF, F::IwlDvm, S::Wifi4, 1, false, "100", "Centrino Wireless-N 100", "iwlwifi-100-5.ucode"),
    // MVM, 7000 series
    model(0x08B1, F::IwlMvm7000, S::Wifi5, 2, true, "7260", "Wireless-AC 7260 / Wireless-N 7260", "iwlwifi-7260-17.ucode"),
    model(0x08B2, F::IwlMvm7000, S::Wifi5, 2, true, "7260", "Wireless-AC 7260 / Wireless-N 7260", "iwlwifi-7260-17.ucode"),
    model(0x08B3, F::IwlMvm7000, S::Wifi5, 1, true, "3160", "Dual Band Wireless-AC 3160", "iwlwifi-3160-17.ucode"),
    model(0x08B4, F::IwlMvm7000, S::Wifi5, 1, true, "3160", "Dual Band Wireless-AC 3160", "iwlwifi-3160-17.ucode"),
    model(0x095A, F::IwlMvm7000, S::Wifi5, 2, true, "7265", "Wireless-AC 7265 / Wireless-N 7265", ""),     // 7265 or 7265D by the hardware revision
    model(0x095B, F::IwlMvm7000, S::Wifi5, 2, true, "7265", "Wireless-AC 7265 / Wireless-N 7265", ""),
    model(0x3165, F::IwlMvm7000, S::Wifi5, 1, true, "3165", "Dual Band Wireless-AC 3165", "iwlwifi-7265D-29.ucode"),
    model(0x3166, F::IwlMvm7000, S::Wifi5, 1, true, "3165", "Dual Band Wireless-AC 3165", "iwlwifi-7265D-29.ucode"),
    model(0x24FB, F::IwlMvm7000, S::Wifi5, 1, true, "3168", "Dual Band Wireless-AC 3168", "iwlwifi-3168-29.ucode"),
    // MVM, 8000 series
    model(0x24F3, F::IwlMvm8000, S::Wifi5, 2, true, "8260", "Dual Band Wireless-AC 8260", "iwlwifi-8000C-36.ucode"),
    model(0x24F4, F::IwlMvm8000, S::Wifi5, 2, true, "8260", "Dual Band Wireless-AC 8260", "iwlwifi-8000C-36.ucode"),
    model(0x24F5, F::IwlMvm8000, S::Wifi5, 2, true, "4165", "Dual Band Wireless-AC 4165", "iwlwifi-8000C-36.ucode"),
    model(0x24F6, F::IwlMvm8000, S::Wifi5, 2, true, "4165", "Dual Band Wireless-AC 4165", "iwlwifi-8000C-36.ucode"),
    model(0x24FD, F::IwlMvm8000, S::Wifi5, 2, true, "8265", "Dual Band Wireless-AC 8265 / 8275", "iwlwifi-8265-36.ucode"),
    // MVM, 9000 series (run by IwlWirelessInterface; same IDs as IwlDevices.cpp)
    model(0x2526, F::IwlMvm9000, S::Wifi5, 2, true, "9260", "Wireless-AC 9260 / 9560 (PCIe)", ""),
    model(0x271B, F::IwlMvm9000, S::Wifi5, 2, true, "9260", "Wireless-AC 9000 series (PCIe)", ""),
    model(0x271C, F::IwlMvm9000, S::Wifi5, 2, true, "9260", "Wireless-AC 9000 series (PCIe)", ""),
    model(0x30DC, F::IwlMvm9000, S::Wifi5, 2, true, "9560", "Wireless-AC 9560 (CNVi, Gemini Lake)", ""),
    model(0x31DC, F::IwlMvm9000, S::Wifi5, 2, true, "9560", "Wireless-AC 9560 (CNVi, Gemini Lake)", ""),
    model(0x9DF0, F::IwlMvm9000, S::Wifi5, 2, true, "9560", "Wireless-AC 9560 (CNVi, Cannon Lake)", ""),
    model(0xA370, F::IwlMvm9000, S::Wifi5, 2, true, "9560", "Wireless-AC 9560 (CNVi, Cannon Lake-H)", ""),
    // 22000: Wi-Fi 6 (and the 9560/9462 modules on the newer CNVi MACs)
    model(0x2723, F::IwlMvm22000, S::Wifi6, 2, true, "AX200", "Wi-Fi 6 AX200 (Killer AX1650x)", "iwlwifi-cc-a0-77.ucode"),
    model(0x02F0, F::IwlMvm22000, S::Wifi6, 2, true, "AX201", "Wi-Fi 6 AX201 / Wireless-AC 9560 (CNVi, Comet Lake)", ""),
    model(0x06F0, F::IwlMvm22000, S::Wifi6, 2, true, "AX201", "Wi-Fi 6 AX201 / Wireless-AC 9560 (CNVi, Comet Lake-H)", ""),
    model(0x34F0, F::IwlMvm22000, S::Wifi6, 2, true, "AX201", "Wi-Fi 6 AX201 / Wireless-AC 9560 (CNVi, Ice Lake)", ""),
    model(0x3DF0, F::IwlMvm22000, S::Wifi6, 2, true, "AX201", "Wi-Fi 6 AX201 / Wireless-AC 9560 (CNVi)", ""),
    model(0x4DF0, F::IwlMvm22000, S::Wifi6, 2, true, "AX201", "Wi-Fi 6 AX201 / Wireless-AC 9560 (CNVi, Jasper Lake)", ""),
    model(0x43F0, F::IwlMvm22000, S::Wifi6, 2, true, "AX201", "Wi-Fi 6 AX201 / Wireless-AC 9560 (CNVi, Tiger Lake-H)", ""),
    model(0xA0F0, F::IwlMvm22000, S::Wifi6, 2, true, "AX201", "Wi-Fi 6 AX201 / Wireless-AC 9560 (CNVi, Tiger Lake)", ""),
    // AX210 family: Wi-Fi 6E
    model(0x2725, F::IwlMvmAx210, S::Wifi6E, 2, true, "AX210", "Wi-Fi 6E AX210 (Killer AX1675x)", ""),
    model(0x51F0, F::IwlMvmAx210, S::Wifi6E, 2, true, "AX211", "Wi-Fi 6E AX211 (CNVi, Alder Lake)", ""),
    model(0x51F1, F::IwlMvmAx210, S::Wifi6E, 2, true, "AX411", "Wi-Fi 6E AX411 (CNVi, Alder Lake)", ""),
    model(0x54F0, F::IwlMvmAx210, S::Wifi6E, 2, true, "AX211", "Wi-Fi 6E AX211 (CNVi, Alder Lake-N)", ""),
    model(0x7A70, F::IwlMvmAx210, S::Wifi6E, 2, true, "AX211", "Wi-Fi 6E AX211 (CNVi, Raptor Lake-S)", ""),
    model(0x7AF0, F::IwlMvmAx210, S::Wifi6E, 2, true, "AX211", "Wi-Fi 6E AX211 (CNVi, Alder Lake-S)", ""),
    model(0x7E40, F::IwlMvmAx210, S::Wifi6E, 2, true, "AX211", "Wi-Fi 6E AX211 (CNVi, Meteor Lake)", ""),
    // Wi-Fi 7
    model(0x272B, F::IwlMldBe200, S::Wifi7, 2, true, "BE200", "Wi-Fi 7 BE200 (Killer BE1750x)", ""),
    model(0xA840, F::IwlMldBe200, S::Wifi7, 2, true, "BE201", "Wi-Fi 7 BE201 / BE211 (CNVi, Lunar Lake)", ""),     // ID from drv.c's BZ table: unverified on hardware
    model(0x7740, F::IwlMldBe200, S::Wifi7, 2, true, "BE201", "Wi-Fi 7 BE201 (CNVi, Arrow Lake)", ""),             // ID from drv.c's BZ table: unverified on hardware
};

}  // namespace

ArrayView<const WifiAdapterModel> intel_wifi_models() { return ArrayView<const WifiAdapterModel>(kModels); }

}  // namespace hardware
