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

#include "Gui/Controls/TextBox.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace gui {

namespace {
constexpr int32_t kPad = 4;  // between the border and the text
}

uint32_t TextBox::visible_chars() const {
  const int32_t room = static_cast<int32_t>(bounds_.width) - 2 * kPad;
  return room > 0 ? static_cast<uint32_t>(room / Painter::kGlyphWidth) : 0;
}

void TextBox::scroll_to_caret() {
  const uint32_t vis = visible_chars();
  if (caret_ < scroll_) scroll_ = caret_;
  if (vis && caret_ > scroll_ + vis) scroll_ = caret_ - vis;
}

void TextBox::changed() {
  scroll_to_caret();
  invalidate();
  fire(changed_);
}

bool TextBox::set_text(StringView text) {
  UiGuard lock(this);
  if (max_length_ && text.size() > max_length_) text = text.substr(0, max_length_);
  if (!text_.assign(text)) return false;
  caret_ = static_cast<uint32_t>(text_.size());
  scroll_ = 0;
  changed();
  return true;
}

void TextBox::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  painter.push_frame({2, 2, bounds_.width > 4 ? bounds_.width - 4 : 0, bounds_.height > 4 ? bounds_.height - 4 : 0});
  const int32_t y = (static_cast<int32_t>(bounds_.height) - Painter::kGlyphHeight) / 2 - 2;
  const uint32_t vis = visible_chars();
  const uint64_t from = scroll_ < text_.size() ? scroll_ : text_.size();
  uint64_t count = text_.size() - from;
  if (count > vis + 1) count = vis + 1;  // one partial character may show
  painter.text(kPad - 2, y, text_.view().substr(from, count), enabled() ? t.text : t.disabled_text);
  if (focused() && enabled()) {
    const int32_t cx = kPad - 2 + static_cast<int32_t>(caret_ - scroll_) * Painter::kGlyphWidth;
    painter.line(cx, y, cx, y + Painter::kGlyphHeight - 1, t.text);
  }
  painter.pop_frame();
}

void TextBox::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  int32_t col = (args.x - kPad + Painter::kGlyphWidth / 2) / Painter::kGlyphWidth;
  if (args.x < kPad) col = 0;
  int64_t index = static_cast<int64_t>(scroll_) + col;
  if (index > static_cast<int64_t>(text_.size())) index = static_cast<int64_t>(text_.size());
  caret_ = static_cast<uint32_t>(index < 0 ? 0 : index);
  invalidate();
}

bool TextBox::on_key(const KeyArgs& args) {
  using namespace hardware;
  const uint32_t length = static_cast<uint32_t>(text_.size());
  switch (args.usage) {
    case key::kLeft:
      if (caret_ > 0) --caret_;
      scroll_to_caret();
      invalidate();
      return true;
    case key::kRight:
      if (caret_ < length) ++caret_;
      scroll_to_caret();
      invalidate();
      return true;
    case 0x4A:  // Home
      caret_ = 0;
      scroll_to_caret();
      invalidate();
      return true;
    case 0x4D:  // End
      caret_ = length;
      scroll_to_caret();
      invalidate();
      return true;
    case key::kBackspace:
      if (!read_only_ && caret_ > 0 && text_.erase(caret_ - 1, 1)) {
        --caret_;
        changed();
      }
      return true;
    case 0x4C:  // Delete
      if (!read_only_ && caret_ < length && text_.erase(caret_, 1)) changed();
      return true;
    default:
      break;
  }
  const char c = args.character;
  if (c >= 32 && c < 127 && !(args.modifiers & modifier::kControl)) {
    if (!read_only_ && (!max_length_ || length < max_length_) && text_.insert(caret_, StringView(&c, 1))) {
      ++caret_;
      changed();
    }
    return true;
  }
  return false;  // Tab, Enter, Escape and friends go to the parent
}

}  // namespace gui
