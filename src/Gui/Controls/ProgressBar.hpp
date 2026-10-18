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

// A horizontal bar that fills from the left as a value grows from 0 to its maximum, with the percentage written over it.
#pragma once
#include "Gui/Control.hpp"

namespace gui {

class ProgressBar : public Control {
 public:
  explicit ProgressBar(allocators::IAllocator& allocator) : Control(allocator) {}
  // The bar is full at `maximum`; a maximum of 0 shows an empty bar. Safe from any thread.
  void set_maximum(uint64_t maximum) { UiGuard lock(this); maximum_ = maximum; clamp(); invalidate(); }
  void set_value(uint64_t value) {
    UiGuard lock(this);
    value_ = value;
    clamp();
    invalidate();
  }
  uint64_t value() const { return value_; }
  uint64_t maximum() const { return maximum_; }
  // 0..100
  uint32_t percent() const { return maximum_ ? static_cast<uint32_t>(value_ * 100 / maximum_) : 0; }
  void paint(Painter& painter) override;

 protected:
  Size default_size() const override { return {120, 20}; }

 private:
  void clamp() { if (value_ > maximum_) value_ = maximum_; }
  uint64_t value_ = 0, maximum_ = 0;
};

}  // namespace gui
