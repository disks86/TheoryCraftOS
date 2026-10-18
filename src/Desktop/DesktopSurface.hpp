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

// The area behind everything on the desktop. With several monitors (set_regions()) the desktop colour is painted on each of them, and what lies
// between them - the corners that no monitor shows - stays black; the watermark goes in the corner of the first (primary) region.
#pragma once
#include "Gui/Control.hpp"
#include "ObjectContainer/List.hpp"

namespace desktop {

class DesktopSurface final : public gui::Control {
 public:
  explicit DesktopSurface(allocators::IAllocator& allocator) : gui::Control(allocator), regions_(allocator) {}
  // The rectangles of the monitors, the primary first; none: the whole surface is one.
  void set_regions(const gui::Rect2D* regions, uint32_t count) {
    gui::UiGuard lock(this);
    regions_.clear();
    for (uint32_t i = 0; i < count; ++i) regions_.push_back(regions[i]);
    invalidate();
  }
  void paint(gui::Painter& painter) override;

 private:
  List<gui::Rect2D> regions_;
};

}  // namespace desktop
