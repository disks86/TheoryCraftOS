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

// The pen as a pointer: an absolute mouse made from the events of a CompositePen, added to the system's mice so that every program that knows a mouse works with the pen. Hovering
// moves the pointer, the tip touching is the left button, the barrel button the right one, the eraser end touching the middle one. When the pen leaves the range the buttons are
// let go. Only changes are reported (a pen sends a report for every scan, mostly the same).
#pragma once
#include "Hardware/Mouse/IMouse.hpp"
#include "Hardware/Pen/CompositePen.hpp"

namespace hardware {

class PenMouse final : public IMouse {
 public:
  explicit PenMouse(CompositePen& pens) : pens_(pens) {}
  const char* name() const override { return "pen"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  bool poll(MouseEvent* out) override;
  bool is_absolute() const override { return true; }
  uint32_t max_x() const override { return max_x_; }
  uint32_t max_y() const override { return max_y_; }

 private:
  CompositePen& pens_;
  uint32_t max_x_ = 0, max_y_ = 0, x_ = 0, y_ = 0;
  uint8_t buttons_ = 0;
  bool in_range_ = false;
};

}  // namespace hardware
