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
#include "Hardware/CentralProcessingUnit/ICentralProcessingUnit.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "uefi.hpp"

namespace hardware {

// A clock built on the CPU's timestamp counter. The counter runs at an unknown
// rate, so initialize() measures it once against the firmware's Stall(), which
// is timed by the platform's own timer: about 50 ms of waiting at boot. After
// that, reading the clock is a single cheap instruction. (Assumes an invariant
// counter - constant rate regardless of power state - which every CPU of the
// last fifteen years has, and every hypervisor presents.)
class TscClock final : public IClock {
 public:
  TscClock(const ICentralProcessingUnit& cpu, efi::SystemTable* st) : cpu_(cpu), st_(st) {}

  const char* name() const override { return "TSC clock"; }
  bool initialize() override;
  void shutdown() override {}

  uint64_t now_us() const override;
  // Counter ticks per microsecond measured by initialize() (0 before it).
  uint64_t ticks_per_microsecond() const { return ticks_per_us_; }

 private:
  const ICentralProcessingUnit& cpu_;
  efi::SystemTable* st_;
  uint64_t ticks_per_us_ = 0;
  uint64_t origin_ = 0;  // counter value when the clock was started
};

}  // namespace hardware
