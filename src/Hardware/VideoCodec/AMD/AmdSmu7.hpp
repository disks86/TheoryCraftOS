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


#pragma once
#include <stdint.h>
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace hardware {

// The message interface to the system management controller (SMC) of the Polaris-family graphics cards (SMU 7.x; the Radeon RX Vega M of a Kaby Lake-G machine), ported from
// the Linux amdgpu driver's smu7_smumgr.c (smu7_send_msg_to_smc, _with_parameter, smu7_get_argument) with the message numbers of smu7_ppsmc.h and the register numbers of
// smu_7_1_3_d.h. The SMC is a small processor on the card that owns its power and clocks; the driver asks it for things by writing a message number to a register and
// waiting for a result word. The registers are the card's register BAR (BAR 5), numbered by dword.
//
// On top of the mailbox, the one thing the video engine needs: power for the Unified Video Decoder (smu7_clockpowergating.c: smu7_powerup_uvd / smu7_powerdown_uvd). This does not
// set clocks (the driver does that through the SMC's DPM tables or the PLL, neither of which this OS has) and does not know whether the SMC firmware is running: a mailbox
// that never answers is reported as such, and nothing is written to it then.
class AmdSmu7 {
 public:
  enum class Result : uint8_t {
    Ok,
    NotAnswering,   // the previous message never finished, or this one got no answer: the SMC is not running (or the registers do not answer)
    Unsupported,    // PPSMC_Result_UnknownCmd
    Failed,         // any other result word
  };
  static constexpr uint16_t kMsgUvdPowerOff = 0x60, kMsgUvdPowerOn = 0x61;   // PPSMC_MSG_UVDPowerOFF / ON
  static constexpr uint16_t kResultOk = 0x01, kResultUnknownCommand = 0xFE;
  static constexpr uint32_t kDefaultTimeoutUs = 100000;                        // adev->usec_timeout

  AmdSmu7(IVcnRegisters& registers, const IClock& clock) : registers_(registers), clock_(clock) {}

  // smu7_send_msg_to_smc: sends `message` (the argument register is left alone), waits for the result. `response` (if given) receives the argument register afterwards.
  Result send(uint16_t message, uint32_t* response = nullptr, uint32_t timeout_us = kDefaultTimeoutUs);
  // smu7_send_msg_to_smc_with_parameter: the same with the argument register written first.
  Result send_with_parameter(uint16_t message, uint32_t argument, uint32_t* response = nullptr, uint32_t timeout_us = kDefaultTimeoutUs);
  // smu7_powerup_uvd / smu7_powerdown_uvd. `dynamic_power_gating` is the platform capability the driver passes as the argument of the power-on message.
  Result uvd_power(bool on, bool dynamic_power_gating = false);

 private:
  // Waits until the result register is not zero (the SMC has finished what it was doing); false on timeout. A register that reads all ones is not an SMC.
  bool wait_for_result(uint32_t timeout_us, uint32_t* value);
  Result send_message(uint16_t message, const uint32_t* argument, uint32_t* response, uint32_t timeout_us);

  IVcnRegisters& registers_;
  const IClock& clock_;
};

}  // namespace hardware
