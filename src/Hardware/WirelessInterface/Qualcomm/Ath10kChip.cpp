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

#include "Hardware/WirelessInterface/Qualcomm/Ath10kChip.hpp"

namespace hardware::ath {

namespace {

constexpr uint32_t kFwApiMax = 6, kFwApiMin = 2;     // ATH10K_FW_API_MAX / ATH10K_FW_API_MIN (API 1, firmware.bin + otp.bin, is not read)
constexpr uint32_t kWarmResetAttempts = 3;            // ATH10K_PCI_NUM_WARM_RESET_ATTEMPTS
constexpr uint16_t kPciSubsystemOffset = 0x2C;

uint32_t hi(uint32_t item) { return kHostInterestAddress + item; }

bool path(FixedString<96>* out, const char* dir, StringView file) {
  out->clear();
  return out->append(dir) && out->push_back('/') && out->append(file);
}

}  // namespace

Ath10kChip::Ath10kChip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator)
    : model_(model), bus_(bus), allocator_(allocator), firmware_file_(allocator), board_file_(allocator) {}

Ath10kChip::~Ath10kChip() { stop(); }

WifiChipResult Ath10kChip::fail(WifiChipResult result, const char* message) {
  message_ = message;
  if (result != WifiChipResult::Unsupported) state_ = WifiChipState::Failed;
  return result;
}

void Ath10kChip::release() {
  bmi_ = UniquePointer<Ath10kBmi>();
  transport_ = UniquePointer<Ath10kPciBmi>();
  pci_ = UniquePointer<Ath10kPci>();
}

void Ath10kChip::stop() {
  if (pci_ && state_ != WifiChipState::Off) {
    // ath10k_pci_hif_power_down: the soft (warm) reset stops the target's DMA before the rings go away
    (void)pci_->warm_reset();
    pci_->sleep();
  }
  release();
  state_ = WifiChipState::Off;
}

void Ath10kChip::read_subsystem_ids() {
  subsystem_vendor_ = subsystem_device_ = 0;
  if (!bus_.pci) return;
  const uint32_t value = bus_.pci->read32(bus_.pci_device.address, kPciSubsystemOffset);
  if (value == 0xFFFFFFFFu) return;
  subsystem_vendor_ = static_cast<uint16_t>(value);
  subsystem_device_ = static_cast<uint16_t>(value >> 16);
}

WifiChipResult Ath10kChip::reset_chip() {
  Ath10kTargetInit init = Ath10kTargetInit::Timeout;
  if (model_.device_id == kQca6174 || model_.device_id == kQca6164 || model_.device_id == kQca9377) {
    init = pci_->qca6174_chip_reset();
  } else {
    // ath10k_pci_qca988x_chip_reset: warm resets first (some boards do not survive a cold one), then a cold reset. The driver also checks that the copy engine
    // answers a diagnostic read after a warm reset; that check is not done here.
    for (uint32_t i = 0; i < kWarmResetAttempts && init != Ath10kTargetInit::Ok; ++i) init = pci_->warm_reset();
    if (init != Ath10kTargetInit::Ok) {
      pci_->cold_reset();
      init = pci_->wait_for_target_init();
    }
  }
  switch (init) {
    case Ath10kTargetInit::Ok: return WifiChipResult::Ok;
    case Ath10kTargetInit::Gone: return fail(WifiChipResult::NoResponse, "the chip reads all ones after its reset");
    case Ath10kTargetInit::Crashed: return fail(WifiChipResult::NoResponse, "the target crashed during its reset");
    case Ath10kTargetInit::Timeout: return fail(WifiChipResult::Timeout, "the target did not set FW_IND_INITIALIZED after its reset");
  }
  return fail(WifiChipResult::NoResponse, "reset failed");
}

WifiChipResult Ath10kChip::load_firmware(IWifiFirmwareSource& source) {
  bool any_file = false;
  for (uint32_t api = kFwApiMax; api >= kFwApiMin; --api) {
    FixedString<20> file;
    if (!file.append("firmware-") || !file.append_uint(api) || !file.append(".bin")) continue;
    FixedString<96> name;
    if (!path(&name, hw_params_->dir, file)) continue;
    if (!source.read(name.c_str(), firmware_file_)) continue;
    any_file = true;
    if (ath10k_parse_firmware(firmware_file_.view(), &fw_) != AthParse::Ok) continue;    // the driver goes on to the next API too
    firmware_name_ = name;
    return WifiChipResult::Ok;
  }
  fw_ = Ath10kFirmwareFile{};
  firmware_file_.clear();
  if (any_file) return fail(WifiChipResult::FirmwareInvalid, "no firmware-N.bin of the chip's folder is a valid ath10k firmware file");
  return fail(WifiChipResult::FirmwareMissing, "no ath10k firmware-N.bin for this chip in /firmware");
}

