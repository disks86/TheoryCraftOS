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

#include "Hardware/Touch/CompositeTouch.hpp"

namespace hardware {

bool CompositeTouch::add(ITouch* touch) {
  kernel::ScopedMutex guard(mutex_);
  if (!touch) return false;
  for (ITouch*& d : devices_) {
    if (d) continue;
    d = touch;
    touch->set_pointer_emulation(emulation_);
    return true;
  }
  return false;
}

bool CompositeTouch::remove(ITouch* touch) {
  kernel::ScopedMutex guard(mutex_);
  for (ITouch*& d : devices_)
    if (d == touch && touch) {
      d = nullptr;
      return true;
    }
  return false;
}

uint32_t CompositeTouch::count() const {
  kernel::ScopedMutex guard(mutex_);
  uint32_t n = 0;
  for (ITouch* d : devices_) n += d != nullptr;
  return n;
}

bool CompositeTouch::poll(TouchEvent* out) {
  kernel::ScopedMutex guard(mutex_);
  for (uint32_t i = 0; i < kMaxDevices; ++i) {
    const uint32_t slot = (next_ + i) % kMaxDevices;
    ITouch* d = devices_[slot];
    TouchEvent e;
    if (!d || !d->poll(&e)) continue;
    next_ = (slot + 1) % kMaxDevices;
    e.id = (slot << 24) | (e.id & 0xFFFFFFu);
    if (out) *out = e;
    return true;
  }
  return false;
}

void CompositeTouch::set_pointer_emulation(bool on) {
  kernel::ScopedMutex guard(mutex_);
  emulation_ = on;
  for (ITouch* d : devices_)
    if (d) d->set_pointer_emulation(on);
}

}  // namespace hardware
