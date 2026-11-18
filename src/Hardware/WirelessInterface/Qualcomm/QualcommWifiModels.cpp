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

// Qualcomm Atheros Wi-Fi adapters by bus ID, after the ID tables of the Linux drivers: ath9k/pci.c (ath_pci_id_table), ath9k/hif_usb.c (ath9k_hif_usb_ids),
// ath10k/pci.c (ath10k_pci_id_table), ath11k/pci.c (ath11k_pci_id_table) and ath12k/pci.c (ath12k_pci_id_table). The firmware names are linux-firmware's:
// ath9k needs none; ath9k_htc loads "ath9k_htc/htc_9271-1.4.0.fw" or "ath9k_htc/htc_7010-1.4.0.fw"; ath10k "<dir>/firmware-N.bin" (the newest API the folder
// has); ath11k and ath12k "<dir>/amss.bin" (or the firmware-2.bin container where linux-firmware ships one). Where the folder depends on the chip revision the
// entry names the common one and the chip driver names the right one once it read the revision (QCA6174 hw2.1 / hw3.0, WCN6855 hw2.0 / hw2.1).
//
// Spatial streams and Bluetooth are the chips' usual configuration; the product names are examples of cards built on the chip.
#include "Hardware/WirelessInterface/WifiAdapterCatalog.hpp"

