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

// The Wi-Fi adapters the system knows by their bus IDs: which vendor and chip family a PCI function or USB device is, which driver family runs it (named after the Linux
// driver it follows, so that the knowledge of the chip can be checked against it), what standard it speaks and which firmware it wants. Each vendor folder keeps its
// own table (Intel/, Realtek/, MediaTek/, Qualcomm/, Broadcom/); WifiAdapterCatalog.hpp looks across all of them.
#pragma once
#include <stdint.h>

namespace hardware {

enum class WifiVendor : uint8_t { Intel, Realtek, MediaTek, Qualcomm, Broadcom };
enum class WifiBus : uint8_t { Pci, Usb, Sdio };

// The newest IEEE 802.11 amendment the chip speaks (Wi-Fi 6E is 802.11ax with the 6 GHz band).
enum class WifiStandard : uint8_t { Wifi4 /* 802.11n */, Wifi5 /* 802.11ac */, Wifi6 /* 802.11ax */, Wifi6E, Wifi7 /* 802.11be */ };

// Where the 802.11 management (scanning, authentication, association, key handling) runs. SoftMac: the host builds the frames (Realtek, MediaTek, Qualcomm ath9k,
// Broadcom b43/brcmsmac). Offload: the host still drives the state machine, the firmware does the timing-critical part and sends many frames itself (Intel MVM,
// Qualcomm ath10k/11k/12k, MediaTek connac). FullMac: the firmware does all of it and the host only asks (Broadcom brcmfmac).
enum class WifiMacSplit : uint8_t { SoftMac, Offload, FullMac };

// The driver family, after the Linux driver whose knowledge of the chip it uses.
enum class WifiDriverFamily : uint8_t {
  IwlDvm,       // Intel Centrino / Wireless-N (1000-6000 series)
  IwlMvm7000,   // Intel Wireless-N/AC 3160, 7260, 7265, 3165, 3168
  IwlMvm8000,   // Intel Wireless-AC 8260, 8265, 8275
  IwlMvm9000,   // Intel Wireless-AC 9260, 9461, 9462, 9560 (driven by the Intel/ driver)
  IwlMvm22000,  // Intel Wi-Fi 6 AX200, AX201, AX101, AX203
  IwlMvmAx210,  // Intel Wi-Fi 6E AX210, AX211, AX411
  IwlMldBe200,  // Intel Wi-Fi 7 BE200, BE201, BE202
  Rtw88,        // Realtek RTL8723DE/8821CE/8822BE/8822CE (PCIe) and their USB and SDIO versions
  Rtw89,        // Realtek RTL8852AE/8852BE/8852CE/8851BE/8922AE (PCIe) and their USB versions
  Rtl8xxxu,     // Realtek RTL8188CU/8188EU/8192CU/8192EU/8188FU/8710BU (USB, 802.11n)
  Rtl8812au,    // Realtek RTL8812AU/8821AU/8811AU (USB, 802.11ac)
  Rtl8192ce,    // Realtek RTL8188CE/8192CE/8192EE/8723BE/8821AE (rtlwifi, PCIe)
  Mt7921,       // MediaTek MT7921/MT7922/MT7920 (AMD RZ608/RZ616) (connac2)
  Mt7925,       // MediaTek MT7925 (AMD RZ717) (connac3)
  Mt76x2u,      // MediaTek MT7612U/MT7632U (USB, 802.11ac)
  Mt76x0u,      // MediaTek MT7610U (USB, 802.11ac)
  Mt7601u,      // MediaTek/Ralink MT7601U (USB, 802.11n)
  Mt7615,       // MediaTek MT7615/MT7663 (PCIe/USB) (connac1)
  Ath9k,        // Qualcomm Atheros AR92xx/AR93xx/AR94xx/AR95xx/QCA95xx (PCIe, 802.11n)
  Ath9kHtc,     // Qualcomm Atheros AR9271/AR7010 (USB, 802.11n)
  Ath10k,       // Qualcomm Atheros QCA6174/QCA9377/QCA988x/QCA99x0 (802.11ac)
  Ath11k,       // Qualcomm QCA6390/WCN6855/QCN9074 (802.11ax)
  Ath12k,       // Qualcomm WCN7850/QCN9274 (802.11be)
  Brcmfmac,     // Broadcom/Cypress FullMAC BCM43xx/BCM4350-4387 (PCIe/SDIO/USB)
  Brcmsmac,     // Broadcom SoftMAC BCM4313/BCM43224/BCM43225 (PCIe)
  B43,          // Broadcom BCM4306-BCM4331 (legacy SoftMAC)
  BroadcomWl,   // Broadcom BCM4352/BCM4360 (only the proprietary "wl" driver exists; no open firmware interface)
};

struct WifiAdapterModel {
  WifiVendor vendor;
  WifiBus bus;
  uint16_t vendor_id;          // PCI vendor or USB idVendor (SDIO: the manufacturer code)
  uint16_t device_id;          // PCI device or USB idProduct (SDIO: the card id)
  WifiDriverFamily family;
  WifiStandard standard;
  uint8_t spatial_streams;     // the chip's streams (1 for 1x1, 2 for 2x2, ...)
  bool bluetooth;              // a combo chip with Bluetooth on the same module (driven by the Bluetooth stack)
  const char* chip;            // "RTL8822CE", "MT7921", "QCA6174", ...
  const char* product;         // what it is sold as / found in, for the hardware list
  const char* firmware;        // the main firmware file under /firmware ("" when it depends on a revision read from the chip; the chip driver then names it)
};

const char* wifi_vendor_name(WifiVendor vendor);
const char* wifi_bus_name(WifiBus bus);
const char* wifi_standard_name(WifiStandard standard);     // "Wi-Fi 6E (802.11ax)"
const char* wifi_driver_family_name(WifiDriverFamily family);  // "rtw88", "mt7921", "ath10k", ...
WifiMacSplit wifi_mac_split(WifiDriverFamily family);
WifiVendor wifi_driver_family_vendor(WifiDriverFamily family);

}  // namespace hardware
