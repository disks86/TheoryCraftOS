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

// A tab control: a row of tabs over a stack of pages, of which only the selected one is shown. Pages are ordinary
// controls (usually Panels) that the caller owns, like every control; add_page() puts one in the tab control, sets it
// to fill the page area and hides it unless it is selected. Click a tab to select its page, Left/Right move between
// tabs while the tab control has the focus, and Ctrl+Tab / Ctrl+Shift+Tab cycle through the pages from anywhere
// inside one. Tabs that do not fit the width are cut off.
#pragma once
#include "Gui/Control.hpp"
#include "Utility/String.hpp"

namespace gui {

class TabControl : public Control {
 public:
  static constexpr int32_t kTabHeight = 24, kTabPad = 10, kBorder = 2;

  explicit TabControl(allocators::IAllocator& allocator) : Control(allocator), titles_(allocator), pages_(allocator) {}

  // Adds a page after the others; the first one added is selected. Returns its index or -1.
  int32_t add_page(StringView title, Control* page);
  uint32_t page_count() const { return static_cast<uint32_t>(pages_.count()); }
  StringView page_title(uint32_t index) const { return index < titles_.count() ? titles_[index].view() : StringView(); }
  Control* page(uint32_t index) const { return index < pages_.count() ? pages_[index] : nullptr; }

  int32_t selected_page() const { return selected_; }  // -1 while there are no pages
  // Shows another page (out of range does nothing). The handler fires when the selection changes.
  void select_page(int32_t index);
  void on_selection_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }

  // Geometry (for painting and tests), in this control's coordinates.
  Rect2D tab_rect(uint32_t index) const;
  Rect2D page_rect() const;
  int32_t tab_at(int32_t x, int32_t y) const;

  bool focusable() const override { return true; }
  Rect2D client_rect() const override { return page_rect(); }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {320, 200}; }

 private:
  void cycle(int32_t direction);

  List<String> titles_;
  List<Control*> pages_;
  int32_t selected_ = -1;
  Handler changed_;
};

}  // namespace gui
