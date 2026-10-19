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

#include "Hardware/Guest/PvPanic.hpp"
#include "Hardware/IoPort.hpp"

namespace hardware {

bool PvPanic::probe() {
  const uint8_t supported = IoPort(kPort).read8();
  // An unclaimed port reads as all ones; a real device reports a few event bits.
  present_ = (supported & kPanicked) && (supported & ~0x07u) == 0;
  events_ = present_ ? supported : 0;
  return present_;
}

bool PvPanic::notify_panic() {
  if (!present_) return false;
  IoPort(kPort).write8(kPanicked);
  return true;
}

}  // namespace hardware
