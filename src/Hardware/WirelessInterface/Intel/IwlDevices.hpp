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


// Which Intel Wireless-AC 9000 series (9260, 9560, 9462 ...) a PCI function is, and which firmware file it wants. Written after iwlwifi's pcie/drv.c, cfg/9000.c and
// iwl-drv.c (iwl_drv_get_fwname_pre): the PCI device IDs, the cards' crystal latency and shared clock, and the file name built from the hardware revision (CSR_HW_REV) and the radio
// (CSR_HW_RF_ID). The Dell XPS 15 9575's card is of this family. The 9000 family comes in two MACs, PU ("9000-pu", the integrated / CNVi cards and the 9560 modules) and TH
// ("9260-th", the PCIe 9260 / 9560 cards, 8086:2526); both pair with the JF radio.
#pragma once
#include <stdint.h>

namespace hardware {

struct IwlPciDevice {
  uint16_t device_id;
  bool integrated;
  uint16_t xtal_latency;                 // microseconds the crystal needs after a power request (0: not set)
  bool shared_clock;                     // FW_PHY_CFG_SHARED_CLK
  const char* family;                    // for the log
};

constexpr uint16_t kIntelVendor = 0x8086;
constexpr uint32_t kIwl9000FirmwareApiMin = 30, kIwl9000FirmwareApiMax = 46;
constexpr uint32_t kIwlMacTypePu = 0x31, kIwlMacTypeTh = 0x32;
constexpr uint32_t kIwlRfTypeJf2 = 0x105, kIwlRfTypeJf1 = 0x108;

// The entry for a 9000 series device ID, or nullptr.
const IwlPciDevice* iwl_find_pci_device(uint16_t vendor, uint16_t device);

inline uint32_t iwl_hw_rev_type(uint32_t hw_rev) { return (hw_rev & 0x000FFF0) >> 4; }
inline uint32_t iwl_hw_rf_type(uint32_t hw_rf_id) { return (hw_rf_id & 0x0FFF000) >> 12; }
inline bool iwl_hw_rf_cdb(uint32_t hw_rf_id) { return ((hw_rf_id & 0x10000000) >> 28) != 0; }

// "iwlwifi-9000-pu-b0-jf-b0" or "iwlwifi-9260-th-b0-jf-b0" from the registers; false for a MAC or radio that is not a 9000 series one. `out` holds at least 40 bytes.
bool iwl_firmware_prefix(uint32_t hw_rev, uint32_t hw_rf_id, char* out, uint32_t capacity);
// "<prefix>-<api>.ucode"
bool iwl_firmware_file_name(const char* prefix, uint32_t api, char* out, uint32_t capacity);

}  // namespace hardware
