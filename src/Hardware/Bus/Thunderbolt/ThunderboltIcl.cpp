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

#include "Hardware/Bus/Thunderbolt/ThunderboltIcl.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

bool TbIceLake::force_power(bool on, uint32_t timeout_us) {
  uint32_t value = config_.read32(tb_icl::kForcePowerRegister);
  if (on) {
    value &= ~tb_icl::kDmaDelayMask;
    value |= tb_icl::kDmaDelay << tb_icl::kDmaDelayShift;
    value |= tb_icl::kForcePower;
  } else {
    value &= ~tb_icl::kForcePower;
  }
  config_.write32(tb_icl::kForcePowerRegister, value);
  if (!on) return true;
  for (uint32_t waited = 0;; waited += 3000) {
    if (config_.read32(tb_icl::kFwReadyRegister) & tb_icl::kFwReady) return true;
    if (waited >= timeout_us) return false;
    delay_us(3000);
  }
}

bool TbIceLake::lc_mailbox(uint32_t command, uint32_t timeout_us) {
  config_.write32(tb_icl::kLcCommandRegister, ((command << tb_icl::kLcCommandShift) & tb_icl::kLcCommandMask) | tb_icl::kLcValid);
  bool done = false;
  for (uint32_t waited = 0; !done; waited += 1000) {
    done = (config_.read32(tb_icl::kLcDoneRegister) & tb_icl::kLcDone) != 0;
    if (done || waited >= timeout_us) break;
    delay_us(1000);
  }
  config_.write32(tb_icl::kLcCommandRegister, 0);                    // the valid bit is cleared either way
  return done;
}

void TbIceLake::set_ltr() {
  const uint32_t max_ltr = config_.read32(tb_icl::kMaxLtrRegister) & 0xFFFF;
  config_.write32(tb_icl::kLtrRegister, (max_ltr << 16) | max_ltr);
}

void TbIceLake::uuid(uint32_t out[4]) {
  out[0] = config_.read32(tb_icl::kUuidLowRegister);
  out[1] = config_.read32(tb_icl::kUuidHighRegister);
  out[2] = out[3] = 0xFFFFFFFFu;
}

}  // namespace hardware
