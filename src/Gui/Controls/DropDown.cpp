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

#include "Gui/Controls/DropDown.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

// --- DropDownList --------------------------------------------------------------

Size DropDownList::default_size() const {
  const int32_t rows = owner_.item_count() > 0 ? owner_.item_count() : 1;
  return {static_cast<int32_t>(owner_.bounds().width), rows * kRowHeight + 4};
}

int32_t DropDownList::row_at(int32_t y) const {
  const int32_t row = (y - 2) / kRowHeight;
  return y >= 2 && row < owner_.item_count() ? row : -1;
}

void DropDownList::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.outline(all, t.text);
  for (int32_t i = 0; i < owner_.item_count(); ++i) {
    const Rect2D row{1, 2 + i * kRowHeight, bounds_.width - 2, static_cast<uint32_t>(kRowHeight)};
    const bool hot = i == highlighted_;
    if (hot) painter.fill(row, t.selection);
    painter.text(row.x + 4, row.y + (kRowHeight - Painter::kGlyphHeight) / 2, owner_.item(i),
                 hot ? t.selection_text : t.text);
  }
}

void DropDownList::on_mouse_move(const MouseArgs& args) {
  const int32_t row = row_at(args.y);
  if (row != highlighted_ && contains({0, 0, bounds_.width, bounds_.height}, args.x, args.y)) {
    highlighted_ = row;
    invalidate();
  }
}

void DropDownList::on_mouse_up(const MouseArgs& args) {
  const int32_t row = row_at(args.y);
  if (row >= 0) owner_.choose(row);
}

bool DropDownList::on_key(const KeyArgs& args) {
  using namespace hardware;
  const int32_t n = owner_.item_count();
  switch (args.usage) {
    case key::kDown:
      if (n) highlighted_ = highlighted_ + 1 < n ? highlighted_ + 1 : 0;
      invalidate();
      return true;
    case key::kUp:
      if (n) highlighted_ = highlighted_ > 0 ? highlighted_ - 1 : n - 1;
      invalidate();
      return true;
    case key::kEnter:
    case key::kSpace:
      if (highlighted_ >= 0) owner_.choose(highlighted_);
      else owner_.close_list();
      return true;
    case key::kEscape:
      owner_.close_list();
      return true;
    default:
      return true;  // a popup swallows other keys
  }
}

// --- DropDown ------------------------------------------------------------------

int32_t DropDown::add_item(StringView text) {
  UiGuard lock(this);
  const int32_t index = items_.add(text);
  invalidate();
  return index;
}

void DropDown::clear_items() {
  UiGuard lock(this);
  close_list();
  items_.clear();
  selected_ = -1;
  invalidate();
}

void DropDown::set_selected_index(int32_t index) {
  UiGuard lock(this);
  if (index < -1 || index >= items_.count()) return;
  if (index == selected_) return;
  selected_ = index;
  invalidate();
  fire(changed_);
}

void DropDown::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  const uint32_t text_color = enabled() ? t.text : t.disabled_text;
  const Rect2D label{3, 3, static_cast<uint32_t>(w > 24 ? w - 24 : 0), static_cast<uint32_t>(h > 6 ? h - 6 : 0)};
  if (focused() && !open_) painter.fill(label, t.selection);
  painter.text(label.x + 2, (h - Painter::kGlyphHeight) / 2, selected_text(),
               focused() && !open_ ? t.selection_text : text_color);
  // The arrow button.
  const Rect2D button{w - 19, 2, 17, static_cast<uint32_t>(h > 4 ? h - 4 : 0)};
  painter.fill(button, t.face);
  painter.bevel(button, open_);
  const int32_t cx = button.x + 8 + (open_ ? 1 : 0), cy = h / 2 - 1 + (open_ ? 1 : 0);
  for (int32_t i = 0; i < 4; ++i) painter.line(cx - 3 + i, cy + i - 1, cx + 3 - i, cy + i - 1, text_color);
}

void DropDown::open_list() {
  Window* w = window();
  if (!w || items_.count() == 0) return;
  const Point o = window_origin();
  list_.start(selected_);
  if (w->open_popup(&list_, this, o.x, o.y + static_cast<int32_t>(bounds_.height))) open_ = true;
  invalidate();
}

void DropDown::close_list() {
  UiGuard lock(this);
  if (open_ && window()) window()->close_popup();
  open_ = false;
}

void DropDown::choose(int32_t index) {
  UiGuard lock(this);
  close_list();
  set_selected_index(index);
}

void DropDown::on_mouse_down(const MouseArgs& args) {
  if (args.button == MouseButton::Left) open_list();  // a click while open is consumed by the window
}

bool DropDown::on_key(const KeyArgs& args) {
  using namespace hardware;
  const int32_t n = items_.count();
  switch (args.usage) {
    case key::kDown:
      if (args.modifiers & (modifier::kLeftAlt | modifier::kRightAlt)) {
        open_list();
      } else if (n) {
        set_selected_index(selected_ + 1 < n ? selected_ + 1 : n - 1);
      }
      return true;
    case key::kUp:
      if (n) set_selected_index(selected_ > 0 ? selected_ - 1 : 0);
      return true;
    case key::kSpace:
    case key::kEnter:
      open_list();
      return true;
    default:
      return false;
  }
}

}  // namespace gui
