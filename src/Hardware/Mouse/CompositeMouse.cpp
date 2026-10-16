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

#include "Hardware/Mouse/CompositeMouse.hpp"

namespace hardware {

bool CompositeMouse::add(IMouse* mouse) {
  if (!mouse || count_ == kMaxMice) return false;
  mice_[count_++] = mouse;
  return true;
}

bool CompositeMouse::poll(MouseEvent* out) {
  for (uint32_t i = 0; i < count_; ++i) {
    IMouse* m = mice_[(next_ + i) % count_];
    if (m->poll(out)) {
      next_ = (next_ + i + 1) % count_;
      return true;
    }
  }
  return false;
}

}  // namespace hardware
