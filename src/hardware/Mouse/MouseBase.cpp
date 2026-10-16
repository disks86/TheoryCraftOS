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

#include "hardware/Mouse/MouseBase.hpp"

namespace hardware {

void MouseBase::push(const MouseEvent& event) {
  if (count_ == kCapacity) return;  // drop on overflow
  MouseEvent e = event;
  if (e.absolute) {  // let consumers of a merged stream scale positions
    e.max_x = max_x();
    e.max_y = max_y();
  }
  queue_[(head_ + count_) % kCapacity] = e;
  ++count_;
}

bool MouseBase::poll(MouseEvent* out) {
  if (count_ == 0) fetch();
  if (count_ == 0 || !out) return false;
  *out = queue_[head_];
  head_ = (head_ + 1) % kCapacity;
  --count_;
  return true;
}

}  // namespace hardware
