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

// A tooltip: a small box with one or more lines of text (split at '\n') that floats above the other controls next to the pointer or a control.
// It is a child of the window (or a panel) added after the controls it floats over, so it is painted last, and it is hidden until show_at() or
// show_below() is called. It sizes itself to its text, is moved to stay inside its parent (flipped to the other side of the pointer or anchor when
// there is no room), and the pointer passes through it. Showing it after a hover delay is up to the caller: this OS has no timer in the GUI layer.
#pragma once
#include "Gui/Control.hpp"
#include "Utility/String.hpp"

namespace gui {

class Tooltip : public Control {
 public:
  explicit Tooltip(allocators::IAllocator& allocator) : Control(allocator), text_(allocator) { set_visible(false); }

  // The text, lines separated by '\n'. An empty text can not be shown. Returns false if memory ran out (the text is then empty).
  bool set_text(StringView text);
  StringView text() const { return text_.view(); }
  uint32_t line_count() const;
  // The size the box needs for its text.
  Size box_size() const;

  // Shows the tooltip for a pointer at (x, y) of the parent: below and to the right of it, or above and/or to the left where that does not fit.
  void show_at(int32_t x, int32_t y);
  // Shows it under `anchor` (a rectangle in the parent's coordinates), left aligned, or above it when there is no room below.
  void show_below(const Rect2D& anchor);
  void hide() { set_visible(false); }
  bool shown() const { return visible(); }

  void paint(Painter& painter) override;
  bool mouse_transparent() const override { return true; }

  static constexpr int32_t kPadX = 5, kPadY = 3, kPointerGap = 14;
  static constexpr uint32_t kBackground = 0xFFFFFFE1;

 private:
  // Moves the box to (x, y), then back inside the parent. No parent: stays where it is put.
  void place_inside(int32_t x, int32_t y);

  String text_;
};

}  // namespace gui
