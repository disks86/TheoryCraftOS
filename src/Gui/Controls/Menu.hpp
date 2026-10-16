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

// Old-school menus: a MenuBar of titles (File, Edit, Help...) each opening a drop-down Menu
// of items (with an optional shortcut text on the right, check marks, separators and disabled
// entries). Mouse: click a title to open it, move across the titles to switch, click an item (or
// drag onto it and release) to run it, click elsewhere to close. Keys while a menu is open:
// Up/Down choose, Left/Right switch menus, Enter/Space run, Escape closes.
// Submenus are not supported yet.
#pragma once
#include "Gui/Control.hpp"
#include "Utility/String.hpp"

namespace gui {

// What a MenuPopup reports back to whatever opened it (a MenuBar, a MenuButton).
class MenuBar;
struct MenuItem;

class MenuHost {
 public:
  // Opens the neighbouring menu if the host has several (Left/Right keys); may do nothing.
  virtual void switch_menu(int32_t direction) = 0;
  // Runs the item and closes the menu.
  virtual void activate(MenuItem* item) = 0;
  virtual void close_menu() = 0;

 protected:
  constexpr MenuHost() = default;
  ~MenuHost() = default;
};

struct MenuItem {
  explicit MenuItem(allocators::IAllocator& allocator) : text(allocator), shortcut(allocator) {}
  String text;
  String shortcut;  // display only, e.g. "Ctrl+O"
  Handler on_click;
  bool enabled = true;
  bool checked = false;
  bool separator = false;
};

class Menu {
 public:
  Menu(allocators::IAllocator& allocator) : allocator_(allocator), title_(allocator), items_(allocator) {}
  ~Menu();
  Menu(const Menu&) = delete;
  Menu& operator=(const Menu&) = delete;

  StringView title() const { return title_.view(); }
  // The returned item stays valid for the life of the menu: set `enabled`/`checked` on it any time.
  MenuItem* add_item(StringView text, Handler on_click = {}, StringView shortcut = {});
  MenuItem* add_separator();
  uint32_t item_count() const { return static_cast<uint32_t>(items_.count()); }
  MenuItem* item(uint32_t index) const { return index < items_.count() ? items_[index] : nullptr; }

 private:
  friend class MenuBar;
  MenuItem* add(StringView text, StringView shortcut, bool separator, Handler on_click);

  allocators::IAllocator& allocator_;
  String title_;
  List<MenuItem*> items_;
};

// The drop-down part of a menu (a popup).
class MenuPopup final : public Control {
 public:
  static constexpr int32_t kRowHeight = 20, kSeparatorHeight = 8;

  MenuPopup(allocators::IAllocator& allocator, MenuHost& host) : Control(allocator), host_(host) {}
  void set_min_width(int32_t width) { min_width_ = width; }
  void show(Menu* menu) { menu_ = menu; highlighted_ = -1; }
  void paint(Painter& painter) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;
  void highlight_first() { step(1, -1); }

 protected:
  Size default_size() const override;

 private:
  int32_t item_at(int32_t y) const;
  int32_t row_top(int32_t index) const;
  void step(int32_t direction, int32_t from);
  void run(int32_t index);

  MenuHost& host_;
  int32_t min_width_ = 100;
  Menu* menu_ = nullptr;
  int32_t highlighted_ = -1;
};

class MenuBar : public Control, public MenuHost {
 public:
  explicit MenuBar(allocators::IAllocator& allocator) : Control(allocator), menus_(allocator), popup_(allocator, *this) {}
  ~MenuBar();

  // Adds a menu at the right end of the bar; null if memory ran out.
  Menu* add_menu(StringView title);
  uint32_t menu_count() const { return static_cast<uint32_t>(menus_.count()); }
  Menu* menu(uint32_t index) const { return index < menus_.count() ? menus_[index] : nullptr; }
  int32_t open_menu_index() const { return open_; }  // -1 = none open

  void paint(Painter& painter) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_leave() override { hot_ = -1; invalidate(); }
  void on_popup_closed(Control*) override { open_ = -1; invalidate(); }

  void open_menu(int32_t index);
  void close_menu() override;
  // Used by the popup: opens the neighbouring menu (wrapping around).
  void switch_menu(int32_t direction) override;
  // Used by the popup: runs the item and closes the menu.
  void activate(MenuItem* item) override;

 protected:
  Size default_size() const override { return {0, 22}; }

 private:
  static constexpr int32_t kTitlePad = 8;
  int32_t title_x(int32_t index) const;
  int32_t title_width(int32_t index) const;
  int32_t title_at(int32_t x) const;

  List<Menu*> menus_;
  MenuPopup popup_;
  int32_t open_ = -1;
  int32_t hot_ = -1;
};

}  // namespace gui
