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
#include "Hardware/IHardware.hpp"

namespace hardware {

// System memory as seen by the OS. Implementations live beside this file.
class IRandomAccessMemory : public IHardware {
 public:
  virtual uint64_t total_bytes() const = 0;   // all installed memory
  virtual uint64_t usable_bytes() const = 0;  // memory the OS may allocate
  // Start of one contiguous, writable region of usable_bytes() bytes.
  virtual void* base() const = 0;

 protected:
  constexpr IRandomAccessMemory() = default;
  ~IRandomAccessMemory() = default;
};

}  // namespace hardware
