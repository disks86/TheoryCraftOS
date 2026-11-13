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

#include "Hardware/Touch/LinuxMultitouch.hpp"

namespace hardware {

bool LinuxMultitouch::feed(uint16_t type, uint16_t code, uint32_t value, TouchTracker& tracker, uint32_t max_x, uint32_t max_y) {
  if (type == kEvAbs) {
    if (code == kAbsMtSlot) {
      slot_ = value < TouchTracker::kMaxContacts ? value : TouchTracker::kMaxContacts;      // (a slot past ours: its events are ignored)
    } else if (code == kAbsMtTrackingId || code == kAbsMtPositionX || code == kAbsMtPositionY) {
      multitouch_ = true;
      if (slot_ < TouchTracker::kMaxContacts) {
        Slot& s = slots_[slot_];
        if (code == kAbsMtTrackingId) s.tracking = static_cast<int32_t>(value) < 0 ? -1 : static_cast<int32_t>(value);
        else if (code == kAbsMtPositionX) s.x = value;
        else s.y = value;
      }
    } else if (code == kAbsX) {
      single_x_ = value;
    } else if (code == kAbsY) {
      single_y_ = value;
    }
    return false;
  }
  if (type == kEvKey) {
    if (code == kBtnTouch) touch_ = value != 0;
    return false;
  }
  if (type != kEvSyn || code != kSynReport) return false;
  TouchTracker::Contact touching[TouchTracker::kMaxContacts];
  uint32_t n = 0;
  if (multitouch_) {
    for (const Slot& s : slots_)
      if (s.tracking >= 0) touching[n++] = {static_cast<uint32_t>(s.tracking), s.x, s.y};
  } else if (touch_) {
    touching[n++] = {0, single_x_, single_y_};
  }
  tracker.frame(ArrayView<const TouchTracker::Contact>(touching, n), max_x, max_y);
  return true;
}

}  // namespace hardware
