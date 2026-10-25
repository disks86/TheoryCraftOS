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

// A pager: a row of buttons for moving through numbered pages - "<" (previous), the page numbers, and ">" (next). With more than seven pages the row
// keeps seven entries and replaces the stretches in between by "..." (the first and last page are always there, and the pages around the current one).
// The current page is drawn pressed. A click on a button goes to that page (an "..." does nothing); Left/Right move by one page, Home/End go to the ends.
// Pages are counted from 0 in the API and shown from 1. The control does not hold any content: it tells the handler which page was picked.
#pragma once
#include "Gui/Control.hpp"

namespace gui {

class Pagination : public Control {
 public:
  explicit Pagination(allocators::IAllocator& allocator) : Control(allocator) {}

  int32_t page_count() const { return page_count_; }
  // At least 1 page. The current page is kept inside the new count.
  void set_page_count(int32_t count);
  int32_t current_page() const { return current_; }
  // Clamped to 0 .. page_count()-1. The handler fires only when the page really changes.
  void set_current_page(int32_t page);
  void on_page_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  // What a button stands for: a page (>= 0), or one of these.
  static constexpr int32_t kGap = -1, kPrevious = -2, kNext = -3, kNone = -4;
  static constexpr int32_t kMaxEntries = 7, kButtonHeight = 24, kButtonWidth = 28, kSpacing = 2, kMargin = 2;

  // The page entries of the row in order (kGap for an "..."); returns how many were written (at most kMaxEntries).
  int32_t entries(ArrayView<int32_t> out) const;
  // The button at a point (kPrevious, kNext, a page, kGap) or kNone; and the rectangle of a button in this control's coordinates.
  int32_t button_at(int32_t x, int32_t y) const;
  Rect2D previous_rect() const;
  Rect2D next_rect() const;
  Rect2D entry_rect(int32_t position) const;   // the `position`-th of entries()
  // The width the whole row needs.
  int32_t row_width() const;

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {260, kButtonHeight + 2 * kMargin}; }

 private:
  int32_t entry_width(int32_t entry) const;
  int32_t entry_x(int32_t position) const;

  int32_t page_count_ = 1;
  int32_t current_ = 0;
  Handler changed_;
};

}  // namespace gui
