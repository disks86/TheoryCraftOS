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

#include "Hardware/WirelessInterface/Qualcomm/AthMhi.hpp"
#include "Kernel/Delay.hpp"

namespace hardware::ath {

using namespace mhi_reg;

namespace {
constexpr uint32_t kPollUs = 1000;
}  // namespace

const char* mhi_state_name(uint32_t state) {
  switch (state) {
    case 0: return "RESET";
    case 1: return "READY";
    case 2: return "M0";
    case 3: return "M1";
    case 4: return "M2";
    case 5: return "M3";
    case 6: return "M3_FAST";
    case 7: return "BHI";
    case 0xFF: return "SYS ERROR";
    default: return "?";
  }
}

const char* mhi_exec_env_name(uint32_t ee) {
  switch (ee) {
    case 0: return "PRIMARY BOOTLOADER";
    case 1: return "SECONDARY BOOTLOADER";
    case 2: return "MISSION MODE";
    case 3: return "RAMDUMP DOWNLOAD MODE";
    case 4: return "WLAN FIRMWARE";
    case 5: return "PASS THROUGH";
    case 6: return "EMERGENCY DOWNLOAD";
    case 7: return "FLASH PROGRAMMER";
    default: return "?";
  }
}

MhiResult AthMhi::locate_bhi() {
  const uint32_t offset = bar_.read32(kBhiOff);
  if (offset == 0xFFFFFFFFu) return MhiResult::Gone;
  // the block must lie inside the registers with all its words (up to the serial number)
  if (offset >= register_length_ || register_length_ - offset <= kBhiSerialNumber || (offset & 3)) return MhiResult::BadOffset;
  bhi_ = offset;
  return MhiResult::Ok;
}

void AthMhi::request_reset() {
  bar_.write32(kMhiCtrl, kMhiCtrlReset);
  delay_us(10000);
}

MhiResult AthMhi::reset_and_wait(uint32_t timeout_ms) {
  bar_.write32(kMhiCtrl, bar_.read32(kMhiCtrl) | kMhiCtrlReset);
  for (uint32_t waited = 0;; waited += kPollUs / 1000) {
    const uint32_t ctrl = bar_.read32(kMhiCtrl);
    if (ctrl == 0xFFFFFFFFu) return MhiResult::Gone;
    if (!(ctrl & kMhiCtrlReset)) break;
    if (waited >= timeout_ms) return MhiResult::Timeout;
    delay_us(kPollUs);
  }
  if (bhi_) bar_.write32(bhi_ + kBhiIntVec, 0);
  return MhiResult::Ok;
}

MhiResult AthMhi::wait_ready(uint32_t timeout_ms) {
  for (uint32_t waited = 0;; waited += kPollUs / 1000) {
    const uint32_t status = bar_.read32(kMhiStatus);
    if (status == 0xFFFFFFFFu) return MhiResult::Gone;
    if ((status & kMhiStatusReady) && !(status & kMhiStatusSysErr)) return MhiResult::Ok;
    if (waited >= timeout_ms) return MhiResult::Timeout;
    delay_us(kPollUs);
  }
}

MhiResult AthMhi::load_bhi(ConstByteView image, uint32_t session_id, uint32_t timeout_ms) {
  if (!bhi_) return MhiResult::BadOffset;
  if (image.empty() || image.size() > 0xFFFFFFFFull) return MhiResult::ImageError;
  const uint64_t address = nic_dma_address(image);
  session_id &= kBhiTxDbSeqMask;
  if (session_id == 0) session_id = 1;     // MHI_RANDOM_U32_NONZERO
  bar_.write32(bhi_ + kBhiStatus, 0);
  bar_.write32(bhi_ + kBhiImgAddrHigh, static_cast<uint32_t>(address >> 32));
  bar_.write32(bhi_ + kBhiImgAddrLow, static_cast<uint32_t>(address));
  bar_.write32(bhi_ + kBhiImgSize, static_cast<uint32_t>(image.size()));
  bar_.write32(bhi_ + kBhiImgTxDb, session_id);
  for (uint32_t waited = 0;; waited += kPollUs / 1000) {
    const uint32_t raw = bar_.read32(bhi_ + kBhiStatus);
    if (raw == 0xFFFFFFFFu) return MhiResult::Gone;
    const uint32_t status = (raw & kBhiStatusMask) >> kBhiStatusShift;
    if (status == kBhiStatusSuccess) return MhiResult::Ok;
    if (status == kBhiStatusError) return MhiResult::ImageError;
    if (sys_error()) return MhiResult::ImageError;
    if (waited >= timeout_ms) return MhiResult::Timeout;
    delay_us(kPollUs);
  }
}

uint32_t ath_window_read32(INicRegisters& bar, uint32_t offset) {
  if (offset < kAthWindowStart) return bar.read32(offset);
  const uint32_t window = (offset & kAthWindowValueMask) >> kAthWindowValueShift;
  bar.write32(kAthWindowRegAddress, kAthWindowEnable | window);
  (void)bar.read32(kAthWindowRegAddress);
  return bar.read32(kAthWindowStart + (offset & kAthWindowRangeMask));
}

}  // namespace hardware::ath
