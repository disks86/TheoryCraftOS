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

// The Linux input layer's multitouch protocol (type B), as virtio-input and evdev-style devices send it: ABS_MT_SLOT picks a contact, ABS_MT_TRACKING_ID
// starts one (or, -1, ends it), ABS_MT_POSITION_X / Y move it, SYN_REPORT closes a frame, which goes to a TouchTracker. A device that sends only ABS_X /
// ABS_Y and BTN_TOUCH is one contact. Slots past TouchTracker::kMaxContacts are ignored. Not thread safe (its owner locks).
#pragma once
#include <stdint.h>
#include "Hardware/Touch/TouchTracker.hpp"

namespace hardware {

class LinuxMultitouch {
 public:
  static constexpr uint16_t kEvSyn = 0, kEvKey = 1, kEvAbs = 3;
  static constexpr uint16_t kAbsX = 0, kAbsY = 1, kAbsMtSlot = 0x2F, kAbsMtPositionX = 0x35, kAbsMtPositionY = 0x36, kAbsMtTrackingId = 0x39;
  static constexpr uint16_t kBtnTouch = 0x14A, kSynReport = 0;

  // One event; true when it closed a frame (the tracker has its events).
  bool feed(uint16_t type, uint16_t code, uint32_t value, TouchTracker& tracker, uint32_t max_x, uint32_t max_y);

 private:
  struct Slot {
    int32_t tracking = -1;      // -1: no contact
    uint32_t x = 0, y = 0;
  };
  Slot slots_[TouchTracker::kMaxContacts];
  uint32_t slot_ = 0;
  bool multitouch_ = false;     // the device sent ABS_MT_*: BTN_TOUCH and ABS_X / Y are not used
  bool touch_ = false;          // BTN_TOUCH (single-touch devices)
  uint32_t single_x_ = 0, single_y_ = 0;
};

}  // namespace hardware
