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

#include "Hardware/WirelessInterface/Qualcomm/Ath11kChip.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware::ath {

namespace {

constexpr uint16_t kPciSubsystemOffset = 0x2C;
constexpr uint32_t kBhiSessionId = 0x0A11CE5;   // any non-zero 30-bit value (Linux picks a random one)

bool path(FixedString<96>* out, const char* dir, StringView file) {
  out->clear();
  return out->append(dir) && out->push_back('/') && out->append(file);
}

uint32_t major_of(uint32_t version) { return (version & kTcsrSocHwVersionMajorMask) >> kTcsrSocHwVersionMajorShift; }
uint32_t minor_of(uint32_t version) { return version & kTcsrSocHwVersionMinorMask; }

struct BoardCount {
  StringView prefix;      // empty: count every BOARD entry
  uint32_t boards = 0, matching = 0, regdb = 0;
};

bool count_board(void* context, uint32_t ie_type, StringView name, ConstByteView) {
  BoardCount& count = *static_cast<BoardCount*>(context);
  if (ie_type == kAthBoardIeRegdb) {
    ++count.regdb;
  } else if (ie_type == kAthBoardIeBoard) {
    ++count.boards;
    if (count.prefix.empty() || name.starts_with(count.prefix)) ++count.matching;
  }
  return true;
}

}  // namespace

const char* ath11k_firmware_dir(uint16_t device_id, uint32_t soc_hw_version) {
  const uint32_t major = major_of(soc_hw_version), minor = minor_of(soc_hw_version);
  switch (device_id) {
    case kQca6390: return major == 2 ? "ath11k/QCA6390/hw2.0" : nullptr;
    case kWcn6855:
      if (major != 2) return nullptr;
      if (minor == 0x00 || minor == 0x01) return "ath11k/WCN6855/hw2.0";
      if (minor == 0x10 || minor == 0x11) return "ath11k/WCN6855/hw2.1";
      return nullptr;
    case kQcn9074: return "ath11k/QCN9074/hw1.0";    // the driver reads no version for it
    case kWcn7850: return major == 2 ? "ath12k/WCN7850/hw2.0" : nullptr;
    case kQcn9274: return major == 2 ? "ath12k/QCN9274/hw2.0" : major == 1 ? "ath12k/QCN9274/hw1.0" : nullptr;
    default: return nullptr;
  }
}

AthBdfSearch ath11k_bdf_search(uint16_t device_id) {
  return device_id == kWcn6855 || device_id == kWcn7850 ? AthBdfSearch::BusAndBoard : AthBdfSearch::Default;
}

Ath11kChip::Ath11kChip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator)
    : model_(model), bus_(bus), allocator_(allocator), firmware_file_(allocator), board_file_(allocator) {}

Ath11kChip::~Ath11kChip() { stop(); }

WifiChipResult Ath11kChip::fail(WifiChipResult result, const char* message) {
  message_ = message;
  if (result != WifiChipResult::Unsupported) state_ = WifiChipState::Failed;
  return result;
}

void Ath11kChip::stop() {
  if (bus_.registers && state_ != WifiChipState::Off) {
    // ath11k_mhi_set_mhictrl_reset: the device leaves its MHI state; it has no DMA of ours outstanding (the SBL buffer is freed after BHI)
    bus_.registers->write32(mhi_reg::kMhiCtrl, mhi_reg::kMhiCtrlReset);
  }
  state_ = WifiChipState::Off;
}

uint32_t Ath11kChip::read_hw_version() {
  switch (model_.device_id) {
    case kQca6390:
    case kWcn6855: return bus_.registers->read32(kAth11kTcsrSocHwVersion);
    case kWcn7850:
    case kQcn9274: return ath_window_read32(*bus_.registers, kAth12kTcsrSocHwVersion);
    default: return 0;
  }
}

