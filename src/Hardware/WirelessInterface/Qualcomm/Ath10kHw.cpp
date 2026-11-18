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

#include "Hardware/WirelessInterface/Qualcomm/Ath10kHw.hpp"

namespace hardware::ath {

namespace {

// qca988x_regs (QCA988X, QCA9887) and qca6174_regs (QCA6174, QCA6164, QCA9377), with the matching ath10k_hw_values. QCA99X0, QCA9984 and QCA9888 have a third map
// (qca99x0_regs) that is not written here: those chips are identified from the catalogue only.
constexpr Ath10kRegs kQca988xRegs = {
    .rtc_soc_base = 0x00004000, .soc_core_base = 0x00009000, .ce_wrapper_base = 0x00057000, .ce0_base = 0x00057400,
    .soc_reset_control_si0_rst_mask = 0x00000001, .soc_reset_control_ce_rst_mask = 0x00040000,
    .soc_chip_id_address = 0x000000EC, .fw_indicator_address = 0x00009030, .pcie_local_base_address = 0x00080000,
    .rtc_state_val_on = 3, .ce_desc_meta_data_mask = 0xFFFC, .ce_desc_meta_data_lsb = 2,
};
constexpr Ath10kRegs kQca6174Regs = {
    .rtc_soc_base = 0x00000800, .soc_core_base = 0x0003A000, .ce_wrapper_base = 0x00034000, .ce0_base = 0x00034400,
    .soc_reset_control_si0_rst_mask = 0x00000000, .soc_reset_control_ce_rst_mask = 0x00000001,
    .soc_chip_id_address = 0x000000F0, .fw_indicator_address = 0x0003A028, .pcie_local_base_address = 0x00080000,
    .rtc_state_val_on = 3, .ce_desc_meta_data_mask = 0xFFFC, .ce_desc_meta_data_lsb = 2,
};

// ath10k_pci_supp_chips: (device, SOC_CHIP_ID revision).
struct SupportedChip {
  uint16_t device;
  uint8_t revision;
};
constexpr SupportedChip kSupported[] = {
    {kQca988xUbnt, 2}, {kQca988x, 2},
    {kQca6164, 4}, {kQca6164, 5}, {kQca6164, 8}, {kQca6164, 9}, {kQca6164, 10},
    {kQca6174, 4}, {kQca6174, 5}, {kQca6174, 8}, {kQca6174, 9}, {kQca6174, 10},
    {kQca99x0, 1}, {kQca9984, 0}, {kQca9888, 0}, {kQca9377, 0}, {kQca9377, 1}, {kQca9887, 0},
};

// ath10k_hw_params_list, PCI entries of the chips with a register map above. Board sizes: QCA988X_BOARD_DATA_SZ 7168, QCA6174_BOARD_DATA_SZ 8192 (QCA9377 the same);
// the patch load address is 0x1234 on all of them (QCA*_PATCH_LOAD_ADDR).
constexpr Ath10kHwParams kHwParams[] = {
    {kQca988xHw20Version, kQca988x, Ath10kHwRev::Qca988x, "qca988x hw2.0", 0x1234, 7, 0, "ath10k/QCA988X/hw2.0", "board.bin", 7168, 0},
    {kQca988xHw20Version, kQca988xUbnt, Ath10kHwRev::Qca988x, "qca988x hw2.0 ubiquiti", 0x1234, 7, 0, "ath10k/QCA988X/hw2.0", "board.bin", 7168, 0},
    {kQca9887Hw10Version, kQca9887, Ath10kHwRev::Qca9887, "qca9887 hw1.0", 0x1234, 7, 0, "ath10k/QCA9887/hw1.0", "board.bin", 7168, 0},
    {kQca6174Hw21Version, kQca6164, Ath10kHwRev::Qca6174, "qca6164 hw2.1", 0x1234, 6, 0, "ath10k/QCA6174/hw2.1", "board.bin", 8192, 0},
    {kQca6174Hw21Version, kQca6174, Ath10kHwRev::Qca6174, "qca6174 hw2.1", 0x1234, 6, 0, "ath10k/QCA6174/hw2.1", "board.bin", 8192, 0},
    {kQca6174Hw30Version, kQca6174, Ath10kHwRev::Qca6174, "qca6174 hw3.0", 0x1234, 6, 0, "ath10k/QCA6174/hw3.0", "board.bin", 8192, 0},
    {kQca6174Hw32Version, kQca6174, Ath10kHwRev::Qca6174, "qca6174 hw3.2", 0x1234, 6, 0, "ath10k/QCA6174/hw3.0", "board.bin", 8192, 0},
    {kQca9377Hw10Version, kQca9377, Ath10kHwRev::Qca9377, "qca9377 hw1.0", 0x1234, 6, 0, "ath10k/QCA9377/hw1.0", "board.bin", 8192, 0},
    {kQca9377Hw11Version, kQca9377, Ath10kHwRev::Qca9377, "qca9377 hw1.1", 0x1234, 6, 0, "ath10k/QCA9377/hw1.0", "board.bin", 8192, 0},
};

}  // namespace

const Ath10kRegs* ath10k_regs_for_device(uint16_t device_id) {
  switch (device_id) {
    case kQca988x:
    case kQca988xUbnt:
    case kQca9887: return &kQca988xRegs;
    case kQca6174:
    case kQca6164:
    case kQca9377: return &kQca6174Regs;
    default: return nullptr;
  }
}

bool ath10k_device_power_save(uint16_t device_id) { return device_id == kQca6174 || device_id == kQca6164 || device_id == kQca9377; }

bool ath10k_chip_supported(uint16_t device_id, uint32_t chip_id) {
  const uint32_t revision = (chip_id & kSocChipIdRevMask) >> kSocChipIdRevShift;
  for (const SupportedChip& chip : kSupported)
    if (chip.device == device_id && chip.revision == revision) return true;
  return false;
}

const Ath10kHwParams* ath10k_find_hw_params(uint32_t target_version, uint16_t device_id) {
  for (const Ath10kHwParams& params : kHwParams)
    if (params.id == target_version && params.dev_id == device_id) return &params;
  return nullptr;
}

const Ath10kHwParams* ath10k_hw_params_from_chip_id(uint16_t device_id, uint32_t chip_id) {
  if (!ath10k_chip_supported(device_id, chip_id)) return nullptr;
  const uint32_t revision = (chip_id & kSocChipIdRevMask) >> kSocChipIdRevShift;
  uint32_t version = 0;
  switch (device_id) {
    case kQca988x:
    case kQca988xUbnt: version = kQca988xHw20Version; break;
    case kQca9887: version = kQca9887Hw10Version; break;
    case kQca6174:
    case kQca6164: version = revision <= 5 ? kQca6174Hw21Version : revision <= 9 ? kQca6174Hw30Version : kQca6174Hw32Version; break;
    case kQca9377: version = revision == 0 ? kQca9377Hw10Version : kQca9377Hw11Version; break;
    default: return nullptr;
  }
  return ath10k_find_hw_params(version, device_id);
}

}  // namespace hardware::ath
