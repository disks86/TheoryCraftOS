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


#include "Hardware/WirelessInterface/Intel/IwlDevices.hpp"

namespace hardware {

namespace {

constexpr IwlPciDevice kDevices[] = {
    {0x2526, false, 0, false, "Wireless-AC 9260 / 9560 (PCIe)"},
    {0x271B, false, 0, false, "Wireless-AC 9000 series (PCIe)"},
    {0x271C, false, 0, false, "Wireless-AC 9000 series (PCIe)"},
    {0x30DC, true, 2820, false, "Wireless-AC 9560 (long latency)"},
    {0x31DC, true, 670, true, "Wireless-AC 9560 (shared clock)"},
    {0x9DF0, true, 650, false, "Wireless-AC 9560 (CNVi)"},
    {0xA370, true, 650, false, "Wireless-AC 9560 (CNVi)"},
};

// appends text; false if it does not fit (the terminating zero included)
bool put(char* out, uint32_t capacity, uint32_t* at, const char* text) {
  for (uint32_t i = 0; text[i]; ++i) {
    if (*at + 1 >= capacity) return false;
    out[(*at)++] = text[i];
  }
  out[*at] = 0;
  return true;
}

}  // namespace

const IwlPciDevice* iwl_find_pci_device(uint16_t vendor, uint16_t device) {
  if (vendor != kIntelVendor) return nullptr;
  for (const IwlPciDevice& d : kDevices)
    if (d.device_id == device) return &d;
  return nullptr;
}

bool iwl_firmware_prefix(uint32_t hw_rev, uint32_t hw_rf_id, char* out, uint32_t capacity) {
  if (capacity == 0) return false;
  out[0] = 0;
  const uint32_t mac = iwl_hw_rev_type(hw_rev), rf = iwl_hw_rf_type(hw_rf_id);
  const char* mac_name;
  if (mac == kIwlMacTypePu) mac_name = "9000-pu";
  else if (mac == kIwlMacTypeTh) mac_name = "9260-th";
  else return false;
  if ((rf != kIwlRfTypeJf1 && rf != kIwlRfTypeJf2) || iwl_hw_rf_cdb(hw_rf_id)) return false;
  uint32_t at = 0;
  // (the 9000 MACs' firmware is always the "b0" step, and so is the JF radio's)
  return put(out, capacity, &at, "iwlwifi-") && put(out, capacity, &at, mac_name) && put(out, capacity, &at, "-b0-jf-b0");
}

bool iwl_firmware_file_name(const char* prefix, uint32_t api, char* out, uint32_t capacity) {
  if (capacity == 0) return false;
  out[0] = 0;
  uint32_t at = 0;
  if (!put(out, capacity, &at, prefix) || !put(out, capacity, &at, "-")) return false;
  char digits[12];
  uint32_t n = 0;
  do { digits[n++] = static_cast<char>('0' + api % 10); api /= 10; } while (api != 0 && n < 11);
  char text[12];
  for (uint32_t i = 0; i < n; ++i) text[i] = digits[n - 1 - i];
  text[n] = 0;
  return put(out, capacity, &at, text) && put(out, capacity, &at, ".ucode");
}

}  // namespace hardware
