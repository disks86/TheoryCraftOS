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

// An ath10k chip on PCI Express (QCA6174, QCA9377, QCA988X, QCA9887) from power-on to "firmware started", in the order of ath10k_pci_probe, ath10k_core_probe_fw
// and ath10k_core_start (ath10k/pci.c, core.c):
//
//   wake the SoC, reset the chip (QCA6174/QCA9377: cold then warm; QCA988X/QCA9887: warm, cold if that fails), read and check SOC_CHIP_ID          -> Identified
//   set up copy engines 0 and 1 for BMI, wake the target CPU, BMI_GET_TARGET_INFO (the target version picks the hardware parameters)
//   firmware-6.bin ... firmware-2.bin from the chip's folder, the OTP's board id query (BMI_EXECUTE with BMI_PARAM_GET_EEPROM_BOARD_ID)
//   board-2.bin by the board name (bmi-chip-id/bmi-board-id when the OTP gave them, else the PCI vendor/device/subsystem IDs), else board.bin
//   configure the target (hi_app_host_interest, hi_option_flag), write the board data to hi_board_data and set hi_board_data_initialized,
//   run the OTP program (BMI_EXECUTE with otp_exe_param, its result must be 0 unless the firmware says to ignore it), stream the firmware to the patch address,
//   switch the debug UART off, BMI_DONE                                                                                                           -> FirmwareLoaded
//
// After BMI_DONE the firmware boots and talks HTC and WMI over the copy engines; that (and so the MAC address, which arrives in WMI's ready event) is not written:
// start() then returns Unsupported. Also not done: the target-side CE configuration the driver writes through the diagnostic CE (ath10k_pci_init_config), the
// code swap image (QCA6174 firmware-4/5), board extension data, calibration files, the hi_be / hi_fw_swap / hi_hci_uart_pwr_mgmt_params_ext writes (QCA6164 checks
// the last one). QCA99X0, QCA9984 and QCA9888 stop with Unsupported before touching the chip (no register map here).
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/WirelessInterface/IWifiChip.hpp"
#include "Hardware/WirelessInterface/Qualcomm/Ath10kBmi.hpp"
#include "Hardware/WirelessInterface/Qualcomm/Ath10kHw.hpp"
#include "Hardware/WirelessInterface/Qualcomm/Ath10kPci.hpp"
#include "Hardware/WirelessInterface/Qualcomm/AthFirmware.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware::ath {

class Ath10kChip final : public IWifiChip {
 public:
  Ath10kChip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator);
  ~Ath10kChip();

  const WifiAdapterModel& model() const override { return model_; }
  [[nodiscard]] WifiChipResult start(IWifiFirmwareSource& firmware) override;
  void stop() override;
  WifiChipState state() const override { return state_; }
  const WifiChipIdentity& identity() const override { return identity_; }
  MacAddress mac_address() const override { return MacAddress{}; }
  const char* firmware_name() const override { return firmware_name_.empty() ? model_.firmware : firmware_name_.c_str(); }
  const char* message() const override { return message_; }

  // What start() found, for the hardware list and the tests.
  const Ath10kHwParams* hw_params() const { return hw_params_; }
  uint32_t target_version() const { return target_version_; }
  uint16_t subsystem_vendor() const { return subsystem_vendor_; }
  uint16_t subsystem_device() const { return subsystem_device_; }
  const AthBoardName& board_name() const { return board_name_; }      // the name that matched in board-2.bin ("" when board.bin was used)
  const char* board_file() const { return board_file_name_.c_str(); }
  const Ath10kFirmwareFile& firmware_file() const { return fw_; }
  bool otp_board_ids_valid() const { return board_ids_.bmi_ids_valid; }
  uint32_t bmi_messages() const { return bmi_ ? bmi_->messages_sent() : 0; }

 private:
  [[nodiscard]] WifiChipResult fail(WifiChipResult result, const char* message);
  [[nodiscard]] WifiChipResult reset_chip();
  [[nodiscard]] WifiChipResult load_firmware(IWifiFirmwareSource& source);
  [[nodiscard]] WifiChipResult load_board(IWifiFirmwareSource& source);
  [[nodiscard]] WifiChipResult query_otp_board_id();
  [[nodiscard]] WifiChipResult download();
  void read_subsystem_ids();
  void release();

  const WifiAdapterModel& model_;
  WifiChipBus bus_;
  allocators::IAllocator& allocator_;
  WifiChipState state_ = WifiChipState::Off;
  WifiChipIdentity identity_{};
  const char* message_ = "not started";
  FixedString<96> firmware_name_, board_file_name_;

  const Ath10kRegs* regs_ = nullptr;
  UniquePointer<Ath10kPci> pci_;
  UniquePointer<Ath10kPciBmi> transport_;
  UniquePointer<Ath10kBmi> bmi_;
  const Ath10kHwParams* hw_params_ = nullptr;
  uint32_t target_version_ = 0;
  uint16_t subsystem_vendor_ = 0, subsystem_device_ = 0;
  AthBoardIds board_ids_{};
  AthBoardName board_name_;
  ByteBuffer firmware_file_, board_file_;
  Ath10kFirmwareFile fw_{};
  ConstByteView board_data_;
};

}  // namespace hardware::ath
