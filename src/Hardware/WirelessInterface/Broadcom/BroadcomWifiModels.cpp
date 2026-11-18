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

// Broadcom (and Cypress/Infineon, which took over the FullMAC line) Wi-Fi adapters by bus ID. The IDs are the ones the Linux 7.2 modules claim (brcmfmac's PCI, SDIO
// and USB tables, bcma's and ssb's PCI host tables for brcmsmac and b43, brcm_hw_ids.h names), read from the modules' aliases; the chip behind each ID follows
// brcm_hw_ids.h. Firmware names are linux-firmware's; "" where the file depends on the chip revision (brcmfmac picks it from the ChipID register, see
// BrcmfFirmware.cpp), where the files are not redistributable (b43: cut from Broadcom's driver with b43-fwcutter, the name depends on the 802.11 core revision) or
// where only Broadcom's closed "wl" driver runs the chip.
//
// The product names say where the chip is commonly found; they are a guide, not a promise (the same ID ships on many modules). The b43 802.11b/g chips are listed
// as Wi-Fi 4 because WifiStandard starts there.
#include "Hardware/WirelessInterface/WifiAdapterCatalog.hpp"

namespace hardware {

namespace {

constexpr uint16_t kBroadcomPci = 0x14e4;
constexpr uint16_t kBroadcomSdio = 0x02d0;
constexpr uint16_t kCypressSdio = 0x04b4;
constexpr uint16_t kBroadcomUsb = 0x0a5c;
constexpr uint16_t kCypressUsb = 0x04b4;
constexpr uint16_t kLinksysUsb = 0x13b1;

constexpr WifiAdapterModel model(WifiBus bus, uint16_t vendor, uint16_t device, WifiDriverFamily family, WifiStandard standard, uint8_t streams, bool bluetooth,
                                 const char* chip, const char* product, const char* firmware) {
  return WifiAdapterModel{WifiVendor::Broadcom, bus, vendor, device, family, standard, streams, bluetooth, chip, product, firmware};
}
constexpr WifiAdapterModel fmac_pci(uint16_t device, WifiStandard standard, uint8_t streams, bool bluetooth, const char* chip, const char* product, const char* firmware) {
  return model(WifiBus::Pci, kBroadcomPci, device, WifiDriverFamily::Brcmfmac, standard, streams, bluetooth, chip, product, firmware);
}
constexpr WifiAdapterModel fmac_sdio(uint16_t vendor, uint16_t device, WifiStandard standard, uint8_t streams, bool bluetooth, const char* chip, const char* product,
                                     const char* firmware) {
  return model(WifiBus::Sdio, vendor, device, WifiDriverFamily::Brcmfmac, standard, streams, bluetooth, chip, product, firmware);
}
constexpr WifiAdapterModel fmac_usb(uint16_t vendor, uint16_t device, WifiStandard standard, uint8_t streams, const char* chip, const char* product, const char* firmware) {
  return model(WifiBus::Usb, vendor, device, WifiDriverFamily::Brcmfmac, standard, streams, false, chip, product, firmware);
}
constexpr WifiAdapterModel pci(uint16_t device, WifiDriverFamily family, WifiStandard standard, uint8_t streams, bool bluetooth, const char* chip, const char* product,
                               const char* firmware) {
  return model(WifiBus::Pci, kBroadcomPci, device, family, standard, streams, bluetooth, chip, product, firmware);
}

using S = WifiStandard;
using F = WifiDriverFamily;

constexpr WifiAdapterModel kModels[] = {
    // ---- brcmfmac, PCIe (brcmf_pcie_devid_table) ----
    fmac_pci(0x43a3, S::Wifi5, 2, true, "BCM4350", "Dell DW1820A, Lenovo/HP M.2 cards", ""),
    fmac_pci(0x4354, S::Wifi5, 2, true, "BCM4354", "BCM4354 (unprogrammed ID; no brcmfmac PCIe firmware entry)", ""),
    fmac_pci(0x43dc, S::Wifi5, 2, true, "BCM4355", "Apple Macs (T2), CYW89459 modules", ""),
    fmac_pci(0x43ec, S::Wifi5, 2, true, "BCM4356", "GPD Win / GPD Pocket, Xiaomi Mi Pad 2", "brcm/brcmfmac4356-pcie.bin"),
    fmac_pci(0x43d3, S::Wifi5, 2, false, "BCM43567", "BCM43567 M.2 cards", "brcm/brcmfmac43570-pcie.bin"),
    fmac_pci(0x43d9, S::Wifi5, 2, false, "BCM43570", "BCM43570 M.2 cards", "brcm/brcmfmac43570-pcie.bin"),
    fmac_pci(0xaa31, S::Wifi5, 2, false, "BCM43570", "BCM43570 (unprogrammed ID)", "brcm/brcmfmac43570-pcie.bin"),
    fmac_pci(0x43e9, S::Wifi5, 2, true, "BCM4358", "BCM4358 M.2 cards", "brcm/brcmfmac4358-pcie.bin"),
    fmac_pci(0x43ef, S::Wifi5, 2, true, "BCM4359", "BCM4359 RSDB cards", ""),
    fmac_pci(0x4415, S::Wifi5, 2, true, "BCM43596", "BCM43596 / CYW89459 modules", ""),
    fmac_pci(0x43ba, S::Wifi5, 3, false, "BCM43602", "MacBook Pro (2015-2017)", "brcm/brcmfmac43602-pcie.bin"),
    fmac_pci(0x43bb, S::Wifi5, 3, false, "BCM43602", "BCM43602 (2.4 GHz only)", "brcm/brcmfmac43602-pcie.bin"),
    fmac_pci(0x43bc, S::Wifi5, 3, false, "BCM43602", "BCM43602 (5 GHz only)", "brcm/brcmfmac43602-pcie.bin"),
    fmac_pci(0xaa52, S::Wifi5, 3, false, "BCM43602", "BCM43602 (unprogrammed ID)", "brcm/brcmfmac43602-pcie.bin"),
    fmac_pci(0x4464, S::Wifi5, 3, true, "BCM4364", "Apple MacBook Pro / Air, iMac with T2 (2018-2020)", ""),
    fmac_pci(0x43ca, S::Wifi5, 4, false, "BCM4365", "802.11ac 4x4 routers", ""),
    fmac_pci(0x43cb, S::Wifi5, 4, false, "BCM4365", "802.11ac 4x4 routers (2.4 GHz)", ""),
    fmac_pci(0x43cc, S::Wifi5, 4, false, "BCM4365", "802.11ac 4x4 routers (5 GHz)", ""),
    fmac_pci(0x43c3, S::Wifi5, 4, false, "BCM4366", "802.11ac 4x4 routers (ASUS RT-AC88U, Netgear R8000P)", ""),
    fmac_pci(0x43c4, S::Wifi5, 4, false, "BCM4366", "802.11ac 4x4 routers (2.4 GHz)", ""),
    fmac_pci(0x43c5, S::Wifi5, 4, false, "BCM4366", "802.11ac 4x4 routers (5 GHz)", ""),
    fmac_pci(0x440d, S::Wifi5, 2, true, "BCM4371", "BCM4371 M.2 cards", "brcm/brcmfmac4371-pcie.bin"),
    fmac_pci(0x4488, S::Wifi6, 2, true, "BCM4377", "Apple MacBook Pro 16-inch / Air (T2, 2019-2020)", "brcm/brcmfmac4377b3-pcie.bin"),
    fmac_pci(0x4425, S::Wifi6, 2, true, "BCM4378", "Apple M1 Macs (MacBook Air / Pro 13-inch, Mac mini, iMac)", ""),
    fmac_pci(0x4433, S::Wifi6E, 2, true, "BCM4387", "Apple M1 Pro/Max and M2 Macs", "brcm/brcmfmac4387c2-pcie.bin"),
    fmac_pci(0x4417, S::Wifi5, 2, true, "CYW54591", "Cypress CYW54591 modules", "brcm/brcmfmac54591-pcie.bin"),
    fmac_pci(0x449d, S::Wifi5, 2, true, "CYW43752", "Cypress/Synaptics CYW43752 modules", "brcm/brcmfmac43752-pcie.bin"),

    // ---- brcmfmac, SDIO (brcmf_sdmmc_ids) ----
    fmac_sdio(kBroadcomSdio, 0xa887, S::Wifi4, 1, false, "BCM43143", "BCM43143 SDIO modules", "brcm/brcmfmac43143-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0x4324, S::Wifi4, 2, false, "BCM43241", "Intel Bay Trail tablets, Advantech MICA", ""),
    fmac_sdio(kBroadcomSdio, 0x4329, S::Wifi4, 1, true, "BCM4329", "BCM4329 SDIO modules (Sonics backplane: not supported here)", "brcm/brcmfmac4329-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0x4330, S::Wifi4, 1, true, "BCM4330", "AP6330 modules (Cubieboard 2, tablets)", "brcm/brcmfmac4330-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0x4334, S::Wifi4, 1, true, "BCM4334", "BCM4334 SDIO modules", "brcm/brcmfmac4334-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0xa94c, S::Wifi4, 1, true, "BCM43340", "Bay Trail tablets (ASUS TF103C, MeegoPad T08)", "brcm/brcmfmac43340-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0xa94d, S::Wifi4, 1, true, "BCM43341", "Bay Trail tablets", "brcm/brcmfmac43340-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0xa962, S::Wifi4, 1, false, "BCM43362", "AP6210 modules (Cubietruck, Banana Pro)", "brcm/brcmfmac43362-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0xa9a4, S::Wifi4, 1, true, "BCM43364", "BCM43364 SDIO modules (no brcmfmac firmware entry)", ""),
    fmac_sdio(kBroadcomSdio, 0x4335, S::Wifi5, 1, true, "BCM4335", "BCM4335 / BCM4339 SDIO modules", "brcm/brcmfmac4335-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0x4339, S::Wifi5, 1, true, "BCM4339", "AP6335 modules", "brcm/brcmfmac4339-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0xa9a6, S::Wifi4, 1, true, "BCM43430", "Raspberry Pi 3 Model B, Zero W, Zero 2 W (BCM43438)", "brcm/brcmfmac43430-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0xa9af, S::Wifi4, 1, true, "CYW43439", "Raspberry Pi Pico W (gSPI there)", "brcm/brcmfmac43439-sdio.bin"),
    fmac_sdio(kCypressSdio, 0xbd3d, S::Wifi4, 1, true, "CYW43439", "CYW43439 modules (Cypress vendor ID)", "brcm/brcmfmac43439-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0x4345, S::Wifi5, 1, true, "BCM4345", "BCM43455 / BCM43456 SDIO modules", ""),
    fmac_sdio(kBroadcomSdio, 0xa9bf, S::Wifi5, 1, true, "CYW43455", "Raspberry Pi 3 Model B+, 4, 400, 5, CM4; Pinebook Pro", "brcm/brcmfmac43455-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0x4354, S::Wifi5, 2, true, "BCM4354", "NVIDIA Jetson TX1 (P2371-2180)", "brcm/brcmfmac4354-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0x4356, S::Wifi5, 2, true, "BCM4356", "AP6356S modules (Firefly RK3399, NanoPC-T4, Rock960)", "brcm/brcmfmac4356-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0x4359, S::Wifi5, 2, true, "BCM4359", "AP6359SA modules", "brcm/brcmfmac4359-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0x4355, S::Wifi5, 2, true, "CYW89359", "Cypress CYW89359 (automotive) modules (no brcmfmac firmware entry)", ""),
    fmac_sdio(kBroadcomSdio, 0x4373, S::Wifi5, 1, true, "CYW4373", "Cypress CYW4373 modules", "brcm/brcmfmac4373-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0xa804, S::Wifi5, 1, true, "CYW43012", "Cypress CYW43012 modules", "brcm/brcmfmac43012-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0xaae7, S::Wifi5, 2, true, "CYW43751", "Synaptics/Cypress CYW43751 modules", "brcm/brcmfmac43752-sdio.bin"),
    fmac_sdio(kBroadcomSdio, 0xaae8, S::Wifi5, 2, true, "CYW43752", "Synaptics/Cypress CYW43752 modules", "brcm/brcmfmac43752-sdio.bin"),

    // ---- brcmfmac, USB (brcmf_usb_devid_table) ----
    fmac_usb(kBroadcomUsb, 0xbd1e, S::Wifi4, 1, "BCM43143", "BCM43143 USB adapters", "brcm/brcmfmac43143.bin"),
    fmac_usb(kBroadcomUsb, 0xbd17, S::Wifi4, 2, "BCM43236", "BCM43236 USB adapters", "brcm/brcmfmac43236b.bin"),
    fmac_usb(kLinksysUsb, 0x0039, S::Wifi4, 2, "BCM43236", "Linksys AE2500", "brcm/brcmfmac43236b.bin"),
    fmac_usb(kBroadcomUsb, 0xbd1f, S::Wifi4, 2, "BCM43242", "BCM43242 USB adapters", "brcm/brcmfmac43242a.bin"),
    fmac_usb(kBroadcomUsb, 0xbd27, S::Wifi5, 2, "BCM43569", "BCM43569 USB adapters", "brcm/brcmfmac43569.bin"),
    fmac_usb(kCypressUsb, 0xbd29, S::Wifi5, 1, "CYW4373", "Cypress CYW4373 USB modules", "brcm/brcmfmac4373.bin"),

    // ---- brcmsmac (bcma PCI host IDs that brcmsmac's 802.11 core revisions cover) ----
    pci(0x4727, F::Brcmsmac, S::Wifi4, 1, false, "BCM4313", "Dell Wireless 1501, HP/Lenovo half mini cards", "brcm/bcm43xx-0.fw"),
    pci(0x4353, F::Brcmsmac, S::Wifi4, 2, false, "BCM43224", "Apple MacBook Air (2010), half mini cards", "brcm/bcm43xx-0.fw"),
    pci(0xa8d8, F::Brcmsmac, S::Wifi4, 2, false, "BCM43224", "BCM43224 (unprogrammed ID)", "brcm/bcm43xx-0.fw"),
    pci(0x4357, F::Brcmsmac, S::Wifi4, 2, false, "BCM43225", "HP/Lenovo half mini cards (2.4 GHz)", "brcm/bcm43xx-0.fw"),

    // ---- b43 (ssb and bcma PCI host IDs) ----
    pci(0x4306, F::B43, S::Wifi4, 1, false, "BCM4306", "802.11b/g cards (Apple AirPort Extreme, Linksys WMP54G)", ""),
    pci(0x4307, F::B43, S::Wifi4, 1, false, "BCM4306", "802.11b/g cards", ""),
    pci(0x4311, F::B43, S::Wifi4, 1, false, "BCM4311", "802.11b/g mini PCIe cards (Dell 1390, HP)", ""),
    pci(0x4312, F::B43, S::Wifi4, 1, false, "BCM4311", "802.11a/b/g mini PCIe cards (Dell 1490)", ""),
    pci(0x4315, F::B43, S::Wifi4, 1, false, "BCM4312", "802.11b/g LP-PHY cards (Dell 1395/1397, HP)", ""),
    pci(0x4318, F::B43, S::Wifi4, 1, false, "BCM4318", "802.11b/g cards (Linksys WMP54GS)", ""),
    pci(0x4319, F::B43, S::Wifi4, 1, false, "BCM4318", "802.11a/b/g cards", ""),
    pci(0x4320, F::B43, S::Wifi4, 1, false, "BCM4306", "802.11b/g cards (Dell 1350, Apple AirPort Extreme)", ""),
    pci(0x4324, F::B43, S::Wifi4, 1, false, "BCM4309", "802.11a/b/g cards (Dell 1450)", ""),
    pci(0x4328, F::B43, S::Wifi4, 2, false, "BCM4321", "802.11a/b/g/n cards (Apple AirPort Extreme n, Dell 1500)", ""),
    pci(0x4329, F::B43, S::Wifi4, 2, false, "BCM4321", "802.11b/g/n cards", ""),
    pci(0x432b, F::B43, S::Wifi4, 2, false, "BCM4322", "802.11a/b/g/n cards (Dell 1510)", ""),
    pci(0x432c, F::B43, S::Wifi4, 2, false, "BCM4322", "802.11b/g/n cards", ""),
    pci(0x4331, F::B43, S::Wifi4, 3, false, "BCM4331", "MacBook Pro / iMac (2011-2012)", ""),

    // ---- the proprietary "wl" driver only ----
    pci(0x43b1, F::BroadcomWl, S::Wifi5, 2, true, "BCM4352", "Dell DW1550 / DW1560, ASUS PCE-AC56, Lenovo (with BCM20702 Bluetooth)", ""),
    pci(0x43a0, F::BroadcomWl, S::Wifi5, 3, false, "BCM4360", "MacBook Pro / Air / iMac (2013-2015), ASUS PCE-AC66", ""),
};

}  // namespace

ArrayView<const WifiAdapterModel> broadcom_wifi_models() { return ArrayView<const WifiAdapterModel>(kModels); }

}  // namespace hardware
