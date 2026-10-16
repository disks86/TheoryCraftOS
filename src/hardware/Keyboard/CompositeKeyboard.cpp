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

#include "hardware/Keyboard/CompositeKeyboard.hpp"

namespace hardware {

bool CompositeKeyboard::add(IKeyboard* keyboard) {
  if (!keyboard || count_ == kMaxKeyboards) return false;
  keyboards_[count_++] = keyboard;
  return true;
}

bool CompositeKeyboard::poll(KeyEvent* out) {
  for (uint32_t i = 0; i < count_; ++i) {
    IKeyboard* k = keyboards_[(next_ + i) % count_];
    if (k->poll(out)) {
      next_ = (next_ + i + 1) % count_;
      return true;
    }
  }
  return false;
}

uint8_t CompositeKeyboard::modifiers() const {
  uint8_t all = 0;
  for (uint32_t i = 0; i < count_; ++i) all |= keyboards_[i]->modifiers();
  return all;
}

}  // namespace hardware
