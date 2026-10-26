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

}  // namespace hardware
