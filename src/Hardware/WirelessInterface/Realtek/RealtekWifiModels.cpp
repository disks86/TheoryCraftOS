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

// The popular Realtek Wi-Fi adapters by their bus IDs. The IDs come from the Linux drivers' device tables (rtw88 rtw88xxe.c / rtw88xxu.c, rtw89 rtw88xxe.c / rtw88xxu.c,
// rtl8xxxu rtl8xxxu_core.c, rtlwifi rtl8xxxe/sw.c, and the out-of-tree rtl8812au driver, whose chips rtw88 now drives as well) with the OEM sticks most often found: TP-Link
// (2357), D-Link (2001), ASUS (0B05), Netgear (0846), Edimax (7392), Linksys (13B1). The firmware names are linux-firmware's; "" where the driver picks the file by a
// revision it reads from the chip (rtl8xxxu's 8192CU/8723AU/8710BU). IDs marked "unverified" were not checked against a current kernel's table.
#include "Hardware/WirelessInterface/WifiAdapterCatalog.hpp"

namespace hardware {

namespace {

constexpr WifiVendor R = WifiVendor::Realtek;
constexpr WifiBus Pci = WifiBus::Pci;
constexpr WifiBus Usb = WifiBus::Usb;
constexpr WifiDriverFamily Rtw88 = WifiDriverFamily::Rtw88;
constexpr WifiDriverFamily Rtw89 = WifiDriverFamily::Rtw89;
constexpr WifiDriverFamily Xxxu = WifiDriverFamily::Rtl8xxxu;
constexpr WifiDriverFamily Au = WifiDriverFamily::Rtl8812au;
constexpr WifiDriverFamily Rtlwifi = WifiDriverFamily::Rtl8192ce;
constexpr WifiStandard N = WifiStandard::Wifi4;
constexpr WifiStandard Ac = WifiStandard::Wifi5;
constexpr WifiStandard Ax = WifiStandard::Wifi6;
constexpr WifiStandard Ax6e = WifiStandard::Wifi6E;
constexpr WifiStandard Be = WifiStandard::Wifi7;

constexpr const char* kFw8822b = "rtw88/rtw8822b_fw.bin";
constexpr const char* kFw8822c = "rtw88/rtw8822c_fw.bin";
constexpr const char* kFw8821c = "rtw88/rtw8821c_fw.bin";
constexpr const char* kFw8723d = "rtw88/rtw8723d_fw.bin";
constexpr const char* kFw8812a = "rtw88/rtw8812a_fw.bin";
constexpr const char* kFw8821a = "rtw88/rtw8821a_fw.bin";
// rtw89 asks for the newest file format first ("-N"), then older ones down to the plain name; the chip driver walks down from these (the newest of
// linux-firmware 2025; rtw89 itself caps N per chip with its *_FW_FORMAT_MAX).
constexpr const char* kFw8852a = "rtw89/rtw8852a_fw-1.bin";
constexpr const char* kFw8852b = "rtw89/rtw8852b_fw-2.bin";
constexpr const char* kFw8852bt = "rtw89/rtw8852bt_fw-1.bin";
constexpr const char* kFw8852c = "rtw89/rtw8852c_fw-2.bin";
constexpr const char* kFw8851b = "rtw89/rtw8851b_fw-1.bin";
constexpr const char* kFw8922a = "rtw89/rtw8922a_fw-4.bin";

constexpr WifiAdapterModel kModels[] = {
    // --- rtw88, PCIe
    {R, Pci, 0x10EC, 0xB822, Rtw88, Ac, 2, true, "RTL8822BE", "Realtek RTL8822BE 802.11ac 2x2 (M.2)", kFw8822b},
    {R, Pci, 0x10EC, 0xC822, Rtw88, Ac, 2, true, "RTL8822CE", "Realtek RTL8822CE 802.11ac 2x2 (M.2)", kFw8822c},
    {R, Pci, 0x10EC, 0xC82F, Rtw88, Ac, 2, true, "RTL8822CE", "Realtek RTL8822CE 802.11ac 2x2 (M.2)", kFw8822c},
    {R, Pci, 0x10EC, 0xC821, Rtw88, Ac, 1, true, "RTL8821CE", "Realtek RTL8821CE 802.11ac 1x1 (M.2)", kFw8821c},
    {R, Pci, 0x10EC, 0xB821, Rtw88, Ac, 1, true, "RTL8821CE", "Realtek RTL8821CE 802.11ac 1x1 (M.2)", kFw8821c},
    {R, Pci, 0x10EC, 0xC82B, Rtw88, Ac, 1, true, "RTL8821CE", "Realtek RTL8821CE 802.11ac 1x1 (M.2)", kFw8821c},
    {R, Pci, 0x10EC, 0xD723, Rtw88, N, 1, true, "RTL8723DE", "Realtek RTL8723DE 802.11n 1x1 (M.2)", kFw8723d},

    // --- rtw89, PCIe
    {R, Pci, 0x10EC, 0x8852, Rtw89, Ax, 2, true, "RTL8852AE", "Realtek RTL8852AE Wi-Fi 6 2x2 (M.2)", kFw8852a},
    {R, Pci, 0x10EC, 0xA85A, Rtw89, Ax, 2, true, "RTL8852AE", "Realtek RTL8852AE Wi-Fi 6 2x2 (M.2)", kFw8852a},
    {R, Pci, 0x10EC, 0xB852, Rtw89, Ax, 2, true, "RTL8852BE", "Realtek RTL8852BE Wi-Fi 6 2x2 (M.2)", kFw8852b},
    {R, Pci, 0x10EC, 0xB85B, Rtw89, Ax, 2, true, "RTL8852BE", "Realtek RTL8852BE Wi-Fi 6 2x2 (M.2)", kFw8852b},
    {R, Pci, 0x10EC, 0x8520, Rtw89, Ax, 2, true, "RTL8852BTE", "Realtek RTL8852BE-VT Wi-Fi 6 2x2 (M.2)", kFw8852bt},  // unverified
    {R, Pci, 0x10EC, 0xC852, Rtw89, Ax6e, 2, true, "RTL8852CE", "Realtek RTL8852CE Wi-Fi 6E 2x2 (M.2)", kFw8852c},
    {R, Pci, 0x10EC, 0xB851, Rtw89, Ax, 1, true, "RTL8851BE", "Realtek RTL8851BE Wi-Fi 6 1x1 (M.2)", kFw8851b},
    {R, Pci, 0x10EC, 0x8922, Rtw89, Be, 2, true, "RTL8922AE", "Realtek RTL8922AE Wi-Fi 7 2x2 (M.2)", kFw8922a},
    {R, Pci, 0x10EC, 0x892A, Rtw89, Be, 2, true, "RTL8922AE", "Realtek RTL8922AE Wi-Fi 7 2x2 (M.2)", kFw8922a},     // unverified

    // --- rtlwifi, PCIe (802.11n and the first 802.11ac parts)
    {R, Pci, 0x10EC, 0x8176, Rtlwifi, N, 1, false, "RTL8188CE", "Realtek RTL8188CE 802.11n 1x1 (Mini PCIe)", "rtlwifi/rtl8192cfw.bin"},
    {R, Pci, 0x10EC, 0x8178, Rtlwifi, N, 2, false, "RTL8192CE", "Realtek RTL8192CE 802.11n 2x2 (Mini PCIe)", "rtlwifi/rtl8192cfw.bin"},
    {R, Pci, 0x10EC, 0x8179, Rtlwifi, N, 1, false, "RTL8188EE", "Realtek RTL8188EE 802.11n 1x1 (Mini PCIe)", "rtlwifi/rtl8188efw.bin"},
    {R, Pci, 0x10EC, 0x818B, Rtlwifi, N, 2, false, "RTL8192EE", "Realtek RTL8192EE 802.11n 2x2 (Mini PCIe)", "rtlwifi/rtl8192eefw.bin"},
    {R, Pci, 0x10EC, 0x8723, Rtlwifi, N, 1, true, "RTL8723AE", "Realtek RTL8723AE 802.11n 1x1 + Bluetooth (Mini PCIe)", "rtlwifi/rtl8723efw.bin"},
    {R, Pci, 0x10EC, 0xB723, Rtlwifi, N, 1, true, "RTL8723BE", "Realtek RTL8723BE 802.11n 1x1 + Bluetooth (M.2 / Mini PCIe)", "rtlwifi/rtl8723befw_36.bin"},
    {R, Pci, 0x10EC, 0x8821, Rtlwifi, Ac, 1, true, "RTL8821AE", "Realtek RTL8821AE 802.11ac 1x1 + Bluetooth (M.2)", "rtlwifi/rtl8821aefw_29.bin"},
    {R, Pci, 0x10EC, 0x8812, Rtlwifi, Ac, 2, false, "RTL8812AE", "Realtek RTL8812AE 802.11ac 2x2 (Mini PCIe)", "rtlwifi/rtl8812aefw.bin"},

    // --- rtw88, USB: RTL8822BU
    {R, Usb, 0x0BDA, 0xB812, Rtw88, Ac, 2, false, "RTL8822BU", "Realtek RTL8822BU 802.11ac 2x2 USB", kFw8822b},
    {R, Usb, 0x0BDA, 0xB82C, Rtw88, Ac, 2, false, "RTL8822BU", "Realtek RTL8822BU 802.11ac 2x2 USB", kFw8822b},
    {R, Usb, 0x2357, 0x0115, Rtw88, Ac, 2, false, "RTL8822BU", "TP-Link Archer T4U v3", kFw8822b},
    {R, Usb, 0x2357, 0x012D, Rtw88, Ac, 2, false, "RTL8822BU", "TP-Link Archer T3U", kFw8822b},
    {R, Usb, 0x2357, 0x0138, Rtw88, Ac, 2, false, "RTL8822BU", "TP-Link Archer T3U Plus", kFw8822b},
    {R, Usb, 0x2001, 0x331E, Rtw88, Ac, 2, false, "RTL8822BU", "D-Link DWA-181 rev A1", kFw8822b},
    {R, Usb, 0x2001, 0x331C, Rtw88, Ac, 2, false, "RTL8822BU", "D-Link DWA-182 rev D1", kFw8822b},
    {R, Usb, 0x0B05, 0x184C, Rtw88, Ac, 2, false, "RTL8822BU", "ASUS USB-AC53 Nano", kFw8822b},
    {R, Usb, 0x0B05, 0x1841, Rtw88, Ac, 2, false, "RTL8822BU", "ASUS USB-AC55 B1", kFw8822b},
    {R, Usb, 0x0846, 0x9055, Rtw88, Ac, 2, false, "RTL8822BU", "Netgear A6150", kFw8822b},                 // unverified
    {R, Usb, 0x13B1, 0x0043, Rtw88, Ac, 2, false, "RTL8822BU", "Linksys WUSB6400M", kFw8822b},
    {R, Usb, 0x7392, 0xC822, Rtw88, Ac, 2, false, "RTL8822BU", "Edimax EW-7822UTC", kFw8822b},
    // RTL8821CU
    {R, Usb, 0x0BDA, 0xC811, Rtw88, Ac, 1, true, "RTL8821CU", "Realtek RTL8811CU / RTL8821CU 802.11ac 1x1 USB", kFw8821c},
    {R, Usb, 0x0BDA, 0xC820, Rtw88, Ac, 1, true, "RTL8821CU", "Realtek RTL8821CU 802.11ac 1x1 USB", kFw8821c},
    {R, Usb, 0x0BDA, 0xC821, Rtw88, Ac, 1, true, "RTL8821CU", "Realtek RTL8821CU 802.11ac 1x1 USB", kFw8821c},
    {R, Usb, 0x0BDA, 0xC82A, Rtw88, Ac, 1, true, "RTL8821CU", "Realtek RTL8821CU 802.11ac 1x1 USB", kFw8821c},
    {R, Usb, 0x0BDA, 0xC82B, Rtw88, Ac, 1, true, "RTL8821CU", "Realtek RTL8821CU 802.11ac 1x1 USB", kFw8821c},
    {R, Usb, 0x0BDA, 0x8811, Rtw88, Ac, 1, false, "RTL8811CU", "Realtek RTL8811CU 802.11ac 1x1 USB", kFw8821c},
    {R, Usb, 0x2001, 0x331D, Rtw88, Ac, 1, false, "RTL8821CU", "D-Link DWA-171 rev D1", kFw8821c},
    {R, Usb, 0x7392, 0xC811, Rtw88, Ac, 1, false, "RTL8811CU", "Edimax EW-7811ULC", kFw8821c},
    {R, Usb, 0x2357, 0x011F, Rtw88, Ac, 1, false, "RTL8811CU", "TP-Link Archer T2U Nano v2", kFw8821c},   // unverified
    // RTL8822CU, RTL8723DU
    {R, Usb, 0x0BDA, 0xC82C, Rtw88, Ac, 2, true, "RTL8822CU", "Realtek RTL8822CU 802.11ac 2x2 USB", kFw8822c},
    {R, Usb, 0x0BDA, 0xC812, Rtw88, Ac, 2, false, "RTL8822CU", "Realtek RTL8812CU 802.11ac 2x2 USB", kFw8822c},  // unverified
    {R, Usb, 0x0BDA, 0xD723, Rtw88, N, 1, true, "RTL8723DU", "Realtek RTL8723DU 802.11n 1x1 + Bluetooth USB", kFw8723d},
    {R, Usb, 0x7392, 0xD611, Rtw88, N, 1, true, "RTL8723DU", "Edimax EW-7611ULB V2", kFw8723d},

    // --- rtw89, USB
    {R, Usb, 0x0BDA, 0xB832, Rtw89, Ax, 2, false, "RTL8832BU", "Realtek RTL8832BU Wi-Fi 6 2x2 USB", kFw8852b},
    {R, Usb, 0x0BDA, 0xB83A, Rtw89, Ax, 2, false, "RTL8832BU", "Realtek RTL8832BU Wi-Fi 6 2x2 USB", kFw8852b},
    {R, Usb, 0x0BDA, 0xB852, Rtw89, Ax, 2, false, "RTL8852BU", "Realtek RTL8852BU Wi-Fi 6 2x2 USB", kFw8852b},
    {R, Usb, 0x0BDA, 0xB85A, Rtw89, Ax, 2, false, "RTL8852BU", "Realtek RTL8852BU Wi-Fi 6 2x2 USB", kFw8852b},
    {R, Usb, 0x0B05, 0x1A62, Rtw89, Ax, 2, false, "RTL8832BU", "ASUS USB-AX56 (rev B)", kFw8852b},          // unverified
    {R, Usb, 0x2357, 0x013F, Rtw89, Ax, 2, false, "RTL8832BU", "TP-Link Archer TX20U Nano", kFw8852b},      // unverified
    {R, Usb, 0x0BDA, 0xB851, Rtw89, Ax, 1, false, "RTL8851BU", "Realtek RTL8851BU Wi-Fi 6 1x1 USB", kFw8851b},
    {R, Usb, 0x3625, 0x010B, Rtw89, Ax, 1, false, "RTL8851BU", "TP-Link Archer TX10UB Nano", kFw8851b},     // unverified
    {R, Usb, 0x0BDA, 0x8832, Rtw89, Ax, 2, false, "RTL8832AU", "Realtek RTL8832AU Wi-Fi 6 2x2 USB", kFw8852a},
    {R, Usb, 0x0BDA, 0x885A, Rtw89, Ax, 2, false, "RTL8852AU", "Realtek RTL8852AU Wi-Fi 6 2x2 USB", kFw8852a},
    {R, Usb, 0x0BDA, 0x885C, Rtw89, Ax, 2, false, "RTL8852AU", "Realtek RTL8852AU Wi-Fi 6 2x2 USB", kFw8852a},
    {R, Usb, 0x0BDA, 0xC832, Rtw89, Ax6e, 2, false, "RTL8832CU", "Realtek RTL8832CU Wi-Fi 6E 2x2 USB", kFw8852c},
    {R, Usb, 0x0BDA, 0xC85A, Rtw89, Ax6e, 2, false, "RTL8852CU", "Realtek RTL8852CU Wi-Fi 6E 2x2 USB", kFw8852c},

    // --- rtl8xxxu, USB 802.11n
    {R, Usb, 0x0BDA, 0x8179, Xxxu, N, 1, false, "RTL8188EU", "Realtek RTL8188EUS 802.11n 1x1 USB", "rtlwifi/rtl8188eufw.bin"},
    {R, Usb, 0x0BDA, 0x0179, Xxxu, N, 1, false, "RTL8188EU", "Realtek RTL8188ETV 802.11n 1x1 USB", "rtlwifi/rtl8188eufw.bin"},
    {R, Usb, 0x2357, 0x010C, Xxxu, N, 1, false, "RTL8188EU", "TP-Link TL-WN722N v2/v3", "rtlwifi/rtl8188eufw.bin"},
    {R, Usb, 0x2357, 0x0111, Xxxu, N, 1, false, "RTL8188EU", "TP-Link TL-WN727N v5", "rtlwifi/rtl8188eufw.bin"},
    {R, Usb, 0x2001, 0x3311, Xxxu, N, 1, false, "RTL8188EU", "D-Link DWA-125 rev D1", "rtlwifi/rtl8188eufw.bin"},
    {R, Usb, 0x0BDA, 0x818B, Xxxu, N, 2, false, "RTL8192EU", "Realtek RTL8192EU 802.11n 2x2 USB", "rtlwifi/rtl8192eu_nic.bin"},
    {R, Usb, 0x2357, 0x0109, Xxxu, N, 2, false, "RTL8192EU", "TP-Link TL-WN823N v2/v3", "rtlwifi/rtl8192eu_nic.bin"},
    {R, Usb, 0x2357, 0x0107, Xxxu, N, 2, false, "RTL8192EU", "TP-Link TL-WN821N v5", "rtlwifi/rtl8192eu_nic.bin"},
    {R, Usb, 0x2001, 0x3319, Xxxu, N, 2, false, "RTL8192EU", "D-Link DWA-131 rev E1", "rtlwifi/rtl8192eu_nic.bin"},
    {R, Usb, 0x0BDA, 0xF179, Xxxu, N, 1, false, "RTL8188FU", "Realtek RTL8188FTV 802.11n 1x1 USB", "rtlwifi/rtl8188fufw.bin"},
    {R, Usb, 0x0BDA, 0xF192, Xxxu, N, 2, false, "RTL8192FU", "Realtek RTL8192FU 802.11n 2x2 USB", "rtlwifi/rtl8192fufw.bin"},
    {R, Usb, 0x0BDA, 0xB720, Xxxu, N, 1, true, "RTL8723BU", "Realtek RTL8723BU 802.11n 1x1 + Bluetooth USB", "rtlwifi/rtl8723bu_nic.bin"},
    {R, Usb, 0x0BDA, 0xB711, Xxxu, N, 1, false, "RTL8710BU", "Realtek RTL8188GU / RTL8710BU 802.11n 1x1 USB", ""},
    {R, Usb, 0x0BDA, 0x8176, Xxxu, N, 1, false, "RTL8188CUS", "Realtek RTL8188CUS 802.11n 1x1 USB", ""},
    {R, Usb, 0x0BDA, 0x8178, Xxxu, N, 2, false, "RTL8192CU", "Realtek RTL8192CU 802.11n 2x2 USB", ""},
    {R, Usb, 0x7392, 0x7811, Xxxu, N, 1, false, "RTL8188CUS", "Edimax EW-7811Un", ""},
    {R, Usb, 0x0846, 0x9041, Xxxu, N, 1, false, "RTL8188CUS", "Netgear WNA1000M", ""},
    {R, Usb, 0x0BDA, 0x8724, Xxxu, N, 1, true, "RTL8723AU", "Realtek RTL8723AU 802.11n 1x1 + Bluetooth USB", ""},

    // --- RTL8812AU / RTL8821AU / RTL8811AU (the out-of-tree rtl8812au driver; rtw88 drives them since Linux 6.13)
    {R, Usb, 0x0BDA, 0x8812, Au, Ac, 2, false, "RTL8812AU", "Realtek RTL8812AU 802.11ac 2x2 USB", kFw8812a},
    {R, Usb, 0x0BDA, 0x881A, Au, Ac, 2, false, "RTL8812AU", "Realtek RTL8812AU 802.11ac 2x2 USB", kFw8812a},
    {R, Usb, 0x0BDA, 0x0811, Au, Ac, 1, false, "RTL8811AU", "Realtek RTL8811AU 802.11ac 1x1 USB", kFw8821a},
    {R, Usb, 0x0BDA, 0xA811, Au, Ac, 1, false, "RTL8811AU", "Realtek RTL8811AU 802.11ac 1x1 USB", kFw8821a},
    {R, Usb, 0x0BDA, 0x0820, Au, Ac, 1, true, "RTL8821AU", "Realtek RTL8821AU 802.11ac 1x1 + Bluetooth USB", kFw8821a},
    {R, Usb, 0x0BDA, 0x0821, Au, Ac, 1, true, "RTL8821AU", "Realtek RTL8821AU 802.11ac 1x1 + Bluetooth USB", kFw8821a},
    {R, Usb, 0x2357, 0x0101, Au, Ac, 2, false, "RTL8812AU", "TP-Link Archer T4U v1", kFw8812a},
    {R, Usb, 0x2357, 0x0103, Au, Ac, 2, false, "RTL8812AU", "TP-Link Archer T4UH v1", kFw8812a},
    {R, Usb, 0x2357, 0x010D, Au, Ac, 2, false, "RTL8812AU", "TP-Link Archer T4U v2", kFw8812a},
    {R, Usb, 0x2357, 0x011E, Au, Ac, 1, false, "RTL8811AU", "TP-Link Archer T2U Nano", kFw8821a},
    {R, Usb, 0x2357, 0x0120, Au, Ac, 1, false, "RTL8811AU", "TP-Link Archer T2U Plus", kFw8821a},
    {R, Usb, 0x2001, 0x3313, Au, Ac, 2, false, "RTL8812AU", "D-Link DWA-182 rev C1", kFw8812a},
    {R, Usb, 0x2001, 0x3314, Au, Ac, 1, false, "RTL8811AU", "D-Link DWA-171 rev C1", kFw8821a},
    {R, Usb, 0x0846, 0x9052, Au, Ac, 1, false, "RTL8811AU", "Netgear A6100", kFw8821a},
    {R, Usb, 0x0B05, 0x17D2, Au, Ac, 2, false, "RTL8812AU", "ASUS USB-AC56", kFw8812a},
    {R, Usb, 0x7392, 0xA812, Au, Ac, 2, false, "RTL8812AU", "Edimax EW-7822UAC", kFw8812a},
};

}  // namespace

ArrayView<const WifiAdapterModel> realtek_wifi_models() { return ArrayView<const WifiAdapterModel>(kModels); }

}  // namespace hardware
