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

#include "Gui/Controls/ContextMenu.hpp"
#include "Gui/Window.hpp"

namespace gui {

MenuItem* ContextMenu::find_item(StringView text) const {
  for (uint32_t i = 0; i < menu_.item_count(); ++i) {
    MenuItem* item = menu_.item(i);
    if (!item->separator && item->text.view().equals(text)) return item;
  }
  return nullptr;
}

bool ContextMenu::show(Window& window, int32_t x, int32_t y, bool from_keyboard) {
  UiGuard lock(&window);
  if (open_) return false;
  if (opening_) opening_(*this);   // (it may be what fills the menu the first time)
  if (menu_.item_count() == 0) return false;
  popup_.show(&menu_);
  if (!window.open_popup(&popup_, this, x, y)) return false;
  window_ = &window;
  open_ = true;
  if (from_keyboard) popup_.highlight_first();
  return true;
}

void ContextMenu::close_menu() {
  if (open_ && window_) window_->close_popup();
  open_ = false;
}

void ContextMenu::activate(MenuItem* item) {
  const Handler handler = item->on_click;
  Window* window = window_;
  close_menu();
  if (window) window->dispatch(handler, *this);   // run the way every handler runs: on the window's handler thread if it has one
}

}  // namespace gui
