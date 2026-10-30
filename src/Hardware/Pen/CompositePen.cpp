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

#include "Hardware/Pen/CompositePen.hpp"

namespace hardware {

bool CompositePen::add(IPen* pen) {
  kernel::ScopedMutex guard(mutex_);
  if (!pen || count_ == kMaxPens) return false;
  pens_[count_++] = pen;
  return true;
}

bool CompositePen::remove(IPen* pen) {
  kernel::ScopedMutex guard(mutex_);
  for (uint32_t i = 0; i < count_; ++i) {
    if (pens_[i] != pen) continue;
    for (uint32_t k = i + 1; k < count_; ++k) pens_[k - 1] = pens_[k];
    pens_[--count_] = nullptr;
    next_ = count_ ? next_ % count_ : 0;
    return true;
  }
  return false;
}

uint32_t CompositePen::count() const {
  kernel::ScopedMutex guard(mutex_);
  return count_;
}

IPen* CompositePen::at(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < count_ ? pens_[index] : nullptr;
}

bool CompositePen::poll(PenEvent* out) {
  kernel::ScopedMutex guard(mutex_);
  for (uint32_t i = 0; i < count_; ++i) {
    IPen* p = pens_[(next_ + i) % count_];
    PenEvent e;
    if (p->poll(&e)) {
      next_ = (next_ + i + 1) % count_;
      last_ = e;
      if (out) *out = e;
      return true;
    }
  }
  return false;
}

PenEvent CompositePen::last() const {
  kernel::ScopedMutex guard(mutex_);
  return last_;
}

}  // namespace hardware
