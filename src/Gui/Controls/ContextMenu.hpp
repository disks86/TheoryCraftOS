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

// A context menu: the menu that pops up at the pointer when the right mouse button is pressed on a control (or the Menu key /
// Shift+F10 is pressed while it has the focus). Give a control one with Control::set_context_menu(); the window then opens it instead
// of delivering that right-button press. A control can decline for some places (Control::accepts_context_menu) and prepare for the
// click first (Control::prepare_context_menu: a list selects the item under the pointer). The menu is the same kind as the Start
// menu and the menu bar: items with an optional shortcut text, check marks, separators and disabled entries; click one (or drag onto it
// and release), use the arrow keys and Enter, or click elsewhere / press Escape to close. on_opening() runs just before the menu
// opens - the place to enable, disable and check its items for the state of the control it belongs to.
//
// A context menu is not in the control tree: build it next to the control it belongs to (it must live as long as that control).
#pragma once
#include "Gui/Controls/Menu.hpp"

namespace gui {

class ContextMenu : public Control, public MenuHost {
 public:
  explicit ContextMenu(allocators::IAllocator& allocator) : Control(allocator), menu_(allocator, this), popup_(allocator, *this) {}

  // The items; the returned item stays valid for the life of the menu (set `enabled` / `checked` on it any time).
  MenuItem* add_item(StringView text, Handler on_click = {}, StringView shortcut = {}) { return menu_.add_item(text, on_click, shortcut); }
  MenuItem* add_separator() { return menu_.add_separator(); }
  Menu& menu() { return menu_; }
  MenuItem* find_item(StringView text) const;
  void set_min_width(int32_t width) { UiGuard lock(this); popup_.set_min_width(width); }
  // Runs (in the thread that opens the menu, before it shows) every time the menu is about to open - also when it has no items yet, so a
  // control can fill its menu the first time it is wanted.
  void on_opening(Handler handler) { UiGuard lock(this); opening_ = handler; }

  // Opens the menu in `window` with its top left at (x, y) of the window (moved to stay inside it). False if it has no items (after on_opening) or is open.
  // From the keyboard the first item starts highlighted.
  bool show(Window& window, int32_t x, int32_t y, bool from_keyboard = false);
  bool open() const { return open_; }
  void close_menu() override;

  void switch_menu(int32_t) override {}
  void activate(MenuItem* item) override;
  void on_popup_closed(Control*) override { open_ = false; }

 protected:
  Size default_size() const override { return {0, 0}; }

 private:
  Menu menu_;
  MenuPopup popup_;
  Handler opening_;
  Window* window_ = nullptr;
  bool open_ = false;
};

}  // namespace gui
