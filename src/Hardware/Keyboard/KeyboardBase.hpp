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
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace hardware {

// Event queue and modifier tracking shared by keyboards. A subclass implements
// fetch(), which reads whatever the device has and calls push() for each event.
class KeyboardBase : public IKeyboard {
 public:
  bool poll(KeyEvent* out) override;
  uint8_t modifiers() const override { return modifiers_; }
  // The device is going away: queues a release for every key it still holds down, so nothing stays pressed in whoever reads the events.
  void release_all();

 protected:
  constexpr KeyboardBase() = default;
  ~KeyboardBase() = default;

  virtual void fetch() = 0;
  virtual void on_release_all() {}   // (with the lock held) push() the releases
  void push(uint8_t usage, bool pressed);

 private:
  static constexpr uint32_t kCapacity = 64;
  KeyEvent queue_[kCapacity] = {};
  uint32_t head_ = 0, count_ = 0;
  uint8_t modifiers_ = 0;
  kernel::Mutex mutex_;  // polling threads take turns
};

}  // namespace hardware
