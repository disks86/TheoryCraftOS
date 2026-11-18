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

// What ath10k knows about each of its PCIe chips, after ath10k/hw.h, hw.c, core.c (ath10k_hw_params_list) and pci.c (ath10k_pci_supp_chips, ath10k_pci_probe):
//
//  - the register map (struct ath10k_hw_regs: qca988x_regs, qca6174_regs, qca99x0_regs; the copy engines are 0x400 apart from CE0 on every one of them),
//  - which revisions of the SOC_CHIP_ID register a device ID is supported with,
//  - the hardware parameters by the target version BMI_GET_TARGET_INFO answers (struct ath10k_hw_params): the firmware folder, the board file and size, the address
//    firmware and OTP load at and the parameter the OTP program runs with.
//
// The values were taken from the Linux source (v6.x) and are not checked against hardware here. QCA6174 hw3.2 uses the hw3.0 folder, as in Linux.
#pragma once
#include <stdint.h>

namespace hardware::ath {

// PCI device IDs (vendor 0x168C; the Ubiquiti QCA988X card is 0x0777:0x11AC).
constexpr uint16_t kQcaVendor = 0x168C, kUbntVendor = 0x0777;
constexpr uint16_t kQca988x = 0x003C, kQca988xUbnt = 0x11AC, kQca6174 = 0x003E, kQca99x0 = 0x0040, kQca6164 = 0x0041, kQca9377 = 0x0042, kQca9984 = 0x0046,
                   kQca9887 = 0x0050, kQca9888 = 0x0056;

enum class Ath10kHwRev : uint8_t { Qca988x, Qca9887, Qca6174, Qca99x0, Qca9984, Qca9888, Qca9377 };

struct Ath10kRegs {
  uint32_t rtc_soc_base, soc_core_base, ce_wrapper_base, ce0_base;
  uint32_t soc_reset_control_si0_rst_mask, soc_reset_control_ce_rst_mask;
  uint32_t soc_chip_id_address, fw_indicator_address, pcie_local_base_address;
  uint32_t rtc_state_val_on;                      // ath10k_hw_values.rtc_state_val_on
  uint32_t ce_desc_meta_data_mask, ce_desc_meta_data_lsb;

  uint32_t ce_base(uint32_t ce) const { return ce0_base + ce * 0x400; }
};

// Fixed offsets (ath10k/hw.h). In the PCIe local window (BAR + pcie_local_base_address):
constexpr uint32_t kRtcStateAddress = 0x0000, kRtcStateVMask = 0x7, kPcieSocWakeAddress = 0x0004, kPcieSocWakeV = 0x1, kPcieSocWakeReset = 0x0,
                   kSocGlobalResetAddress = 0x0008;
// In the RTC SoC block (BAR + rtc_soc_base):
constexpr uint32_t kSocResetControlAddress = 0x0000, kSocResetControlCpuWarmRst = 0x40, kSocLfTimerControl0Address = 0x0050, kSocLfTimerControl0Enable = 0x4;
constexpr uint32_t kSocChipIdRevMask = 0x00000F00, kSocChipIdRevShift = 8;
// In the SoC core block (BAR + soc_core_base): CORE_CTRL, whose CPU interrupt bit wakes the target CPU (ath10k_pci_wake_target_cpu).
constexpr uint32_t kCoreCtrlAddress = 0x0000, kCoreCtrlCpuIntr = 0x00002000;
// FW_INDICATOR (BAR + fw_indicator_address).
constexpr uint32_t kFwIndEventPending = 0x1, kFwIndInitialized = 0x2, kFwIndHostReady = 0x80000000u;

// The target's "host interest" area (targaddrs.h, struct host_interest at QCA988X_HOST_INTEREST_ADDRESS, used for every PCIe chip).
constexpr uint32_t kHostInterestAddress = 0x00400800;
constexpr uint32_t kHiAppHostInterest = 0x00, kHiOptionFlag = 0x10, kHiBoardData = 0x54, kHiBoardDataInitialized = 0x58, kHiSkipClockInit = 0x20;
constexpr uint32_t kHtcProtocolVersion = 0x0002;
// hi_option_flag fields (HI_OPTION_*_SHIFT) and the values ath10k_init_configure_target sets.
constexpr uint32_t kHiOptionNumDevShift = 9, kHiOptionFwModeShift = 12, kHiOptionMacAddrMethodShift = 3, kHiOptionFwModeAp = 2;

struct Ath10kHwParams {
  uint32_t id;                    // the target version (BMI_GET_TARGET_INFO)
  uint16_t dev_id;
  Ath10kHwRev rev;
  const char* name;               // "qca6174 hw3.2"
  uint32_t patch_load_addr;
  uint32_t uart_pin;
  uint32_t otp_exe_param;
  const char* dir;                // "ath10k/QCA6174/hw3.0"
  const char* board;              // "board.bin" (API 1)
  uint32_t board_size, board_ext_size;
};

// Target versions.
constexpr uint32_t kQca988xHw20Version = 0x4100016C, kQca9887Hw10Version = 0x4100016D, kQca6174Hw21Version = 0x05010000, kQca6174Hw30Version = 0x05020000,
                   kQca6174Hw32Version = 0x05030000, kQca99x0Hw20Version = 0x01000000, kQca9984Hw10Version = 0x01000000, kQca9888Hw20Version = 0x01000000,
                   kQca9377Hw10Version = 0x05020000, kQca9377Hw11Version = 0x05020001;

// The register map and the PCIe power-save flag for a device ID; null for one ath10k does not drive over PCIe.
const Ath10kRegs* ath10k_regs_for_device(uint16_t device_id);
// Whether the device needs the SoC woken before register access (ar_pci->pci_ps: QCA6174, QCA6164, QCA9377).
bool ath10k_device_power_save(uint16_t device_id);
// True if this SOC_CHIP_ID value (its revision field) is one the driver supports on that device (ath10k_pci_chip_is_supported).
bool ath10k_chip_supported(uint16_t device_id, uint32_t chip_id);
// The hardware parameters for a target version on a device, or null (ath10k_init_hw_params).
const Ath10kHwParams* ath10k_find_hw_params(uint32_t target_version, uint16_t device_id);
// The expected parameters from the chip ID alone (before BMI answers): the revision field picks the version (QCA6174 4/5 -> hw2.1, 8/9 -> hw3.0, 10 -> hw3.2).
const Ath10kHwParams* ath10k_hw_params_from_chip_id(uint16_t device_id, uint32_t chip_id);

}  // namespace hardware::ath
