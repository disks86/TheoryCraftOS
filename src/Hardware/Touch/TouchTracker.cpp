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

#include "Hardware/Touch/TouchTracker.hpp"

namespace hardware {

void TouchTracker::push(TouchPhase phase, const Contact& c) {
  if (count_ == kQueue) {
    ++dropped_;
    return;
  }
  TouchEvent& e = queue_[(head_ + count_) % kQueue];
  e.phase = phase;
  e.id = c.id;
  e.x = c.x;
  e.y = c.y;
  e.max_x = max_x_;
  e.max_y = max_y_;
  ++count_;
}

void TouchTracker::frame(ArrayView<const Contact> touching, uint32_t max_x, uint32_t max_y) {
  max_x_ = max_x;
  max_y_ = max_y;
  // the ones gone: Up (with their last position), keeping the order of the others
  uint32_t kept = 0;
  for (uint32_t i = 0; i < active_count_; ++i) {
    bool still = false;
    for (const Contact& t : touching) still |= t.id == active_[i].id;
    if (still) active_[kept++] = active_[i];
    else push(TouchPhase::Up, active_[i]);
  }
  active_count_ = kept;
  // moved, and new
  uint32_t seen = 0;
  for (const Contact& t : touching) {
    if (seen++ == kMaxContacts) break;
    bool duplicate = false;
    for (uint32_t j = 0; j + 1 < seen && !duplicate; ++j) duplicate = touching[j].id == t.id;
    if (duplicate) continue;
    bool found = false;
    for (uint32_t i = 0; i < active_count_ && !found; ++i) {
      if (active_[i].id != t.id) continue;
      found = true;
      if (active_[i].x != t.x || active_[i].y != t.y) {
        active_[i] = t;
        push(TouchPhase::Motion, t);
      }
    }
    if (!found && active_count_ < kMaxContacts) {
      active_[active_count_++] = t;
      push(TouchPhase::Down, t);
    }
  }
}

void TouchTracker::cancel_all() {
  for (uint32_t i = 0; i < active_count_; ++i) push(TouchPhase::Cancel, active_[i]);
  active_count_ = 0;
}

bool TouchTracker::pop(TouchEvent* out) {
  if (count_ == 0) return false;
  if (out) *out = queue_[head_];
  head_ = (head_ + 1) % kQueue;
  --count_;
  return true;
}

bool TouchTracker::primary(uint32_t* x, uint32_t* y) const {
  if (active_count_ == 0) return false;
  *x = active_[0].x;
  *y = active_[0].y;
  return true;
}

}  // namespace hardware
