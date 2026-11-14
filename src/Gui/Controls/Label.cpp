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

#include "Gui/Controls/Label.hpp"
#include "Gui/Window.hpp"

namespace gui {

namespace {
// The first control that can take the focus in `from` and what is inside it, in the order they were added.
Control* first_focusable(Control* from) {
  if (!from->visible() || !from->enabled()) return nullptr;
  if (from->focusable()) return from;
  for (uint32_t i = 0; i < from->child_count(); ++i)
    if (Control* found = first_focusable(from->child(i))) return found;
  return nullptr;
}
}  // namespace

void Label::paint(Painter& painter) {
  const int32_t y = (static_cast<int32_t>(bounds_.height) - Painter::kGlyphHeight) / 2;
  const uint32_t color = enabled() ? painter.theme().text : painter.theme().disabled_text;
  painter.text(4, y, text_.view(), color);
  painter.underline(4, y, text_.view(), access_index(0), color);
}

// The focus goes to the control the label names: the next one after it that can take the focus, looked for among the controls that follow the label
// in its parent, then those that follow the parent, and so on up to the frame the label is in.
void Label::access_activate(uint32_t) {
  Window* w = window();
  if (!w) return;
  for (Control* at = this; at->parent() && !at->handler_lane(); at = at->parent()) {
    Control* parent = at->parent();
    bool after = false;
    for (uint32_t i = 0; i < parent->child_count(); ++i) {
      Control* sibling = parent->child(i);
      if (sibling == at) {
        after = true;
      } else if (after) {
        if (Control* found = first_focusable(sibling)) {
          w->set_focus(found);
          return;
        }
      }
    }
  }
}

}  // namespace gui
