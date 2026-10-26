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

class MenuItem {
 public:
  MenuItem(allocators::IAllocator& allocator, const Control* owner) : text(allocator), shortcut(allocator), owner_(owner) {}
  String text;
  String shortcut;  // display only, e.g. "Ctrl+O"
  Handler on_click;
  bool separator = false;

  // Thread-safe: may be changed from any thread while the menu is in use.
  bool enabled() const { return enabled_; }
  void set_enabled(bool enabled) { UiGuard lock(owner_); enabled_ = enabled; if (owner_) owner_->invalidate(); }
  bool checked() const { return checked_; }
  void set_checked(bool checked) { UiGuard lock(owner_); checked_ = checked; if (owner_) owner_->invalidate(); }

 private:
  const Control* owner_;  // the control the menu belongs to: its window's lock protects the flags
  bool enabled_ = true;
  bool checked_ = false;
};

class Menu {
 public:
  // `owner` is the MenuBar / MenuButton the menu belongs to (for locking and repainting).
  Menu(allocators::IAllocator& allocator, const Control* owner) : allocator_(allocator), owner_(owner), title_(allocator), items_(allocator) {}
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
  const Control* owner_;
  String title_;
  List<MenuItem*> items_;
};

// The drop-down part of a menu (a popup).
class MenuPopup final : public Control {
 public:
  static constexpr int32_t kRowHeight = 20, kSeparatorHeight = 8, kSearchHeight = 28;

  MenuPopup(allocators::IAllocator& allocator, MenuHost& host) : Control(allocator), host_(host), query_(allocator) {}
  void set_min_width(int32_t width) { min_width_ = width; }
  // `armed_`: a menu opened by a mouse press ignores the release of that press until the pointer has moved, so a click that opens it at
  // the pointer cannot also choose an item.
  void show(Menu* menu) { menu_ = menu; highlighted_ = -1; armed_ = false; query_.clear(); }
  // A search box along the bottom that is always focused: typing filters the items (case-insensitive, fuzzy: the typed letters must appear in
  // the item's text in that order, not necessarily next to each other), Backspace erases, Enter runs the highlighted item or else the first
  // match. Separators are hidden while something is typed.
  void set_searchable(bool searchable) { searchable_ = searchable; }
  StringView query() const { return query_.view(); }
  // True if `text` contains the letters of `query` in order, ignoring case. An empty query matches everything.
  static bool fuzzy_match(StringView query, StringView text);
  void paint(Painter& painter) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;
  void highlight_first() { step(1, -1); }

 protected:
  Size default_size() const override;

 private:
  bool visible(uint32_t index) const;
  int32_t row_height(uint32_t index) const;
  void edit_query(bool erase, char c);
  int32_t item_at(int32_t y) const;
  int32_t row_top(int32_t index) const;
  void step(int32_t direction, int32_t from);
  void run(int32_t index);

  MenuHost& host_;
  int32_t min_width_ = 100;
  Menu* menu_ = nullptr;
  int32_t highlighted_ = -1;
  bool armed_ = false;
  bool searchable_ = false;
  String query_;
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
  // The item with this text in the menu with this title (for enabling, checking or running it from code); nullptr if there is none.
  MenuItem* find_item(StringView menu_title, StringView item_text) const;

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
