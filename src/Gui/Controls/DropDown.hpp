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
#include "Gui/Control.hpp"
#include "Gui/Controls/StringList.hpp"

namespace gui {

class DropDown;

// The list that drops down (a popup, drawn above the other controls).
class DropDownList final : public Control {
 public:
  static constexpr int32_t kRowHeight = 18;

  DropDownList(allocators::IAllocator& allocator, DropDown& owner) : Control(allocator), owner_(owner) {}
  void paint(Painter& painter) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;
  void start(int32_t highlighted) { highlighted_ = highlighted; }

 protected:
  Size default_size() const override;

 private:
  int32_t row_at(int32_t y) const;

  DropDown& owner_;
  int32_t highlighted_ = -1;
};

// A combo box without free text entry: shows the selected item; clicking it (or Alt+Down, Space,
// Enter) drops the list down, the arrow keys change the selection. All items are shown in the list.
class DropDown : public Control {
 public:
  explicit DropDown(allocators::IAllocator& allocator) : Control(allocator), items_(allocator), list_(allocator, *this) {}

  // Returns the new item's index, or -1 if memory ran out.
  int32_t add_item(StringView text);
  void clear_items();
  int32_t item_count() const { return items_.count(); }
  StringView item(int32_t index) const { return items_.at(index); }
  int32_t selected_index() const { return selected_; }  // -1 = nothing selected
  StringView selected_text() const { return items_.at(selected_); }
  void set_selected_index(int32_t index);
  void on_selection_changed(Handler handler) { UiGuard lock(this); changed_ = handler; }
  bool open() const { return open_; }

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;
  void on_popup_closed(Control*) override { open_ = false; invalidate(); }
  void on_focus_changed(bool) override { invalidate(); }

  // Used by the list.
  void choose(int32_t index);
  const StringList& items() const { return items_; }
  void close_list();

 protected:
  Size default_size() const override { return {140, 24}; }

 private:
  void open_list();

  StringList items_;
  DropDownList list_;
  Handler changed_;
  int32_t selected_ = -1;
  bool open_ = false;
};

}  // namespace gui
