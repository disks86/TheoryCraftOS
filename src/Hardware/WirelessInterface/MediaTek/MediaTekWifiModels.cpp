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

// The MediaTek Wi-Fi adapters, after the device tables of Linux mt76: mt7921/pci.c and mt7921/usb.c (MT7921, MT7922, MT7920), mt7925/pci.c and mt7925/usb.c (MT7925),
// mt7615/pci.c (MT7615, MT7663), mt76x2/usb.c (MT7612U), mt76x0/usb.c (MT7610U) and mt7601u/usb.c. The firmware is the RAM code file the Linux driver names for the
// device (the ROM patch, and the chip's own choice between MT7921/MT7922/MT7920 files, follow from MT_HW_CHIPID; see mt792x_firmware_names).
//
// Only IDs taken from those tables are listed; products are what the Linux table comments say. A USB ID that two Linux drivers claim (7392:b711 is in both the mt76x2u
// and the mt76x0u tables, told apart by the chip id at probe) is listed once, with the driver of the more common product. Spatial streams of MT7920 and the USB
// products' Bluetooth are not verified.
#include "Hardware/WirelessInterface/WifiAdapterCatalog.hpp"

namespace hardware {

namespace {

constexpr WifiVendor M = WifiVendor::MediaTek;
constexpr WifiBus P = WifiBus::Pci, U = WifiBus::Usb;
constexpr WifiDriverFamily F7921 = WifiDriverFamily::Mt7921, F7925 = WifiDriverFamily::Mt7925, F7615 = WifiDriverFamily::Mt7615, F76x2u = WifiDriverFamily::Mt76x2u,
                           F76x0u = WifiDriverFamily::Mt76x0u, F7601u = WifiDriverFamily::Mt7601u;
constexpr WifiStandard N = WifiStandard::Wifi4, AC = WifiStandard::Wifi5, AX = WifiStandard::Wifi6, AXE = WifiStandard::Wifi6E, BE = WifiStandard::Wifi7;

constexpr const char* kMt7921Ram = "mediatek/WIFI_RAM_CODE_MT7961_1.bin";
constexpr const char* kMt7922Ram = "mediatek/WIFI_RAM_CODE_MT7922_1.bin";
constexpr const char* kMt7920Ram = "mediatek/WIFI_RAM_CODE_MT7961_1a.bin";
constexpr const char* kMt7925Ram = "mediatek/mt7925/WIFI_RAM_CODE_MT7925_1_1.bin";
constexpr const char* kMt7615N9 = "mediatek/mt7615_n9.bin";
constexpr const char* kMt7663N9 = "mediatek/mt7663_n9_v3.bin";     // the offload firmware (MT7663_OFFLOAD_FIRMWARE_N9)
constexpr const char* kMt7662u = "mediatek/mt7662u.bin";            // with mediatek/mt7662u_rom_patch.bin
constexpr const char* kMt7610u = "mediatek/mt7610u.bin";
constexpr const char* kMt7601u = "mt7601u.bin";

constexpr WifiAdapterModel kModels[] = {
    // --- PCIe, connac2: mt7921e
    {M, P, 0x14C3, 0x7961, F7921, AX, 2, true, "MT7921", "MediaTek MT7921 Wi-Fi 6", kMt7921Ram},
    {M, P, 0x14C3, 0x0608, F7921, AXE, 2, true, "MT7921K", "AMD RZ608 Wi-Fi 6E (MT7921K)", kMt7921Ram},
    {M, P, 0x14C3, 0x0616, F7921, AXE, 2, true, "MT7922", "AMD RZ616 Wi-Fi 6E (MT7922)", kMt7922Ram},
    {M, P, 0x14C3, 0x7922, F7921, AXE, 2, true, "MT7922", "MediaTek MT7922 Wi-Fi 6E", kMt7922Ram},
    {M, P, 0x14C3, 0x7920, F7921, AX, 2, true, "MT7920", "MediaTek MT7920 Wi-Fi 6", kMt7920Ram},
    // --- PCIe, connac3: mt7925e
    {M, P, 0x14C3, 0x7925, F7925, BE, 2, true, "MT7925", "MediaTek MT7925 Wi-Fi 7", kMt7925Ram},
    {M, P, 0x14C3, 0x0717, F7925, BE, 2, true, "MT7925", "AMD RZ717 Wi-Fi 7 (MT7925)", kMt7925Ram},
    // --- PCIe, connac1: mt7615e
    {M, P, 0x14C3, 0x7615, F7615, AC, 4, false, "MT7615", "MediaTek MT7615 Wi-Fi 5 (4x4)", kMt7615N9},
    {M, P, 0x14C3, 0x7663, F7615, AC, 2, true, "MT7663", "MediaTek MT7663 Wi-Fi 5", kMt7663N9},

    // --- USB, connac2: mt7921u
    {M, U, 0x0E8D, 0x7961, F7921, AXE, 2, true, "MT7921AU", "MediaTek MT7921AU USB Wi-Fi 6E", kMt7921Ram},
    {M, U, 0x3574, 0x6211, F7921, AXE, 2, false, "MT7921AU", "Comfast CF-952AX", kMt7921Ram},
    {M, U, 0x0846, 0x9060, F7921, AXE, 2, false, "MT7921AU", "Netgear A8000 (AXE3000)", kMt7921Ram},
    {M, U, 0x35BC, 0x0107, F7921, AXE, 2, false, "MT7921AU", "TP-Link TXE50UH", kMt7921Ram},
    // --- USB, connac3: mt7925u
    {M, U, 0x0E8D, 0x7925, F7925, BE, 2, false, "MT7925U", "MediaTek MT7925 USB Wi-Fi 7", kMt7925Ram},

    // --- USB, mt76x2u (802.11ac 2x2)
    {M, U, 0x0E8D, 0x7612, F76x2u, AC, 2, false, "MT7612U", "MT7612U (Alfa AWUS036ACM, Aukey USB-AC1200)", kMt7662u},
    {M, U, 0x0B05, 0x1833, F76x2u, AC, 2, false, "MT7612U", "ASUS USB-AC54", kMt7662u},
    {M, U, 0x0B05, 0x17EB, F76x2u, AC, 2, false, "MT7612U", "ASUS USB-AC55", kMt7662u},
    {M, U, 0x0B05, 0x180B, F76x2u, AC, 2, false, "MT7612U", "ASUS USB-N53 B1", kMt7662u},
    {M, U, 0x057C, 0x8503, F76x2u, AC, 2, false, "MT7612U", "AVM FRITZ!WLAN AC 860", kMt7662u},
    {M, U, 0x7392, 0xB711, F76x2u, AC, 2, false, "MT7612U", "Edimax EW-7722UAC", kMt7662u},
    {M, U, 0x2C4E, 0x0103, F76x2u, AC, 2, false, "MT7612U", "Mercury UD13", kMt7662u},
    {M, U, 0x0846, 0x9053, F76x2u, AC, 2, false, "MT7612U", "Netgear A6210", kMt7662u},
    {M, U, 0x2357, 0x0137, F76x2u, AC, 2, false, "MT7612U", "TP-Link TL-WDN6200", kMt7662u},

    // --- USB, mt76x0u (802.11ac 1x1)
    {M, U, 0x148F, 0x7610, F76x0u, AC, 1, false, "MT7610U", "MediaTek MT7610U", kMt7610u},
    {M, U, 0x0E8D, 0x7610, F76x0u, AC, 1, false, "MT7610U", "Sabrent NTWLAC", kMt7610u},
    {M, U, 0x13B1, 0x003E, F76x0u, AC, 1, false, "MT7610U", "Linksys AE6000", kMt7610u},
    {M, U, 0x7392, 0xA711, F76x0u, AC, 1, false, "MT7610U", "Edimax EW-7711MAC", kMt7610u},
    {M, U, 0x148F, 0x761A, F76x0u, AC, 1, false, "MT7610U", "TP-Link TL-WDN5200", kMt7610u},
    {M, U, 0x0B05, 0x17D1, F76x0u, AC, 1, false, "MT7610U", "ASUS USB-AC51", kMt7610u},
    {M, U, 0x0B05, 0x17DB, F76x0u, AC, 1, false, "MT7610U", "ASUS USB-AC50", kMt7610u},
    {M, U, 0x2001, 0x3D02, F76x0u, AC, 1, false, "MT7610U", "D-Link DWA-171 rev B1", kMt7610u},
    {M, U, 0x057C, 0x8502, F76x0u, AC, 1, false, "MT7610U", "AVM FRITZ!WLAN AC 430", kMt7610u},
    {M, U, 0x2357, 0x0105, F76x0u, AC, 1, false, "MT7610U", "TP-Link Archer T1U", kMt7610u},
    {M, U, 0x2357, 0x0123, F76x0u, AC, 1, false, "MT7610U", "TP-Link Archer T2UHP", kMt7610u},

    // --- USB, mt7601u (802.11n 1x1)
    {M, U, 0x148F, 0x7601, F7601u, N, 1, false, "MT7601U", "MediaTek MT7601U", kMt7601u},
    {M, U, 0x148F, 0x760B, F7601u, N, 1, false, "MT7601U", "MT7601U USB adapter", kMt7601u},
    {M, U, 0x0E8D, 0x760A, F7601u, N, 1, false, "MT7601U", "MT7601U USB adapter", kMt7601u},
    {M, U, 0x0E8D, 0x760B, F7601u, N, 1, false, "MT7601U", "MT7601U USB adapter", kMt7601u},
    {M, U, 0x0B05, 0x17D3, F7601u, N, 1, false, "MT7601U", "ASUS USB-N10 Nano B1", kMt7601u},
    {M, U, 0x2001, 0x3D04, F7601u, N, 1, false, "MT7601U", "D-Link DWA-127 rev B1", kMt7601u},
    {M, U, 0x2717, 0x4106, F7601u, N, 1, false, "MT7601U", "Xiaomi Mi Wi-Fi", kMt7601u},
    {M, U, 0x7392, 0x7710, F7601u, N, 1, false, "MT7601U", "Edimax EW-7711ULC", kMt7601u},
};

}  // namespace

ArrayView<const WifiAdapterModel> mediatek_wifi_models() { return ArrayView<const WifiAdapterModel>(kModels); }

}  // namespace hardware
