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

#include "Hardware/Keyboard/CompositeKeyboard.hpp"

namespace hardware {

bool CompositeKeyboard::add(IKeyboard* keyboard) {
  kernel::ScopedMutex guard(mutex_);
  if (!keyboard || count_ == kMaxKeyboards) return false;
  keyboards_[count_++] = keyboard;
  return true;
}

bool CompositeKeyboard::remove(IKeyboard* keyboard) {
  kernel::ScopedMutex guard(mutex_);
  for (uint32_t i = 0; i < count_; ++i) {
    if (keyboards_[i] != keyboard) continue;
    KeyEvent event;
    for (uint32_t n = 0; n < 4 * kOrphans && keyboard->poll(&event); ++n)   // (the lock keeps other threads out of it meanwhile)
      if (orphan_count_ < kOrphans) {
        orphans_[(orphan_head_ + orphan_count_) % kOrphans] = event;
        ++orphan_count_;
      }
    for (uint32_t k = i + 1; k < count_; ++k) keyboards_[k - 1] = keyboards_[k];
    keyboards_[--count_] = nullptr;
    if (count_ > 0) next_ %= count_;
    else next_ = 0;
    return true;
  }
  return false;
}

uint32_t CompositeKeyboard::count() const {
  kernel::ScopedMutex guard(mutex_);
  return count_;
}

IKeyboard* CompositeKeyboard::at(uint32_t index) const {
  kernel::ScopedMutex guard(mutex_);
  return index < count_ ? keyboards_[index] : nullptr;
}

bool CompositeKeyboard::poll(KeyEvent* out) {
  kernel::ScopedMutex guard(mutex_);
  if (orphan_count_ > 0 && out) {
    *out = orphans_[orphan_head_];
    orphan_head_ = (orphan_head_ + 1) % kOrphans;
    --orphan_count_;
    return true;
  }
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
  kernel::ScopedMutex guard(mutex_);
  uint8_t all = 0;
  for (uint32_t i = 0; i < count_; ++i) all |= keyboards_[i]->modifiers();
  return all;
}

}  // namespace hardware