WifiChipResult Ath10kChip::query_otp_board_id() {
  // ath10k_core_get_board_id_from_otp: run the OTP program with BMI_PARAM_GET_EEPROM_BOARD_ID; the answer has the board id in bits 10..14 and the chip id in 15..16,
  // and is valid when the status byte is 0 and the board id is not.
  board_ids_.bmi_ids_valid = false;
  if (fw_.otp.empty()) return WifiChipResult::Ok;
  if (bmi_->fast_download(hw_params_->patch_load_addr, fw_.otp) != BmiResult::Ok) return fail(WifiChipResult::Timeout, "BMI: the OTP image could not be sent");
  uint32_t result = 0;
  if (bmi_->execute(hw_params_->patch_load_addr, kBmiParamGetEepromBoardId, &result) != BmiResult::Ok)
    return fail(WifiChipResult::Timeout, "BMI: the OTP board id query did not answer");
  const uint32_t board_id = (result & kBmiBoardIdFromOtpMask) >> kBmiBoardIdFromOtpShift;
  const uint32_t chip_id = (result & kBmiChipIdFromOtpMask) >> kBmiChipIdFromOtpShift;
  if ((result & kBmiBoardIdStatusMask) == 0 && board_id != 0) {
    board_ids_.bmi_ids_valid = true;
    board_ids_.bmi_board_id = board_id;
    board_ids_.bmi_chip_id = chip_id;
  }
  return WifiChipResult::Ok;
}

WifiChipResult Ath10kChip::load_board(IWifiFirmwareSource& source) {
  board_name_.clear();
  board_data_ = ConstByteView();
  board_ids_.bus = "pci";
  board_ids_.vendor = model_.vendor_id;
  board_ids_.device = model_.device_id;
  board_ids_.subsystem_vendor = subsystem_vendor_;
  board_ids_.subsystem_device = subsystem_device_;
  FixedString<96> name;
  if (path(&name, hw_params_->dir, "board-2.bin") && source.read(name.c_str(), board_file_)) {
    AthBoardName tried;
    ConstByteView data;
    const AthBoardFind found = ath_select_board(board_file_.view(), AthFamily::Ath10k, AthBdfSearch::Default, kAthBoardIeBoard, board_ids_, &data, &tried);
    if (found == AthBoardFind::Found && !data.empty()) {
      board_name_ = tried;
      board_file_name_ = name;
      board_data_ = data;
      return WifiChipResult::Ok;
    }
    // not there (or the file is damaged): API 1, as the driver does
  }
  if (path(&name, hw_params_->dir, hw_params_->board) && source.read(name.c_str(), board_file_)) {
    board_file_name_ = name;
    board_data_ = board_file_.view();
    return WifiChipResult::Ok;
  }
  board_file_.clear();
  return fail(WifiChipResult::FirmwareMissing, "no board data for this card (board-2.bin has no entry for its IDs and there is no board.bin)");
}

WifiChipResult Ath10kChip::download() {
  uint32_t value = 0;
  // ath10k_init_configure_target: the HTC version, then the firmware mode in hi_option_flag (one device, AP mode as the driver sets it, global MAC addresses)
  if (bmi_->write32(hi(kHiAppHostInterest), kHtcProtocolVersion) != BmiResult::Ok || bmi_->read32(hi(kHiOptionFlag), &value) != BmiResult::Ok)
    return fail(WifiChipResult::Timeout, "BMI: configuring the target failed");
  value |= (1u << kHiOptionNumDevShift) | (kHiOptionFwModeAp << kHiOptionFwModeShift) | (1u << kHiOptionMacAddrMethodShift);
  if (bmi_->write32(hi(kHiOptionFlag), value) != BmiResult::Ok) return fail(WifiChipResult::Timeout, "BMI: configuring the target failed");

  // ath10k_download_board_data
  uint32_t board_address = 0;
  if (bmi_->read32(hi(kHiBoardData), &board_address) != BmiResult::Ok) return fail(WifiChipResult::Timeout, "BMI: reading hi_board_data failed");
  const uint64_t board_length = board_data_.size() < hw_params_->board_size ? board_data_.size() : hw_params_->board_size;
  if (board_address == 0) return fail(WifiChipResult::FirmwareRejected, "the target reported no board data address");
  if (bmi_->write_memory(board_address, board_data_.first(board_length)) != BmiResult::Ok || bmi_->write32(hi(kHiBoardDataInitialized), 1) != BmiResult::Ok)
    return fail(WifiChipResult::Timeout, "BMI: writing the board data failed");

  // ath10k_download_and_run_otp
  if (!fw_.otp.empty()) {
    if (bmi_->fast_download(hw_params_->patch_load_addr, fw_.otp) != BmiResult::Ok) return fail(WifiChipResult::Timeout, "BMI: the OTP image could not be sent");
    uint32_t result = 0;
    if (bmi_->execute(hw_params_->patch_load_addr, hw_params_->otp_exe_param, &result) != BmiResult::Ok)
      return fail(WifiChipResult::Timeout, "BMI: running the OTP program did not answer");
    if (result != 0 && !fw_.has_feature(kAth10kFeatureIgnoreOtpResult)) return fail(WifiChipResult::FirmwareRejected, "the OTP calibration failed");
  }
  if (fw_.has_feature(kAth10kFeatureSkipClockInit) && bmi_->write32(hi(kHiSkipClockInit), 1) != BmiResult::Ok)
    return fail(WifiChipResult::Timeout, "BMI: hi_skip_clock_init could not be written");

  // ath10k_download_fw (BMI only: the diagnostic CE download of newer chips is not used)
  if (bmi_->fast_download(hw_params_->patch_load_addr, fw_.firmware) != BmiResult::Ok) return fail(WifiChipResult::Timeout, "BMI: the firmware image could not be sent");
  // ath10k_init_uart without the uart_print option: the firmware's debug UART stays off (hi_serial_enable at 0x14)
  if (bmi_->write32(hi(0x14), 0) != BmiResult::Ok) return fail(WifiChipResult::Timeout, "BMI: hi_serial_enable could not be written");
  if (bmi_->done() != BmiResult::Ok) return fail(WifiChipResult::Timeout, "BMI: DONE was not delivered");
  return WifiChipResult::Ok;
}

