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

// An ath11k (QCA6390, WCN6855, QCN9074) or ath12k (WCN7850, QCN9274) chip on PCI Express, as far as it goes without QMI, after ath11k/pci.c, ath11k/mhi.c,
// ath11k/core.c and their ath12k counterparts:
//
//   read TCSR_SOC_HW_VERSION (ath11k_pci_read_hw_version) and pick the firmware folder (WCN6855 minor 0/1 -> hw2.0, 0x10/0x11 -> hw2.1, ...)       -> Identified
//   the firmware: <dir>/firmware-2.bin ("QCOM-ATH1xK-FW" container) or else <dir>/amss.bin (an ELF file); the board file <dir>/board-2.bin is checked and the
//   entries for this card's PCI subsystem IDs are counted (the final choice needs the QMI chip and board ids), else <dir>/board.bin
//   the SoC global reset and MHICTRL reset (ath11k_pci_sw_reset), the BHI block, the execution environment; from the primary boot loader the first 512 KiB of
//   the image (the SBL) go over BHI (mhi_fw_load_bhi), then the wait for MHI READY                                                                 -> FirmwareLoaded
//
// Not written: the MHI context (rings, event rings, M0), the BHIe download of the rest of amss.bin that the SBL asks for, QMI (which reports the chip/board ids,
// takes the board data and M3 image, and the MAC address), and the data path. start() returns Unsupported there. The register offsets are from the Linux source,
// not verified on hardware; QCA2066 and QCA6698AQ (which share WCN6855's device ID and are told apart by TCSR_SOC_HW_SUB_VER) are taken as WCN6855 hw2.1.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/WirelessInterface/IWifiChip.hpp"
#include "Hardware/WirelessInterface/Qualcomm/AthFirmware.hpp"
#include "Hardware/WirelessInterface/Qualcomm/AthMhi.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/FixedString.hpp"

namespace hardware::ath {

// PCI device IDs (vendor 0x17CB).
constexpr uint16_t kQcomVendor = 0x17CB;
constexpr uint16_t kQca6390 = 0x1101, kWcn6855 = 0x1103, kQcn9074 = 0x1104, kWcn7850 = 0x1107, kQcn9274 = 0x1109;

// The firmware folder for a device and its TCSR_SOC_HW_VERSION value ("ath11k/WCN6855/hw2.1"), or null for a version the driver does not support.
const char* ath11k_firmware_dir(uint16_t device_id, uint32_t soc_hw_version);
// Whether the board file is searched by bus and board (ab->id.bdf_search = ATH1xK_BDF_SEARCH_BUS_AND_BOARD: WCN6855, WCN7850).
AthBdfSearch ath11k_bdf_search(uint16_t device_id);

class Ath11kChip final : public IWifiChip {
 public:
  // The MHI registers lie in the first window of BAR 0; offsets beyond `register_length` are refused.
  static constexpr uint32_t kRegisterLength = kAthWindowStart;

  Ath11kChip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator);
  ~Ath11kChip();

  const WifiAdapterModel& model() const override { return model_; }
  [[nodiscard]] WifiChipResult start(IWifiFirmwareSource& firmware) override;
  void stop() override;
  WifiChipState state() const override { return state_; }
  const WifiChipIdentity& identity() const override { return identity_; }
  MacAddress mac_address() const override { return MacAddress{}; }
  const char* firmware_name() const override { return firmware_name_.empty() ? model_.firmware : firmware_name_.c_str(); }
  const char* message() const override { return message_; }

  bool is_ath12k() const { return model_.family == WifiDriverFamily::Ath12k; }
  const char* firmware_dir() const { return dir_; }
  ConstByteView amss() const { return amss_; }
  uint32_t board_entries() const { return board_entries_; }            // BOARD entries in board-2.bin
  uint32_t matching_board_entries() const { return matching_entries_; } // ... whose name has this card's PCI IDs (bus-and-board search)
  bool regdb_present() const { return regdb_entries_ > 0; }
  uint32_t exec_env_at_start() const { return exec_env_; }
  uint16_t subsystem_vendor() const { return subsystem_vendor_; }
  uint16_t subsystem_device() const { return subsystem_device_; }

 private:
  [[nodiscard]] WifiChipResult fail(WifiChipResult result, const char* message);
  [[nodiscard]] WifiChipResult load_firmware(IWifiFirmwareSource& source);
  [[nodiscard]] WifiChipResult load_board(IWifiFirmwareSource& source);
  [[nodiscard]] WifiChipResult boot_sbl();
  uint32_t read_hw_version();

  const WifiAdapterModel& model_;
  WifiChipBus bus_;
  allocators::IAllocator& allocator_;
  WifiChipState state_ = WifiChipState::Off;
  WifiChipIdentity identity_{};
  const char* message_ = "not started";
  const char* dir_ = nullptr;
  FixedString<96> firmware_name_;
  ByteBuffer firmware_file_, board_file_;
  ConstByteView amss_;
  uint32_t board_entries_ = 0, matching_entries_ = 0, regdb_entries_ = 0;
  uint32_t exec_env_ = 0;
  uint16_t subsystem_vendor_ = 0, subsystem_device_ = 0;
};

}  // namespace hardware::ath