WifiChipResult Ath11kChip::load_firmware(IWifiFirmwareSource& source) {
  amss_ = ConstByteView();
  FixedString<96> name;
  if (path(&name, dir_, "firmware-2.bin") && source.read(name.c_str(), firmware_file_)) {
    AthQcomFirmwareFile file;
    const AthParse parsed = ath_parse_qcom_firmware(firmware_file_.view(), is_ath12k() ? kAth12kFirmwareMagic : kAth11kFirmwareMagic, &file);
    if (parsed != AthParse::Ok) return fail(WifiChipResult::FirmwareInvalid, "firmware-2.bin is not a valid firmware container");
    if (!ath_is_elf32(file.amss)) return fail(WifiChipResult::FirmwareInvalid, "the AMSS image in firmware-2.bin is not an ELF file");
    amss_ = file.amss;
    firmware_name_ = name;
    return WifiChipResult::Ok;
  }
  if (path(&name, dir_, "amss.bin") && source.read(name.c_str(), firmware_file_)) {
    if (!ath_is_elf32(firmware_file_.view())) return fail(WifiChipResult::FirmwareInvalid, "amss.bin is not an ELF file");
    amss_ = firmware_file_.view();
    firmware_name_ = name;
    return WifiChipResult::Ok;
  }
  firmware_file_.clear();
  return fail(WifiChipResult::FirmwareMissing, "neither firmware-2.bin nor amss.bin of this chip is in /firmware");
}

WifiChipResult Ath11kChip::load_board(IWifiFirmwareSource& source) {
  board_entries_ = matching_entries_ = regdb_entries_ = 0;
  FixedString<96> name;
  if (path(&name, dir_, "board-2.bin") && source.read(name.c_str(), board_file_)) {
    // the names start with the PCI IDs when the search is by bus and board; the QMI part comes from the firmware
    AthBoardIds ids;
    ids.vendor = model_.vendor_id;
    ids.device = model_.device_id;
    ids.subsystem_vendor = subsystem_vendor_;
    ids.subsystem_device = subsystem_device_;
    AthBoardName full;
    BoardCount count;
    const AthBdfSearch search = ath11k_bdf_search(model_.device_id);
    if (search == AthBdfSearch::BusAndBoard && ath11k_board_name(ids, search, false, false, false, &full)) {
      const uint64_t at = full.view().find(",qmi-chip-id=");
      if (at != StringView::npos) count.prefix = full.view().first(at + 1);
    }
    if (!ath_board_for_each(board_file_.view(), is_ath12k() ? kAth12kBoardMagic : kAth11kBoardMagic, &count_board, &count))
      return fail(WifiChipResult::FirmwareInvalid, "board-2.bin is damaged");
    board_entries_ = count.boards;
    matching_entries_ = count.matching;
    regdb_entries_ = count.regdb;
    if (board_entries_ == 0) return fail(WifiChipResult::FirmwareInvalid, "board-2.bin holds no board data");
    return WifiChipResult::Ok;
  }
  if (path(&name, dir_, "board.bin") && source.read(name.c_str(), board_file_)) {
    board_entries_ = matching_entries_ = 1;
    return WifiChipResult::Ok;
  }
  board_file_.clear();
  return fail(WifiChipResult::FirmwareMissing, "neither board-2.bin nor board.bin of this chip is in /firmware");
}

