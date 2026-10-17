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

#include "Gui/Controls/Toggle.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

void Toggle::set_on(bool on) {
  UiGuard lock(this);
  if (on == on_) return;
  on_ = on;
  invalidate();
  fire(changed_);
}

void Toggle::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const int32_t h = static_cast<int32_t>(bounds_.height);
  const int32_t top = (h - kTrackHeight) / 2;
  const Rect2D track{0, top, static_cast<uint32_t>(kTrackWidth), static_cast<uint32_t>(kTrackHeight)};
  painter.fill(track, !enabled() ? t.face : on_ ? t.selection : t.field);
  painter.bevel(track, true);
  // The knob: a raised block that fills one half of the track.
  const int32_t knob_w = kTrackWidth / 2 - 2, knob_h = kTrackHeight - 4;
  const Rect2D knob{on_ ? kTrackWidth - 2 - knob_w : 2, top + 2, static_cast<uint32_t>(knob_w), static_cast<uint32_t>(knob_h)};
  painter.fill(knob, t.face);
  painter.bevel(knob, pressed_);
  const int32_t text_x = kTrackWidth + kGap;
  const int32_t text_y = (h - Painter::kGlyphHeight) / 2;
  painter.text(text_x, text_y, text_.view(), enabled() ? t.text : t.disabled_text);
  if (focused()) {
    if (text_.size() > 0)
      painter.focus_frame({text_x - 2, text_y - 2, static_cast<uint32_t>(Painter::text_width(text_.view()) + 4), static_cast<uint32_t>(Painter::kGlyphHeight + 4)});
    else
      painter.focus_frame({-1, top - 1, static_cast<uint32_t>(kTrackWidth + 2), static_cast<uint32_t>(kTrackHeight + 2)});
  }
}

void Toggle::on_mouse_down(const MouseArgs& args) {
  if (args.button == MouseButton::Left) {
    held_ = pressed_ = true;
    invalidate();
  }
}

void Toggle::on_mouse_move(const MouseArgs& args) {
  const bool now = held_ && inside(args);
  if (now != pressed_) {
    pressed_ = now;
    invalidate();
  }
}

void Toggle::on_mouse_up(const MouseArgs& args) {
  const bool hit = held_ && args.button == MouseButton::Left && inside(args);
  held_ = pressed_ = false;
  invalidate();
  if (hit) flip();
}

bool Toggle::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kSpace) {
    flip();
    return true;
  }
  return false;
}

}  // namespace gui