namespace hardware {

namespace {

constexpr WifiVendor Q = WifiVendor::Qualcomm;
constexpr WifiBus Pci = WifiBus::Pci, Usb = WifiBus::Usb;
constexpr WifiDriverFamily Ath9k = WifiDriverFamily::Ath9k, Htc = WifiDriverFamily::Ath9kHtc, Ath10k = WifiDriverFamily::Ath10k, Ath11k = WifiDriverFamily::Ath11k,
                           Ath12k = WifiDriverFamily::Ath12k;
constexpr WifiStandard N = WifiStandard::Wifi4, Ac = WifiStandard::Wifi5, Ax = WifiStandard::Wifi6, Ax6e = WifiStandard::Wifi6E, Be = WifiStandard::Wifi7;

constexpr const char* kHtc9271 = "ath9k_htc/htc_9271-1.4.0.fw";
constexpr const char* kHtc7010 = "ath9k_htc/htc_7010-1.4.0.fw";

constexpr WifiAdapterModel kModels[] = {
    // ath9k, PCIe (no firmware: the MAC is driven by the host)
    {Q, Pci, 0x168C, 0x002A, Ath9k, N, 2, false, "AR9280", "Atheros AR928X Wireless Network Adapter (AR5BXB92)", ""},
    {Q, Pci, 0x168C, 0x002B, Ath9k, N, 1, false, "AR9285", "Atheros AR9285 Wireless Network Adapter (AR5B95)", ""},
    {Q, Pci, 0x168C, 0x002E, Ath9k, N, 2, false, "AR9287", "Atheros AR9287 Wireless Network Adapter", ""},
    {Q, Pci, 0x168C, 0x0030, Ath9k, N, 3, false, "AR9380", "Atheros AR93xx Wireless Network Adapter (AR9380/AR9382)", ""},
    {Q, Pci, 0x168C, 0x0032, Ath9k, N, 1, false, "AR9485", "Atheros AR9485 Wireless Network Adapter", ""},
    {Q, Pci, 0x168C, 0x0033, Ath9k, N, 3, false, "AR9580", "Atheros AR9580 Wireless Network Adapter", ""},
    {Q, Pci, 0x168C, 0x0034, Ath9k, N, 2, true, "AR9462", "Qualcomm Atheros AR9462 (AR5B22, Killer Wireless-N 1202)", ""},
    {Q, Pci, 0x168C, 0x0036, Ath9k, N, 1, true, "QCA9565", "Qualcomm Atheros QCA9565 / AR9565 (Dell Wireless 1705)", ""},
    {Q, Pci, 0x168C, 0x0037, Ath9k, N, 1, false, "AR9485", "Qualcomm Atheros AR1111 / AR9485 Wireless Network Adapter", ""},

    // ath9k_htc, USB (the host interface firmware runs on the chip's own CPU)
    {Q, Usb, 0x0CF3, 0x9271, Htc, N, 1, false, "AR9271", "Atheros AR9271 USB adapter (TP-Link TL-WN722N v1, Alfa AWUS036NHA)", kHtc9271},
    {Q, Usb, 0x0CF3, 0x1006, Htc, N, 1, false, "AR9271", "Atheros AR9271 USB adapter", kHtc9271},
    {Q, Usb, 0x0846, 0x9030, Htc, N, 1, false, "AR9271", "Netgear WNA1100 (N150)", kHtc9271},
    {Q, Usb, 0x07B8, 0x9271, Htc, N, 1, false, "AR9271", "Altai WA1011N-GU", kHtc9271},
    {Q, Usb, 0x07D1, 0x3A10, Htc, N, 1, false, "AR9271", "D-Link Wireless 150", kHtc9271},
    {Q, Usb, 0x13D3, 0x3327, Htc, N, 1, false, "AR9271", "AzureWave AR9271 module", kHtc9271},
    {Q, Usb, 0x13D3, 0x3328, Htc, N, 1, false, "AR9271", "AzureWave AR9271 module", kHtc9271},
    {Q, Usb, 0x04CA, 0x4605, Htc, N, 1, false, "AR9271", "Lite-On AR9271 module", kHtc9271},
    {Q, Usb, 0x057C, 0x8403, Htc, N, 1, false, "AR9271", "AVM FRITZ!WLAN 11N v2 USB", kHtc9271},
    {Q, Usb, 0x0CF3, 0x7015, Htc, N, 2, false, "AR7010", "Atheros AR7010 + AR9287 USB adapter", kHtc7010},
    {Q, Usb, 0x0CF3, 0x7010, Htc, N, 2, false, "AR7010", "Atheros AR7010 + AR9280 USB adapter", kHtc7010},
    {Q, Usb, 0x0846, 0x9018, Htc, N, 2, false, "AR7010", "Netgear WNDA3200 (AR7010 + AR9280)", kHtc7010},
    {Q, Usb, 0x0411, 0x017F, Htc, N, 2, false, "AR7010", "Sony UWA-BR100 (AR7010 + AR9280)", kHtc7010},

    // ath10k, PCIe
    {Q, Pci, 0x168C, 0x003C, Ath10k, Ac, 3, false, "QCA988X", "Qualcomm Atheros QCA9880/QCA9882 (Compex WLE900VX)", "ath10k/QCA988X/hw2.0/firmware-5.bin"},
    {Q, Pci, 0x0777, 0x11AC, Ath10k, Ac, 3, false, "QCA988X", "Ubiquiti QCA988X card", "ath10k/QCA988X/hw2.0/firmware-5.bin"},
    {Q, Pci, 0x168C, 0x003E, Ath10k, Ac, 2, true, "QCA6174", "Qualcomm Atheros QCA6174 (Killer Wireless-n/a/ac 1535, Killer Wireless-AC 1525, Dell DW1820)", "ath10k/QCA6174/hw3.0/firmware-6.bin"},
    {Q, Pci, 0x168C, 0x0040, Ath10k, Ac, 4, false, "QCA99X0", "Qualcomm Atheros QCA9990/QCA9994 (802.11ac wave 2)", "ath10k/QCA99X0/hw2.0/firmware-5.bin"},
    // QCA6164 is a variant of QCA6174 with its own device ID; the stream count is that of QCA6174 (not checked)
    {Q, Pci, 0x168C, 0x0041, Ath10k, Ac, 2, true, "QCA6164", "Qualcomm Atheros QCA6164", "ath10k/QCA6174/hw2.1/firmware-5.bin"},
    {Q, Pci, 0x168C, 0x0042, Ath10k, Ac, 1, true, "QCA9377", "Qualcomm Atheros QCA9377 (Dell DW1810)", "ath10k/QCA9377/hw1.0/firmware-6.bin"},
    {Q, Pci, 0x168C, 0x0046, Ath10k, Ac, 4, false, "QCA9984", "Qualcomm Atheros QCA9984 (802.11ac wave 2)", "ath10k/QCA9984/hw1.0/firmware-5.bin"},
    {Q, Pci, 0x168C, 0x0050, Ath10k, Ac, 1, false, "QCA9887", "Qualcomm Atheros QCA9887", "ath10k/QCA9887/hw1.0/firmware-5.bin"},
    {Q, Pci, 0x168C, 0x0056, Ath10k, Ac, 2, false, "QCA9888", "Qualcomm Atheros QCA9886/QCA9888 (802.11ac wave 2)", "ath10k/QCA9888/hw2.0/firmware-5.bin"},

    // ath11k, PCIe
    {Q, Pci, 0x17CB, 0x1101, Ath11k, Ax, 2, true, "QCA6390", "Qualcomm QCA6390 (FastConnect 6800)", "ath11k/QCA6390/hw2.0/amss.bin"},
    {Q, Pci, 0x17CB, 0x1103, Ath11k, Ax6e, 2, true, "WCN6855", "Qualcomm WCN6855 / QCNFA765 (FastConnect 6900, Killer Wi-Fi 6E AX1675)", "ath11k/WCN6855/hw2.0/amss.bin"},
    {Q, Pci, 0x17CB, 0x1104, Ath11k, Ax6e, 4, false, "QCN9074", "Qualcomm QCN9074 (802.11ax 5/6 GHz radio)", "ath11k/QCN9074/hw1.0/amss.bin"},

    // ath12k, PCIe
    {Q, Pci, 0x17CB, 0x1107, Ath12k, Be, 2, true, "WCN7850", "Qualcomm WCN7850 / NCM865 (FastConnect 7800)", "ath12k/WCN7850/hw2.0/amss.bin"},
    {Q, Pci, 0x17CB, 0x1109, Ath12k, Be, 4, false, "QCN9274", "Qualcomm QCN9274 (802.11be radio)", "ath12k/QCN9274/hw2.0/firmware-2.bin"},
};

}  // namespace

ArrayView<const WifiAdapterModel> qualcomm_wifi_models() { return ArrayView<const WifiAdapterModel>(kModels); }

}  // namespace hardware
