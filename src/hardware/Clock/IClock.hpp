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
#include "hardware/IHardware.hpp"

namespace hardware {

// A monotonic clock: time since some fixed moment (boot), never going backwards.
// It tells how long things took, not what the date is.
class IClock : public IHardware {
 public:
  virtual uint64_t now_us() const = 0;
  uint64_t now_ms() const { return now_us() / 1000; }

 protected:
  constexpr IClock() = default;
  ~IClock() = default;
};

}  // namespace hardware
