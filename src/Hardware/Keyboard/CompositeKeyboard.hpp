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
#include "Kernel/Sync.hpp"
#include "Hardware/Keyboard/IKeyboard.hpp"

namespace hardware {

// Presents several keyboards as one: poll() returns events from whichever has
// them, and modifiers() is the union. Children are not owned. Keyboards may be
// added and removed while other threads poll (USB hot-plug); a removed
// keyboard's remaining events are still delivered, and nothing calls into it
// once remove() has returned.
class CompositeKeyboard final : public IKeyboard {
 public:
  static constexpr uint32_t kMaxKeyboards = 8;

  constexpr CompositeKeyboard() = default;

  const char* name() const override { return "keyboards"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  bool add(IKeyboard* keyboard);
  // Takes a keyboard out. Whatever events it still has (a release for every key it held, if the caller asked it to) are kept and delivered
  // by poll() first. False if it was not there.
  bool remove(IKeyboard* keyboard);
  uint32_t count() const;
  // (A pointer for use on the thread that adds and removes keyboards, or while none can be removed.)
  IKeyboard* at(uint32_t index) const;

  bool poll(KeyEvent* out) override;
  uint8_t modifiers() const override;

 private:
  static constexpr uint32_t kOrphans = 64;

  mutable kernel::Mutex mutex_;  // polling threads take turns, and the list changes under it
  KeyEvent orphans_[kOrphans] = {};   // events of keyboards that were removed
  uint32_t orphan_head_ = 0, orphan_count_ = 0;
  IKeyboard* keyboards_[kMaxKeyboards] = {};
  uint32_t count_ = 0;
  uint32_t next_ = 0;  // round-robin start, so one chatty device cannot starve the rest
};

}  // namespace hardware
