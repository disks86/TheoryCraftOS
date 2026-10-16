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

// One line of text, vertically centered.
class Label : public Control {
 public:
  explicit Label(allocators::IAllocator& allocator) : Control(allocator), text_(allocator) {}
  void set_text(StringView text) { text_.assign(text); invalidate(); }
  StringView text() const { return text_.view(); }
  void paint(Painter& painter) override;

 protected:
  Size default_size() const override { return {Painter::text_width(text_.view()) + 8, 22}; }

 private:
  String text_;
};

}  // namespace gui
