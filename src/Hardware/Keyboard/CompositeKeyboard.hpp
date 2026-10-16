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

#pragma once
#include "Hardware/Keyboard/IKeyboard.hpp"

namespace hardware {

// Presents several keyboards as one: poll() returns events from whichever has
// them, and modifiers() is the union. Children are not owned.
class CompositeKeyboard final : public IKeyboard {
 public:
  static constexpr uint32_t kMaxKeyboards = 8;

  constexpr CompositeKeyboard() = default;

  const char* name() const override { return "keyboards"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  bool add(IKeyboard* keyboard);
  uint32_t count() const { return count_; }
  IKeyboard* at(uint32_t index) const { return index < count_ ? keyboards_[index] : nullptr; }

  bool poll(KeyEvent* out) override;
  uint8_t modifiers() const override;

 private:
  IKeyboard* keyboards_[kMaxKeyboards] = {};
  uint32_t count_ = 0;
  uint32_t next_ = 0;  // round-robin start, so one chatty device cannot starve the rest
};

}  // namespace hardware
