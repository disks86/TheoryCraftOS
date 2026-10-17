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

#include "Gui/Controls/SpinBox.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

SpinBox::SpinBox(allocators::IAllocator& allocator) : Control(allocator), edit_(allocator, *this) {
  edit_.on_changed(Handler::immediate(&SpinBox::text_changed, this));
  add(&edit_);
  show_value();
}

void SpinBox::text_changed(void* self, Control&) { static_cast<SpinBox*>(self)->on_text_changed(); }

// What is typed becomes the value as far as it makes sense; anything that is not part of a number is thrown out.
void SpinBox::on_text_changed() {
  if (updating_) return;
  const StringView text = edit_.text();
  bool valid = true;
  int64_t number = 0;
  bool negative = false, digits = false;
  for (uint64_t i = 0; i < text.size() && valid; ++i) {
    const char c = text[i];
    if (c == '-' && i == 0 && min_ < 0) negative = true;
    else if (c >= '0' && c <= '9' && i < 10) {
      number = number * 10 + (c - '0');
      digits = true;
    } else {
      valid = false;
    }
  }
  if (!valid) {  // put back what the value says
    show_value();
    return;
  }
  if (!digits) return;  // empty (or just "-"): the value stays until the box is left
  const int32_t typed = static_cast<int32_t>(negative ? -number : number);
  const int32_t clamped = typed < min_ ? min_ : typed > max_ ? max_ : typed;
  if (clamped != value_) {
    value_ = clamped;
    invalidate();
    fire(changed_);
  }
}

void SpinBox::show_value() {
  updating_ = true;
  String text(allocator());
  text.append_int(value_);
  edit_.set_text(text.view());
  updating_ = false;
}

void SpinBox::normalize() {
  UiGuard lock(this);
  show_value();
}

void SpinBox::set_range(int32_t minimum, int32_t maximum) {
  UiGuard lock(this);
  min_ = minimum;
  max_ = maximum < minimum ? minimum : maximum;
  const int32_t old = value_;
  if (value_ < min_) value_ = min_;
  if (value_ > max_) value_ = max_;
  show_value();
  if (value_ != old) fire(changed_);
}

void SpinBox::set_value(int32_t value) {
  UiGuard lock(this);
  value = value < min_ ? min_ : value > max_ ? max_ : value;
  const bool changed = value != value_;
  value_ = value;
  show_value();
  if (changed) fire(changed_);
}

void SpinBox::step_by(int32_t direction) {
  if (direction == 0 || !enabled()) return;
  set_value(static_cast<int32_t>(static_cast<int64_t>(value_) + static_cast<int64_t>(direction) * step_));
}

Rect2D SpinBox::up_rect() const {
  const int32_t h = static_cast<int32_t>(bounds_.height);
  return {static_cast<int32_t>(bounds_.width) - kButtonWidth, 0, static_cast<uint32_t>(kButtonWidth), static_cast<uint32_t>(h / 2)};
}

Rect2D SpinBox::down_rect() const {
  const int32_t h = static_cast<int32_t>(bounds_.height);
  return {static_cast<int32_t>(bounds_.width) - kButtonWidth, h / 2, static_cast<uint32_t>(kButtonWidth), static_cast<uint32_t>(h - h / 2)};
}

void SpinBox::layout() {
  const int32_t w = static_cast<int32_t>(bounds_.width) - kButtonWidth;
  edit_.set_bounds({0, 0, static_cast<uint32_t>(w > 0 ? w : 0), bounds_.height});
  Control::layout();
}

void SpinBox::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const uint32_t ink = enabled() ? t.text : t.disabled_text;
  for (int up = 1; up >= 0; --up) {
    const Rect2D r = up ? up_rect() : down_rect();
    painter.fill(r, t.face);
    painter.bevel(r, false);
    const int32_t cx = r.x + static_cast<int32_t>(r.width) / 2, cy = r.y + static_cast<int32_t>(r.height) / 2;
    for (int32_t i = 0; i < 3; ++i) {  // a small triangle
      const int32_t y = up ? cy - 1 + i : cy + 1 - i;
      painter.line(cx - i, y, cx + i, y, ink);
    }
  }
}

void SpinBox::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  if (contains(up_rect(), args.x, args.y)) step_by(1);
  else if (contains(down_rect(), args.x, args.y)) step_by(-1);
}

bool SpinBox::on_key(const KeyArgs& args) {
  using namespace hardware;
  switch (args.usage) {
    case key::kUp: step_by(1); return true;
    case key::kDown: step_by(-1); return true;
    case 0x4B: step_by(10); return true;   // Page Up
    case 0x4E: step_by(-10); return true;  // Page Down
    case key::kEnter: normalize(); return false;  // the parent (a dialog) may take Enter as well
    default: return false;
  }
}

}  // namespace gui
