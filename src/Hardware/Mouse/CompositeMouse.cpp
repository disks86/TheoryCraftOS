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
  kernel::ScopedMutex guard(mutex_);
  if (!mouse || count_ == kMaxMice) return false;
  mice_[count_++] = mouse;
  return true;
}

bool CompositeMouse::remove(IMouse* mouse) {
  kernel::ScopedMutex guard(mutex_);
  for (uint32_t i = 0; i < count_; ++i) {
    if (mice_[i] != mouse) continue;
    MouseEvent event;
    for (uint32_t n = 0; n < 4 * kOrphans && mouse->poll(&event); ++n)
      if (orphan_count_ < kOrphans) {
        orphans_[(orphan_head_ + orphan_count_) % kOrphans] = event;
        ++orphan_count_;
      }
    for (uint32_t k = i + 1; k < count_; ++k) mice_[k - 1] = mice_[k];
    mice_[--count_] = nullptr;
    if (count_ > 0) next_ %= count_;
    else next_ = 0;
    return true;
  }
  return false;
}

uint32_t CompositeMouse::count() const {
  kernel::ScopedMutex guard(mutex_);
  return count_;
}

IMouse* CompositeMouse::at(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < count_ ? mice_[index] : nullptr;
}

bool CompositeMouse::poll(MouseEvent* out) {
  kernel::ScopedMutex guard(mutex_);
  if (orphan_count_ > 0 && out) {
    *out = orphans_[orphan_head_];
    orphan_head_ = (orphan_head_ + 1) % kOrphans;
    --orphan_count_;
    return true;
  }
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
