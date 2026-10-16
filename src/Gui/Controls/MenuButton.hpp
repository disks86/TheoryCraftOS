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

// A button that opens a menu (a Start button): the menu pops up above the button or below it.
#pragma once
#include "Gui/Controls/Menu.hpp"

namespace gui {

class MenuButton : public Control, public MenuHost {
 public:
  enum class Direction : uint8_t { Up, Down };

  explicit MenuButton(allocators::IAllocator& allocator)
      : Control(allocator), text_(allocator), menu_(allocator), popup_(allocator, *this) {}

  void set_text(StringView text) { text_.assign(text); invalidate(); }
  Menu& menu() { return menu_; }
  void set_direction(Direction d) { direction_ = d; }
  void set_menu_min_width(int32_t width) { popup_.set_min_width(width); }
  bool open() const { return open_; }

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_popup_closed(Control*) override { open_ = false; invalidate(); }
  bool on_key(const KeyArgs& args) override;

  void switch_menu(int32_t) override {}
  void activate(MenuItem* item) override;
  void close_menu() override;

 protected:
  Size default_size() const override { return {Painter::text_width(text_.view()) + 24, 24}; }

 private:
  void open_menu();

  String text_;
  Menu menu_;
  MenuPopup popup_;
  Direction direction_ = Direction::Down;
  bool open_ = false;
};

}  // namespace gui
