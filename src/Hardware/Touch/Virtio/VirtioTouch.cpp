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

#include "Hardware/Touch/Virtio/VirtioTouch.hpp"

namespace hardware {

bool VirtioTouch::initialize() {
  if (!device_.initialize()) return false;
  if (device_.kind() != VirtioInputKind::Touch) {
    device_.shutdown();
    return false;
  }
  return true;
}

void VirtioTouch::set_pointer_emulation(bool on) {
  kernel::ScopedMutex guard(mutex_);
  emulation_ = on;
  mouse_count_ = 0;      // (what was queued for the mouse side is not wanted any more, or is stale)
}

void VirtioTouch::read() {
  VirtioInputEvent e;
  while (device_.next_event(&e))
    if (protocol_.feed(e.type, e.code, e.value, tracker_, device_.absolute_max(0), device_.absolute_max(1))) frame();
}

bool VirtioTouch::poll(TouchEvent* out) {
  kernel::ScopedMutex guard(mutex_);
  read();
  return tracker_.pop(out);
}

bool VirtioTouch::poll_mouse(MouseEvent* out) {
  kernel::ScopedMutex guard(mutex_);
  read();
  if (mouse_count_ == 0) return false;
  if (out) *out = mouse_[mouse_head_];
  mouse_head_ = (mouse_head_ + 1) % kMouseQueue;
  --mouse_count_;
  return true;
}

void VirtioTouch::emit(const MouseEvent& e) {
  if (mouse_count_ == kMouseQueue) return;      // (dropped: nobody reads the mouse side)
  mouse_[(mouse_head_ + mouse_count_) % kMouseQueue] = e;
  ++mouse_count_;
}

void VirtioTouch::frame() {
  const uint32_t max_x = device_.absolute_max(0), max_y = device_.absolute_max(1);
  // the mouse side: the first finger is the left button where it is
  if (!emulation_) {
    emulated_down_ = false;
    return;
  }
  MouseEvent m{};
  m.absolute = true;
  m.max_x = max_x;
  m.max_y = max_y;
  uint32_t x = 0, y = 0;
  if (tracker_.primary(&x, &y)) {
    m.x = emulated_x_ = x;
    m.y = emulated_y_ = y;
    m.buttons = mouse_button::kLeft;
    emit(m);
    emulated_down_ = true;
  } else if (emulated_down_) {
    m.x = emulated_x_;      // (let go where it was last)
    m.y = emulated_y_;
    emit(m);
    emulated_down_ = false;
  }
}

}  // namespace hardware
