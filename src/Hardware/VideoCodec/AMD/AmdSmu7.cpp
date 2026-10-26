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


#include "Hardware/VideoCodec/AMD/AmdSmu7.hpp"

namespace hardware {

namespace {
// smu_7_1_3_d.h
constexpr uint32_t mmSMC_MESSAGE_0 = 0x94, mmSMC_RESP_0 = 0x95, mmSMC_MSG_ARG_0 = 0xa4;
constexpr uint32_t mmSMC_IND_INDEX_11 = 0x1AC, mmSMC_IND_DATA_11 = 0x1AD, mmSMC_IND_ACCESS_CNTL = 0x92;
constexpr uint32_t kAutoIncrementInd11 = 0x800;   // SMC_IND_ACCESS_CNTL__AUTO_INCREMENT_IND_11_MASK
constexpr uint32_t kRespMask = 0xFFFF;   // SMC_RESP_0__SMC_RESP_MASK
}  // namespace

bool AmdSmu7::wait_for_result(uint32_t timeout_us, uint32_t* value) {
  const uint64_t begin = clock_.now_us();
  do {
    const uint32_t raw = registers_.read(mmSMC_RESP_0);
    if (raw == 0xFFFFFFFFu) return false;   // nothing answers at this address
    if ((raw & kRespMask) != 0) {
      *value = raw & kRespMask;
      return true;
    }
  } while (clock_.now_us() - begin < timeout_us);
  return false;
}

AmdSmu7::Result AmdSmu7::send(uint16_t message, uint32_t* response, uint32_t timeout_us) { return send_message(message, nullptr, response, timeout_us); }

AmdSmu7::Result AmdSmu7::send_with_parameter(uint16_t message, uint32_t argument, uint32_t* response, uint32_t timeout_us) {
  return send_message(message, &argument, response, timeout_us);
}

AmdSmu7::Result AmdSmu7::send_message(uint16_t message, const uint32_t* argument, uint32_t* response, uint32_t timeout_us) {
  uint32_t value = 0;
  // The driver first waits for the previous message to finish (PHM_WAIT_FIELD_UNEQUAL(SMC_RESP_0, SMC_RESP, 0)). A mailbox that is stuck at zero is no SMC: nothing is written.
  if (!wait_for_result(timeout_us, &value)) return Result::NotAnswering;
  if (argument) registers_.write(mmSMC_MSG_ARG_0, *argument);
  registers_.write(mmSMC_RESP_0, 0);
  registers_.write(mmSMC_MESSAGE_0, message);
  if (!wait_for_result(timeout_us, &value)) return Result::NotAnswering;
  if (response) *response = registers_.read(mmSMC_MSG_ARG_0);
  if (value == kResultOk) return Result::Ok;
  return value == kResultUnknownCommand ? Result::Unsupported : Result::Failed;
}

AmdSmu7::Result AmdSmu7::uvd_power(bool on, bool dynamic_power_gating) {
  return on ? send_with_parameter(kMsgUvdPowerOn, dynamic_power_gating ? 1 : 0) : send(kMsgUvdPowerOff);
}

AmdSmu7::Result AmdSmu7::vce_power(bool on) { return send(on ? kMsgVcePowerOn : kMsgVcePowerOff); }

bool AmdSmu7::set_sram_address(uint32_t address) {
  if ((address & 3) != 0 || address > kSmcRamEnd - 4) return false;
  registers_.write(mmSMC_IND_INDEX_11, address);
  registers_.write(mmSMC_IND_ACCESS_CNTL, registers_.read(mmSMC_IND_ACCESS_CNTL) & ~kAutoIncrementInd11);
  return true;
}

bool AmdSmu7::read_sram_dword(uint32_t address, uint32_t* value) {
  if (!set_sram_address(address)) return false;
  *value = registers_.read(mmSMC_IND_DATA_11);
  return true;
}

bool AmdSmu7::write_sram_dword(uint32_t address, uint32_t value) {
  if (!set_sram_address(address)) return false;
  registers_.write(mmSMC_IND_DATA_11, value);
  return true;
}

namespace {
AmdSmu7::DpmResult dpm_result(AmdSmu7::Result r) {
  switch (r) {
    case AmdSmu7::Result::Ok: return AmdSmu7::DpmResult::Ok;
    case AmdSmu7::Result::NotAnswering: return AmdSmu7::DpmResult::NotAnswering;
    case AmdSmu7::Result::Unsupported: return AmdSmu7::DpmResult::Unsupported;
    case AmdSmu7::Result::Failed: break;
  }
  return AmdSmu7::DpmResult::Failed;
}
}  // namespace

AmdSmu7::DpmResult AmdSmu7::dpm_enable(uint32_t count_offset, uint32_t boot_offset, uint32_t max_levels, bool top, bool mask, uint16_t mask_message, uint16_t enable_message,
                                       uint32_t* level) {
  // vegam_process_firmware_header: where the SMC keeps its DPM table.
  uint32_t table = 0;
  if (!read_sram_dword(kFirmwareHeaderLocation + kHeaderDpmTableOffset, &table) || table == 0 || table == 0xFFFFFFFFu || (table & 3) != 0) return DpmResult::NoDpmTable;
  if (table > kSmcRamEnd || boot_offset >= kSmcRamEnd - table || count_offset >= kSmcRamEnd - table) return DpmResult::BadDpmTable;
  // The byte of a field in the big-endian dword that holds it.
  const uint32_t count_address = table + count_offset, boot_address = table + boot_offset;
  uint32_t word = 0;
  if (!read_sram_dword(count_address & ~3u, &word)) return DpmResult::BadDpmTable;
  const uint32_t count = (word >> (24 - 8 * (count_address & 3))) & 0xFF;
  if (count == 0 || count > max_levels) return DpmResult::BadDpmTable;
  const uint32_t boot = top ? count - 1 : 0;
  // vegam_update_vce_smc_table / _uvd_: the boot level into its byte of the table.
  const uint32_t shift = 24 - 8 * (boot_address & 3);
  if (!read_sram_dword(boot_address & ~3u, &word)) return DpmResult::BadDpmTable;
  word = (word & ~(0xFFu << shift)) | (boot << shift);
  if (!write_sram_dword(boot_address & ~3u, word)) return DpmResult::BadDpmTable;
  if (mask) {
    const Result sent = send_with_parameter(mask_message, 1u << boot);
    if (sent != Result::Ok) return dpm_result(sent);
  }
  if (level) *level = boot;
  return dpm_result(send(enable_message));
}

AmdSmu7::DpmResult AmdSmu7::vce_dpm_enable(VceLevel which, uint32_t* level) {
  const bool top = which == VceLevel::Highest;
  return dpm_enable(kDpmVceLevelCountOffset, kDpmVceBootLevelOffset, kMaxVceLevels, top, top, kMsgVceDpmSetEnabledMask, kMsgVceDpmEnable, level);
}

AmdSmu7::DpmResult AmdSmu7::uvd_dpm_enable(UvdLevel which, uint32_t* level) {
  return dpm_enable(kDpmUvdLevelCountOffset, kDpmUvdBootLevelOffset, kMaxUvdLevels, true, which == UvdLevel::Highest, kMsgUvdDpmSetEnabledMask, kMsgUvdDpmEnable, level);
}

AmdSmu7::Result AmdSmu7::vce_dpm_disable() { return send(kMsgVceDpmDisable); }

AmdSmu7::Result AmdSmu7::uvd_dpm_disable() { return send(kMsgUvdDpmDisable); }

}  // namespace hardware