WifiChipResult Ath10kChip::start(IWifiFirmwareSource& firmware) {
  stop();
  identity_ = WifiChipIdentity{};
  firmware_name_.clear();
  board_file_name_.clear();
  hw_params_ = nullptr;
  target_version_ = 0;
  board_ids_ = AthBoardIds{};
  fw_ = Ath10kFirmwareFile{};
  if (!bus_.registers) return fail(WifiChipResult::NoRegisters, "no register window for the chip");
  regs_ = ath10k_regs_for_device(model_.device_id);
  if (!regs_) return fail(WifiChipResult::Unsupported, "ath10k: the QCA99X0/QCA9984/QCA9888 register map is not written");
  pci_ = UniquePointer<Ath10kPci>::make(allocator_, *bus_.registers, *regs_, ath10k_device_power_save(model_.device_id));
  if (!pci_) return fail(WifiChipResult::NoResponse, "out of memory");
  state_ = WifiChipState::Off;
  if (!pci_->wake()) return fail(WifiChipResult::NoResponse, "the SoC did not wake up (RTC_STATE)");

  if (const WifiChipResult r = reset_chip(); r != WifiChipResult::Ok) return r;
  const uint32_t chip_id = pci_->chip_id();
  if (chip_id == 0xFFFFFFFFu) return fail(WifiChipResult::NoResponse, "SOC_CHIP_ID reads all ones");
  identity_.chip_id = chip_id;
  identity_.revision = (chip_id & kSocChipIdRevMask) >> kSocChipIdRevShift;
  if (!ath10k_chip_supported(model_.device_id, chip_id)) return fail(WifiChipResult::UnknownRevision, "this chip revision is not supported by ath10k");
  hw_params_ = ath10k_hw_params_from_chip_id(model_.device_id, chip_id);
  read_subsystem_ids();
  state_ = WifiChipState::Identified;
  message_ = "chip identified";

  // BMI over copy engines 0 and 1
  transport_ = UniquePointer<Ath10kPciBmi>::make(allocator_, *pci_, allocator_);
  if (!transport_ || !transport_->init()) return fail(WifiChipResult::NoResponse, "out of DMA memory for the BMI copy engines");
  bmi_ = UniquePointer<Ath10kBmi>::make(allocator_, *transport_);
  if (!bmi_) return fail(WifiChipResult::NoResponse, "out of memory");
  pci_->wake_target_cpu();
  bmi_->start();
  BmiTargetInfo info;
  if (bmi_->get_target_info(&info) != BmiResult::Ok) return fail(WifiChipResult::Timeout, "BMI: GET_TARGET_INFO did not answer");
  target_version_ = info.version;
  hw_params_ = ath10k_find_hw_params(info.version, model_.device_id);
  if (!hw_params_) return fail(WifiChipResult::UnknownRevision, "ath10k has no hardware parameters for this target version");

  if (const WifiChipResult r = load_firmware(firmware); r != WifiChipResult::Ok) return r;
  if (fw_.has_feature(kAth10kFeatureNonBmi)) return fail(WifiChipResult::FirmwareInvalid, "the firmware is for a chip without BMI");
  if (!fw_.code_swap.empty()) return fail(WifiChipResult::Unsupported, "ath10k: firmware with a code swap image is not supported (use firmware-6.bin)");
  identity_.firmware_version[0] = '\0';
  for (uint64_t i = 0; i < fw_.version.size() && i + 1 < sizeof identity_.firmware_version; ++i) {
    identity_.firmware_version[i] = fw_.version[i];
    identity_.firmware_version[i + 1] = '\0';
  }
  if (const WifiChipResult r = query_otp_board_id(); r != WifiChipResult::Ok) return r;
  if (const WifiChipResult r = load_board(firmware); r != WifiChipResult::Ok) return r;
  if (const WifiChipResult r = download(); r != WifiChipResult::Ok) return r;
  state_ = WifiChipState::FirmwareLoaded;
  return fail(WifiChipResult::Unsupported, "ath10k: firmware started (BMI done); HTC and WMI over the copy engines are not written");
}

}  // namespace hardware::ath
