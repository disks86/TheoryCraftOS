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

// Panels that place their children one after the other in the order they were added: a StackColumn from top to bottom, a StackRow from left to right.
// Every child gets the height (width) it asks for; the one child docked Fill gets whatever is left over, wherever it is in the order.
//
// Why not dock styles alone: Tab walks the controls in the order they were added, and so do the access keys of labels. With dock styles the
// row of buttons along the bottom of a dialog has to be added before the content that fills the rest, and the rightmost button before the one
// left of it, so Tab would reach Cancel first, then OK, then the fields. The guidelines want the reverse (docs/UserInterfaceGuidelines.md, 11.1:
// the Tab order follows the reading order, from the main field to the transaction buttons). Here the order on the screen is the order in the tree.
#pragma once
#include "Gui/Controls/Panel.hpp"

namespace desktop {

class StackPanel : public gui::Panel {
 public:
  StackPanel(allocators::IAllocator& allocator, gui::Orientation orientation) : Panel(allocator), orientation_(orientation) { set_style(gui::PanelStyle::Transparent); }

  void layout() override {
    const gui::Rect2D area = client_rect();
    const bool down = orientation_ == gui::Orientation::Vertical;
    const int64_t room = down ? area.height : area.width;
    // What the children with a size of their own take; the rest goes to the Fill child.
    int64_t fixed = 0;
    for (uint32_t i = 0; i < child_count(); ++i) {
      const gui::Control* c = child(i);
      if (!c->visible() || c->dock() == gui::DockStyle::Fill) continue;
      const gui::Size want = c->preferred_size();
      fixed += down ? want.height : want.width;
    }
    const int64_t rest = room > fixed ? room - fixed : 0;
    int64_t at = 0;
    for (uint32_t i = 0; i < child_count(); ++i) {
      gui::Control* c = child(i);
      if (!c->visible()) continue;
      const gui::Size want = c->preferred_size();
      int64_t length = c->dock() == gui::DockStyle::Fill ? rest : (down ? want.height : want.width);
      if (length < 0) length = 0;
      if (at + length > room) length = room > at ? room - at : 0;   // (what does not fit is cut off, never drawn outside)
      if (down) place(*c, {area.x, area.y + static_cast<int32_t>(at), area.width, static_cast<uint32_t>(length)});
      else place(*c, {area.x + static_cast<int32_t>(at), area.y, static_cast<uint32_t>(length), area.height});
      at += length;
      c->layout();
    }
  }

 private:
  gui::Orientation orientation_;
};

// Top to bottom.
class StackColumn final : public StackPanel {
 public:
  explicit StackColumn(allocators::IAllocator& allocator) : StackPanel(allocator, gui::Orientation::Vertical) {}
};

// Left to right. A row of dialog buttons at the right edge starts with an empty Fill panel.
class StackRow final : public StackPanel {
 public:
  explicit StackRow(allocators::IAllocator& allocator) : StackPanel(allocator, gui::Orientation::Horizontal) {}
};

}  // namespace desktop
