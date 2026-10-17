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

#include "Gui/Controls/RadioButton.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

// --- RadioGroup ----------------------------------------------------------------

uint32_t RadioGroup::count() const {
  uint32_t n = 0;
  for (const RadioButton* b = first_; b; b = b->next_) ++n;
  return n;
}

RadioButton* RadioGroup::at(uint32_t index) const {
  RadioButton* b = first_;
  for (; b && index > 0; --index) b = b->next_;
  return b;
}

RadioButton* RadioGroup::selected() const {
  for (RadioButton* b = first_; b; b = b->next_)
    if (b->checked_) return b;
  return nullptr;
}

int32_t RadioGroup::selected_index() const {
  int32_t i = 0;
  for (const RadioButton* b = first_; b; b = b->next_, ++i)
    if (b->checked_) return i;
  return -1;
}

void RadioGroup::select(uint32_t index) {
  if (RadioButton* b = at(index)) b->set_checked(true);
}

// --- RadioButton ---------------------------------------------------------------

void RadioButton::set_group(RadioGroup* group) {
  UiGuard lock(this);
  if (group == group_) return;
  if (group_) {  // leave the old group
    RadioButton** link = &group_->first_;
    while (*link && *link != this) link = &(*link)->next_;
    if (*link) *link = next_;
  }
  group_ = group;
  next_ = nullptr;
  if (group_) {  // join at the end
    RadioButton** link = &group_->first_;
    while (*link) link = &(*link)->next_;
    *link = this;
    // A group has at most one checked button: an earlier one keeps the check.
    if (checked_)
      for (RadioButton* b = group_->first_; b; b = b->next_)
        if (b != this && b->checked_) {
          checked_ = false;
          break;
        }
  }
  invalidate();
}

void RadioButton::set_checked(bool checked) {
  UiGuard lock(this);
  if (checked == checked_) return;
  checked_ = checked;
  invalidate();
  if (checked_ && group_) {
    for (RadioButton* b = group_->first_; b; b = b->next_) {
      if (b == this || !b->checked_) continue;
      b->checked_ = false;
      b->invalidate();
      b->fire(b->changed_);
    }
  }
  fire(changed_);
}

void RadioButton::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const int32_t h = static_cast<int32_t>(bounds_.height);
  const int32_t top = (h - kCircle) / 2;
  const uint32_t face = pressed_ || !enabled() ? t.face : t.field;
  // A 13-pixel disc, drawn row by row; the outline is the sunken bevel's two tones.
  static const int8_t kHalfWidth[kCircle] = {2, 4, 5, 5, 6, 6, 6, 6, 6, 5, 5, 4, 2};
  for (int32_t row = 0; row < kCircle; ++row) {
    const int32_t half = kHalfWidth[row];
    const int32_t cx = kCircle / 2;
    painter.fill({cx - half, top + row, static_cast<uint32_t>(2 * half + 1), 1}, face);
    const bool upper = row < kCircle / 2;  // the lit side is the lower right, the dark side the upper left
    painter.fill({cx - half, top + row, 1, 1}, upper ? t.shadow : t.highlight);
    painter.fill({cx + half, top + row, 1, 1}, upper ? t.highlight : t.shadow);
    if (row == 0 || row == kCircle - 1) painter.fill({cx - half, top + row, static_cast<uint32_t>(2 * half + 1), 1}, row == 0 ? t.shadow : t.highlight);
  }
  if (checked_) {
    const uint32_t color = enabled() ? t.text : t.disabled_text;
    painter.fill({kCircle / 2 - 2, top + kCircle / 2 - 2, 5, 5}, color);
    painter.fill({kCircle / 2 - 3, top + kCircle / 2 - 1, 7, 3}, color);
    painter.fill({kCircle / 2 - 1, top + kCircle / 2 - 3, 3, 7}, color);
  }
  const int32_t text_x = kCircle + kGap;
  const int32_t text_y = (h - Painter::kGlyphHeight) / 2;
  painter.text(text_x, text_y, text_.view(), enabled() ? t.text : t.disabled_text);
  if (focused() && text_.size() > 0)
    painter.focus_frame({text_x - 2, text_y - 2, static_cast<uint32_t>(Painter::text_width(text_.view()) + 4), static_cast<uint32_t>(Painter::kGlyphHeight + 4)});
  else if (focused())
    painter.focus_frame({-1, top - 1, static_cast<uint32_t>(kCircle + 2), static_cast<uint32_t>(kCircle + 2)});
}

void RadioButton::on_mouse_down(const MouseArgs& args) {
  if (args.button == MouseButton::Left) {
    held_ = pressed_ = true;
    invalidate();
  }
}

void RadioButton::on_mouse_move(const MouseArgs& args) {
  const bool now = held_ && inside(args);
  if (now != pressed_) {
    pressed_ = now;
    invalidate();
  }
}

void RadioButton::on_mouse_up(const MouseArgs& args) {
  const bool hit = held_ && args.button == MouseButton::Left && inside(args);
  held_ = pressed_ = false;
  invalidate();
  if (hit) set_checked(true);
}

// Moves the check (and the focus) to the neighbour in the group, skipping buttons that cannot be used.
void RadioButton::step(int direction) {
  if (!group_) return;
  const uint32_t n = group_->count();
  int32_t index = -1;
  for (uint32_t i = 0; i < n; ++i)
    if (group_->at(i) == this) index = static_cast<int32_t>(i);
  if (index < 0) return;
  for (uint32_t tries = 0; tries < n; ++tries) {
    index = (index + direction + static_cast<int32_t>(n)) % static_cast<int32_t>(n);
    RadioButton* other = group_->at(static_cast<uint32_t>(index));
    if (other == this || !other->effectively_enabled() || !other->visible()) continue;
    other->set_checked(true);
    if (Window* w = other->window()) w->set_focus(other);
    return;
  }
}

bool RadioButton::on_key(const KeyArgs& args) {
  using namespace hardware;
  switch (args.usage) {
    case key::kSpace:
      set_checked(true);
      return true;
    case key::kDown:
    case key::kRight:
      step(+1);
      return true;
    case key::kUp:
    case key::kLeft:
      step(-1);
      return true;
    default:
      return false;
  }
}

}  // namespace gui
