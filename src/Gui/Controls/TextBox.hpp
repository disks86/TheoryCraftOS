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
#include "Utility/String.hpp"

namespace gui {

// A single-line text editor: typing inserts at the caret, Backspace/Delete/Left/Right/Home/End edit and
// move it, a click places it. Long text scrolls horizontally. (No selection or clipboard yet.)
class TextBox : public Control {
 public:
  explicit TextBox(allocators::IAllocator& allocator) : Control(allocator), text_(allocator) {}

  StringView text() const { return text_.view(); }
  // Replaces the text (the caret goes to the end). False if memory ran out.
  bool set_text(StringView text);
  void set_read_only(bool read_only) { read_only_ = read_only; }
  void set_max_length(uint32_t max) { max_length_ = max; }
  uint32_t caret() const { return caret_; }
  void on_changed(Handler handler) { changed_ = handler; }

  bool focusable() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  Size default_size() const override { return {160, 24}; }

 private:
  void scroll_to_caret();
  uint32_t visible_chars() const;
  void changed();

  String text_;
  Handler changed_;
  uint32_t caret_ = 0;   // between characters: 0 .. text length
  uint32_t scroll_ = 0;  // first visible character
  uint32_t max_length_ = 0;  // 0 = unlimited
  bool read_only_ = false;
};

}  // namespace gui
