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

#include "Gui/Controls/CheckBox.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

void CheckBox::set_checked(bool checked) {
  UiGuard lock(this);
  if (checked == checked_) return;
  checked_ = checked;
  invalidate();
  fire(changed_);
}

void CheckBox::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const int32_t h = static_cast<int32_t>(bounds_.height);
  const int32_t top = (h - kBox) / 2;
  const Rect2D box{0, top, static_cast<uint32_t>(kBox), static_cast<uint32_t>(kBox)};
  painter.fill(box, pressed_ || !enabled() ? t.face : t.field);
  painter.bevel(box, true);
  if (checked_) {  // a tick, two pixels thick
    const uint32_t color = enabled() ? t.text : t.disabled_text;
    for (int32_t d = 0; d < 2; ++d) {
      painter.line(3, top + 6 + d, 5, top + 8 + d, color);
      painter.line(5, top + 8 + d, 9, top + 4 + d, color);
    }
  }
  const int32_t text_x = kBox + kGap;
  const int32_t text_y = (h - Painter::kGlyphHeight) / 2;
  painter.text(text_x, text_y, text_.view(), enabled() ? t.text : t.disabled_text);
  if (focused() && text_.size() > 0)
    painter.focus_frame({text_x - 2, text_y - 2, static_cast<uint32_t>(Painter::text_width(text_.view()) + 4), static_cast<uint32_t>(Painter::kGlyphHeight + 4)});
  else if (focused())
    painter.focus_frame({-1, top - 1, static_cast<uint32_t>(kBox + 2), static_cast<uint32_t>(kBox + 2)});
}

void CheckBox::on_mouse_down(const MouseArgs& args) {
  if (args.button == MouseButton::Left) {
    held_ = pressed_ = true;
    invalidate();
  }
}

void CheckBox::on_mouse_move(const MouseArgs& args) {
  const bool now = held_ && inside(args);
  if (now != pressed_) {
    pressed_ = now;
    invalidate();
  }
}

void CheckBox::on_mouse_up(const MouseArgs& args) {
  const bool hit = held_ && args.button == MouseButton::Left && inside(args);
  held_ = pressed_ = false;
  invalidate();
  if (hit) toggle();
}

bool CheckBox::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kSpace) {
    toggle();
    return true;
  }
  return false;
}

}  // namespace gui
