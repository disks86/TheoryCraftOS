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

#include "Gui/Controls/Panel.hpp"

namespace gui {

void Panel::paint(Painter& painter) {
  if (style_ == PanelStyle::Transparent) return;
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, painter.theme().face);
  if (style_ == PanelStyle::Raised) painter.bevel(all, false);
  if (style_ == PanelStyle::Sunken) painter.bevel(all, true);
}

void UniformPanel::layout() {
  const Rect2D area = client_rect();
  uint32_t visible = 0;
  for (uint32_t i = 0; i < child_count(); ++i) visible += child(i)->visible();
  if (visible == 0) return;
  const bool horizontal = orientation_ == Orientation::Horizontal;
  const int64_t length = horizontal ? area.width : area.height;
  const int64_t gaps = static_cast<int64_t>(spacing_) * (visible - 1);
  const int64_t usable = length > gaps ? length - gaps : 0;
  const int64_t each = usable / visible, extra = usable % visible;
  int64_t at = horizontal ? area.x : area.y;
  uint32_t index = 0;
  for (uint32_t i = 0; i < child_count(); ++i) {
    Control* c = child(i);
    if (!c->visible()) continue;
    const int64_t size = each + (index < extra ? 1 : 0);
    c->set_bounds(horizontal ? Rect2D{static_cast<int32_t>(at), area.y, static_cast<uint32_t>(size), area.height}
                             : Rect2D{area.x, static_cast<int32_t>(at), area.width, static_cast<uint32_t>(size)});
    at += size + spacing_;
    ++index;
    c->layout();
  }
}

}  // namespace gui
