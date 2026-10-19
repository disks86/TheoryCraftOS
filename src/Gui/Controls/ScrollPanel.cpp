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

#include "Gui/Controls/ScrollPanel.hpp"

namespace gui {

ScrollPanel::ScrollPanel(allocators::IAllocator& allocator) : Panel(allocator), bar_(allocator) {
  set_style(PanelStyle::Transparent);
  bar_.set_visible(false);
  bar_.set_step(22);
  bar_.on_value_changed(Handler::immediate(&ScrollPanel::on_scrolled, this));
  add(&bar_);
}

void ScrollPanel::scroll_to(int32_t offset) { bar_.set_value(offset); }

void ScrollPanel::on_scrolled(void* self, Control&) { static_cast<ScrollPanel*>(self)->layout(); }

void ScrollPanel::layout() {
  const Rect2D area = client_rect();
  int32_t total = 0;
  for (uint32_t i = 0; i < child_count(); ++i) {
    Control* c = child(i);
    if (c != &bar_ && c->visible()) total += c->preferred_size().height;
  }
  content_height_ = total;
  const bool needs_bar = total > static_cast<int32_t>(area.height);
  bar_.set_visible(needs_bar);
  const int32_t bar_width = needs_bar ? ScrollBar::kThickness : 0;
  const uint32_t width = area.width > static_cast<uint32_t>(bar_width) ? area.width - static_cast<uint32_t>(bar_width) : 0;
  bar_.set_metrics(total, static_cast<int32_t>(area.height));   // (keeps the value inside the new range)
  if (needs_bar) {
    bar_.set_bounds({area.x + static_cast<int32_t>(width), area.y, static_cast<uint32_t>(ScrollBar::kThickness), area.height});
    bar_.layout();
  }
  int32_t y = area.y - bar_.value();
  for (uint32_t i = 0; i < child_count(); ++i) {
    Control* c = child(i);
    if (c == &bar_ || !c->visible()) continue;
    const int32_t h = c->preferred_size().height;
    c->set_bounds({area.x, y, width, static_cast<uint32_t>(h < 0 ? 0 : h)});
    y += h;
    c->layout();
  }
}

}  // namespace gui
