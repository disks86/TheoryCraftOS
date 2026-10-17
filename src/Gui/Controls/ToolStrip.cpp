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

#include "Gui/Controls/ToolStrip.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/Move.hpp"

namespace gui {

int32_t ToolStrip::add_tool(StringView name, ToolIcon icon) {
  UiGuard lock(this);
  Tool tool{String(allocator()), icon};
  if (!tool.name.assign(name) || !tools_.push_back(util::move(tool))) return -1;
  if (selected_ < 0) selected_ = 0;
  invalidate();
  return static_cast<int32_t>(tools_.count()) - 1;
}

void ToolStrip::set_columns(uint32_t columns) {
  UiGuard lock(this);
  columns_ = columns ? columns : 1;
  invalidate();
}

void ToolStrip::select_tool(int32_t index) {
  UiGuard lock(this);
  if (index < 0 || index >= static_cast<int32_t>(tools_.count()) || index == selected_) return;
  selected_ = index;
  invalidate();
  fire(changed_);
}

Rect2D ToolStrip::tool_rect(uint32_t index) const {
  return {kPad + static_cast<int32_t>(index % columns_) * kButton, kPad + static_cast<int32_t>(index / columns_) * kButton,
          static_cast<uint32_t>(kButton), static_cast<uint32_t>(kButton)};
}

int32_t ToolStrip::tool_at(int32_t x, int32_t y) const {
  for (uint32_t i = 0; i < tools_.count(); ++i)
    if (contains(tool_rect(i), x, y)) return static_cast<int32_t>(i);
  return -1;
}

Size ToolStrip::content_size() const {
  const uint32_t n = static_cast<uint32_t>(tools_.count());
  const uint32_t columns = n < columns_ ? (n ? n : 1) : columns_;
  const uint32_t rows = n ? (n + columns_ - 1) / columns_ : 1;
  return {2 * kPad + static_cast<int32_t>(columns) * kButton, 2 * kPad + static_cast<int32_t>(rows) * kButton};
}

void ToolStrip::paint(Painter& painter) {
  const Theme& t = painter.theme();
  painter.fill({0, 0, bounds_.width, bounds_.height}, t.face);
  const uint32_t ink = enabled() ? t.text : t.disabled_text;
  for (uint32_t i = 0; i < tools_.count(); ++i) {
    const Rect2D r = tool_rect(i);
    const bool selected = static_cast<int32_t>(i) == selected_;
    painter.fill(r, selected ? t.light : t.face);
    painter.bevel(r, selected);
    const int32_t shift = selected ? 1 : 0;
    const Rect2D icon{r.x + 6 + shift, r.y + 6 + shift, r.width - 12, r.height - 12};
    if (tools_[i].icon) tools_[i].icon(painter, icon, ink);
    if (selected && focused()) painter.focus_frame({r.x + 3, r.y + 3, r.width - 6, r.height - 6});
  }
}

void ToolStrip::set_hover(int32_t index) {
  if (index == hover_) return;
  hover_ = index;
  invalidate();
  fire(hover_changed_);
}

void ToolStrip::on_mouse_move(const MouseArgs& args) { set_hover(tool_at(args.x, args.y)); }
void ToolStrip::on_mouse_leave() { set_hover(-1); }

void ToolStrip::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  const int32_t tool = tool_at(args.x, args.y);
  if (tool >= 0) select_tool(tool);
}

bool ToolStrip::on_key(const KeyArgs& args) {
  using namespace hardware;
  const int32_t columns = static_cast<int32_t>(columns_);
  int32_t step = 0;
  switch (args.usage) {
    case key::kLeft: step = -1; break;
    case key::kRight: step = 1; break;
    case key::kUp: step = -columns; break;
    case key::kDown: step = columns; break;
    default: return false;
  }
  select_tool(selected_ + step);
  return true;
}

}  // namespace gui