WifiChipResult Ath11kChip::boot_sbl() {
  INicRegisters& bar = *bus_.registers;
  // ath11k_pci_sw_reset (power on): the SoC global reset and MHICTRL.RESET. The link (LTSSM), interrupt clearing, WLAON power control, L1SS fix and debug register
  // clearing it also does are not written.
  const uint32_t reset = bar.read32(kAth11kPcieSocGlobalReset);
  if (reset == 0xFFFFFFFFu) return fail(WifiChipResult::NoResponse, "the chip reads all ones");
  bar.write32(kAth11kPcieSocGlobalReset, reset | kAth11kPcieSocGlobalResetV);
  delay_us(10000);
  bar.write32(kAth11kPcieSocGlobalReset, reset & ~kAth11kPcieSocGlobalResetV);
  delay_us(10000);
  if (bar.read32(kAth11kPcieSocGlobalReset) == 0xFFFFFFFFu) return fail(WifiChipResult::NoResponse, "the link went down during the SoC reset");
  AthMhi mhi(bar, kRegisterLength);
  mhi.request_reset();

  // mhi_prepare_for_power_up / mhi_async_power_up
  switch (mhi.locate_bhi()) {
    case MhiResult::Ok: break;
    case MhiResult::Gone: return fail(WifiChipResult::NoResponse, "MHI: the registers read all ones");
    default: return fail(WifiChipResult::NoResponse, "MHI: BHIOFF points outside the registers");
  }
  exec_env_ = mhi.exec_env();
  const auto ee = static_cast<MhiExecEnv>(exec_env_);
  const bool in_pbl = ee == MhiExecEnv::Pbl || ee == MhiExecEnv::Pthru || ee == MhiExecEnv::Edl;
  if (!in_pbl && ee != MhiExecEnv::Amss) return fail(WifiChipResult::NoResponse, "MHI: the device is in no execution environment it can be powered up from");
  if (mhi.mhi_state() == static_cast<uint32_t>(MhiState::SysErr) || mhi.sys_error()) {
    if (mhi.reset_and_wait() != MhiResult::Ok) return fail(WifiChipResult::Timeout, "MHI: the device did not leave SYS_ERR");
  }
  if (ee == MhiExecEnv::Pbl) {
    // mhi_fw_load_handler with fbc_download: the SBL (the image's first 512 KiB) over BHI from DMA memory
    const uint64_t size = amss_.size() < kAthSblSize ? amss_.size() : kAthSblSize;
    ByteView sbl = allocate_dma_view<uint8_t>(allocator_, size, 4096);
    if (sbl.empty()) return fail(WifiChipResult::NoResponse, "out of DMA memory for the boot image");
    if (!sbl.copy_from(amss_.first(size))) {
      release_view(allocator_, sbl);
      return fail(WifiChipResult::NoResponse, "copying the boot image failed");
    }
    const MhiResult loaded = mhi.load_bhi(sbl, kBhiSessionId);
    release_view(allocator_, sbl);
    if (loaded == MhiResult::ImageError) return fail(WifiChipResult::FirmwareRejected, "MHI: the boot loader refused the SBL image (BHI_STATUS error)");
    if (loaded != MhiResult::Ok) return fail(WifiChipResult::Timeout, "MHI: the SBL download over BHI did not finish");
  } else if (ee != MhiExecEnv::Amss) {
    return fail(WifiChipResult::Unsupported, "MHI: pass-through and emergency download modes are not written");
  }
  // mhi_ready_state_transition: the device reports READY once the SBL (or the running firmware) has its MHI up
  if (mhi.wait_ready() != MhiResult::Ok) return fail(WifiChipResult::Timeout, "MHI: the device did not reach READY");
  return WifiChipResult::Ok;
}

WifiChipResult Ath11kChip::start(IWifiFirmwareSource& firmware) {
  stop();
  identity_ = WifiChipIdentity{};
  firmware_name_.clear();
  dir_ = nullptr;
  exec_env_ = 0;
  if (!bus_.registers) return fail(WifiChipResult::NoRegisters, "no register window for the chip");
  const uint32_t version = read_hw_version();
  if (version == 0xFFFFFFFFu) return fail(WifiChipResult::NoResponse, "TCSR_SOC_HW_VERSION reads all ones");
  identity_.chip_id = model_.device_id;
  identity_.revision = version & (kTcsrSocHwVersionMajorMask | kTcsrSocHwVersionMinorMask);
  dir_ = ath11k_firmware_dir(model_.device_id, version);
  if (!dir_) return fail(WifiChipResult::UnknownRevision, "this SoC hardware version is not supported");
  subsystem_vendor_ = subsystem_device_ = 0;
  if (bus_.pci) {
    const uint32_t value = bus_.pci->read32(bus_.pci_device.address, kPciSubsystemOffset);
    if (value != 0xFFFFFFFFu) {
      subsystem_vendor_ = static_cast<uint16_t>(value);
      subsystem_device_ = static_cast<uint16_t>(value >> 16);
    }
  }
  state_ = WifiChipState::Identified;
  message_ = "chip identified";
  if (const WifiChipResult r = load_firmware(firmware); r != WifiChipResult::Ok) return r;
  if (const WifiChipResult r = load_board(firmware); r != WifiChipResult::Ok) return r;
  if (const WifiChipResult r = boot_sbl(); r != WifiChipResult::Ok) return r;
  state_ = WifiChipState::FirmwareLoaded;
  return fail(WifiChipResult::Unsupported,
              is_ath12k() ? "ath12k: boot loader running (MHI READY); MHI context, BHIe download and QMI are not written"
                          : "ath11k: boot loader running (MHI READY); MHI context, BHIe download and QMI are not written");
}

}  // namespace hardware::ath
