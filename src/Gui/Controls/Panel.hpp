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

// Containers: Panel (a plain area with an optional look) and UniformPanel (splits its area evenly).
#pragma once
#include "Gui/Control.hpp"

namespace gui {

enum class PanelStyle : uint8_t { Flat, Raised, Sunken, Transparent };

// A container. Its children are laid out by dock style (see Control::layout()).
class Panel : public Control {
 public:
  explicit Panel(allocators::IAllocator& allocator) : Control(allocator) {}
  void set_style(PanelStyle style) { style_ = style; invalidate(); }
  void paint(Painter& painter) override;

 private:
  PanelStyle style_ = PanelStyle::Flat;
};

// Divides its client area evenly between its visible children (side by side for Horizontal, stacked
// for Vertical), ignoring their dock style and sizes: with n children each gets 1/n of the length,
// the leftover pixels going one each to the first children, so the pieces always add up to the
// whole. `spacing` pixels are kept between neighbours.
class UniformPanel : public Panel {
 public:
  explicit UniformPanel(allocators::IAllocator& allocator) : Panel(allocator) {}
  void set_orientation(Orientation o) { orientation_ = o; invalidate(); }
  void set_spacing(int32_t pixels) { spacing_ = pixels < 0 ? 0 : pixels; invalidate(); }
  void layout() override;

 private:
  Orientation orientation_ = Orientation::Horizontal;
  int32_t spacing_ = 0;
};

}  // namespace gui
