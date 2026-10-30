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

// The Ice Lake host interface's own power handling (nhi_ops.c of the Linux driver, the vendor specific registers in the NHI's PCI configuration space). The controller is always there on an Ice
// Lake machine but its firmware runs only while the controller is "force powered" (which does not really power anything: the power is the ACPI power resources'); the LC mailbox
// asks the link controller to get ready for a reset or to enter Sx; the maximum latency tolerated goes to the LTR registers. The connection manager firmware is then the same as Titan
// Ridge's in its messages (ThunderboltIcm with TbGeneration::IceLake).
#pragma once
#include <stdint.h>
#include "Hardware/Bus/Thunderbolt/ThunderboltCio.hpp"

namespace hardware {

namespace tb_icl {
// the vendor specific registers (VS_CAP_n at 0xC8 + 4 * (n - 9)) and their bits
constexpr uint16_t kFwReadyRegister = 0xC8, kUuidLowRegister = 0xCC, kUuidHighRegister = 0xD0, kLtrRegister = 0xE0, kMaxLtrRegister = 0xE4, kLcDoneRegister = 0xEC, kLcCommandRegister = 0xF0,
                   kForcePowerRegister = 0xFC;
constexpr uint32_t kFwReady = 1u << 31, kLcDone = 1u << 0, kLcValid = 1u << 0, kLcCommandShift = 1, kLcCommandMask = 0x7Fu << 1;
constexpr uint32_t kForcePower = 1u << 1, kDmaDelayShift = 24, kDmaDelayMask = 0xFFu << 24, kDmaDelay = 0x22;
// icl_lc_mailbox_cmd
constexpr uint32_t kGoToSx = 0x02, kGoToSxNoWake = 0x03, kPrepareForReset = 0x21;
constexpr uint32_t kFwReadyTimeoutUs = 1050000, kLcTimeoutUs = 500000;
}  // namespace tb_icl

class TbIceLake {
 public:
  explicit TbIceLake(ITbConfigSpace& config) : config_(config) {}
  // "Force power" on or off. On waits for the firmware to say it is up and running (false if it does not).
  bool force_power(bool on, uint32_t timeout_us = tb_icl::kFwReadyTimeoutUs);
  // A command to the link controller and its completion.
  bool lc_mailbox(uint32_t command, uint32_t timeout_us = tb_icl::kLcTimeoutUs);
  // The same latency for snooped and non-snooped traffic: the maximum the controller says it tolerates.
  void set_ltr();
  // The first half of the UUID the controller's root router has (the rest is all ones).
  void uuid(uint32_t out[4]);

 private:
  ITbConfigSpace& config_;
};

}  // namespace hardware
