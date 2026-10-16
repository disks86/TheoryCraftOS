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
#include "hardware/Mouse/IMouse.hpp"

namespace hardware {

// Presents several mice/tablets as one stream of events. Children are not
// owned. Absolute and relative events can interleave; check MouseEvent::absolute
// (and use the producing device's max_x()/max_y() through at()).
class CompositeMouse final : public IMouse {
 public:
  static constexpr uint32_t kMaxMice = 8;

  constexpr CompositeMouse() = default;

  const char* name() const override { return "mice"; }
  bool initialize() override { return true; }
  void shutdown() override {}

  bool add(IMouse* mouse);
  uint32_t count() const { return count_; }
  IMouse* at(uint32_t index) const { return index < count_ ? mice_[index] : nullptr; }

  bool poll(MouseEvent* out) override;
  bool is_absolute() const override { return false; }
  uint32_t max_x() const override { return 0; }
  uint32_t max_y() const override { return 0; }

 private:
  IMouse* mice_[kMaxMice] = {};
  uint32_t count_ = 0;
  uint32_t next_ = 0;
};

}  // namespace hardware
