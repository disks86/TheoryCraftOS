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

#include "Hardware/Backlight/IntelPchBacklight.hpp"

namespace hardware {

bool IntelPchBacklight::initialize() {
  period_ = 0;
  if (!(registers_.read32(kControl1) & kEnable)) return false;
  period_ = registers_.read32(kControl2) >> 16;
  return period_ != 0;
}

bool IntelPchBacklight::percent(uint8_t* out) {
  if (period_ == 0) return false;
  const uint32_t duty = registers_.read32(kControl2) & 0xFFFF;
  const uint32_t clamped = duty > period_ ? period_ : duty;
  *out = static_cast<uint8_t>((uint64_t{clamped} * 100 + period_ / 2) / period_);
  return true;
}

bool IntelPchBacklight::set_percent(uint8_t percent) {
  if (period_ == 0) return false;
  if (percent > 100) percent = 100;
  const uint32_t duty = static_cast<uint32_t>((uint64_t{period_} * percent + 50) / 100);
  registers_.write32(kControl2, (period_ << 16) | duty);
  return true;
}

}  // namespace hardware
