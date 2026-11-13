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

// Turns the frames of a touch device (the contacts touching at one moment: what a multitouch report says) into touch events: Down for a contact that
// was not there before, Motion for one that moved, Up for one that is gone, in that order; and remembers which contact came first, the one a mouse
// emulation follows. Not thread safe: its owner locks around it (a device's queue lock).
#pragma once
#include <stdint.h>
#include "Hardware/Touch/ITouch.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

class TouchTracker {
 public:
  static constexpr uint32_t kMaxContacts = 10, kQueue = 64;
  struct Contact {
    uint32_t id = 0;
    uint32_t x = 0, y = 0;
  };

  // The contacts touching now (more than kMaxContacts: the rest are ignored; an id twice: the first counts).
  void frame(ArrayView<const Contact> touching, uint32_t max_x, uint32_t max_y);
  // Every contact is cancelled (the device went away or was reset).
  void cancel_all();
  bool pop(TouchEvent* out);
  // The contact down longest (what a mouse emulation follows); false when none is down.
  bool primary(uint32_t* x, uint32_t* y) const;
  uint32_t active_count() const { return active_count_; }
  // Events that did not fit in the queue (the reader is too slow): dropped, the oldest kept.
  uint64_t dropped() const { return dropped_; }

 private:
  void push(TouchPhase phase, const Contact& c);

  Contact active_[kMaxContacts];      // in the order they came down
  uint32_t active_count_ = 0;
  uint32_t max_x_ = 0, max_y_ = 0;
  TouchEvent queue_[kQueue];
  uint32_t head_ = 0, count_ = 0;
  uint64_t dropped_ = 0;
};

}  // namespace hardware
