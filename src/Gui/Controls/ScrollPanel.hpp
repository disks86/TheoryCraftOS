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
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/ScrollBar.hpp"

namespace gui {

// A container that stacks its visible children one under the other at their preferred heights (the full width) and scrolls them when they do not all fit:
// a scroll bar shows at the right edge then, and the mouse wheel anywhere over the panel (also over children that take no wheel input themselves, such as
// labels and meters) scrolls. Children with a dock style are stacked all the same.
class ScrollPanel : public Panel {
 public:
  explicit ScrollPanel(allocators::IAllocator& allocator);

  // The height of all visible children together.
  int32_t content_height() const { return content_height_; }
  // How far the content is scrolled up, in pixels.
  int32_t scroll_offset() const { return bar_.value(); }
  void scroll_to(int32_t offset);
  ScrollBar& scroll_bar() { return bar_; }
  bool scroll_bar_visible() const { return bar_.visible(); }

  void layout() override;
  void on_mouse_wheel(const MouseArgs& args) override { bar_.on_mouse_wheel(args); }
  bool wheel_to_parent() const override { return false; }   // (it takes the wheel itself)

 private:
  static void on_scrolled(void* self, Control&);

  ScrollBar bar_;
  int32_t content_height_ = 0;
};

}  // namespace gui
