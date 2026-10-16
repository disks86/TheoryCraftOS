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

#include "hardware/Clock/TscClock.hpp"

namespace hardware {

bool TscClock::initialize() {
  if (!st_ || !st_->boot_services) return false;
  constexpr uint64_t kCalibrationUs = 50000;
  // Stall() never returns early, only late: an interruption (firmware timer work,
  // a hypervisor descheduling us) makes a run take longer and so count MORE
  // ticks. The run with the fewest ticks is therefore the least disturbed.
  uint64_t fewest = ~0ull;
  for (int run = 0; run < 3; ++run) {
    const uint64_t start = cpu_.timestamp_counter();
    st_->boot_services->stall(kCalibrationUs);
    const uint64_t ticks = cpu_.timestamp_counter() - start;
    if (ticks < fewest) fewest = ticks;
  }
  ticks_per_us_ = fewest / kCalibrationUs;
  if (ticks_per_us_ == 0) return false;
  origin_ = cpu_.timestamp_counter();
  return true;
}

uint64_t TscClock::now_us() const {
  if (!ticks_per_us_) return 0;
  return (cpu_.timestamp_counter() - origin_) / ticks_per_us_;
}

}  // namespace hardware
