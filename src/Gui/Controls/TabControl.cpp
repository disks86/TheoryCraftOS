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

#include "Gui/Controls/TabControl.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/Move.hpp"

namespace gui {

int32_t TabControl::add_page(StringView title, Control* page) {
  UiGuard lock(this);
  if (!page) return -1;
  String name(allocator());
  if (!name.assign(title) || !titles_.push_back(util::move(name))) return -1;
  if (!pages_.push_back(page) || !add(page)) {
    titles_.remove_at(titles_.count() - 1);
    if (pages_.count() > titles_.count()) pages_.remove_at(pages_.count() - 1);
    return -1;
  }
  page->set_dock(DockStyle::Fill);
  const int32_t index = static_cast<int32_t>(pages_.count()) - 1;
  const bool was_empty = selected_ < 0;
  page->set_visible(index == 0);
  if (was_empty) selected_ = 0;
  if (was_empty) invalidate();  // only invalidate if we actually made a change (first page or visible change)
  return index;
}

void TabControl::select_page(int32_t index) {
  UiGuard lock(this);
  if (index < 0 || index >= static_cast<int32_t>(pages_.count()) || index == selected_) return;
  selected_ = index;
  for (uint32_t i = 0; i < pages_.count(); ++i) pages_[i]->set_visible(static_cast<int32_t>(i) == selected_);
  invalidate();
  fire(changed_);
}

void TabControl::cycle(int32_t direction) {
  const int32_t n = static_cast<int32_t>(pages_.count());
  if (n > 1) select_page(((selected_ + direction) % n + n) % n);
}

Rect2D TabControl::tab_rect(uint32_t index) const {
  int32_t x = kBorder;
  for (uint32_t i = 0; i < index && i < titles_.count(); ++i) x += Painter::text_width(titles_[i].view()) + 2 * kTabPad;
  const int32_t w = index < titles_.count() ? Painter::text_width(titles_[index].view()) + 2 * kTabPad : 0;
  const bool selected = static_cast<int32_t>(index) == selected_;
  return {x - (selected ? 2 : 0), selected ? 0 : 2, static_cast<uint32_t>(w + (selected ? 4 : 0)),
          static_cast<uint32_t>(selected ? kTabHeight : kTabHeight - 2)};
}

Rect2D TabControl::page_rect() const {
  const int32_t w = static_cast<int32_t>(bounds_.width) - 2 * kBorder, h = static_cast<int32_t>(bounds_.height) - kTabHeight - kBorder;
  return {kBorder, kTabHeight - 1 + kBorder, static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(h - 1 > 0 ? h - 1 : 0)};
}

int32_t TabControl::tab_at(int32_t x, int32_t y) const {
  for (uint32_t i = 0; i < titles_.count(); ++i)
    if (contains(tab_rect(i), x, y)) return static_cast<int32_t>(i);
  return -1;
}

void TabControl::paint(Painter& painter) {
  const Theme& t = painter.theme();
  painter.fill({0, 0, bounds_.width, bounds_.height}, t.face);
  const int32_t body_top = kTabHeight - 1;
  painter.bevel({0, body_top, bounds_.width, bounds_.height > static_cast<uint32_t>(body_top) ? bounds_.height - body_top : 0}, false);
  const uint32_t ink = enabled() ? t.text : t.disabled_text;
  for (uint32_t i = 0; i < titles_.count(); ++i) {
    const Rect2D r = tab_rect(i);
    const bool selected = static_cast<int32_t>(i) == selected_;
    painter.fill(r, t.face);
    painter.bevel(r, false);
    if (selected) painter.fill({r.x + 2, r.y + static_cast<int32_t>(r.height) - 2, r.width > 4 ? r.width - 4 : 0, 4}, t.face);  // joins the page
    painter.text_centered({r.x, r.y, r.width, r.height - (selected ? 0u : 2u)}, titles_[i].view(), ink);
    if (selected && focused()) painter.focus_frame({r.x + 4, r.y + 3, r.width > 8 ? r.width - 8 : 0, static_cast<uint32_t>(kTabHeight - 8)});
  }
}

void TabControl::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  const int32_t tab = tab_at(args.x, args.y);
  if (tab >= 0) select_page(tab);
}

bool TabControl::on_key(const KeyArgs& args) {
  using namespace hardware;
  if (args.usage == key::kTab && (args.modifiers & modifier::kControl)) {
    cycle((args.modifiers & modifier::kShift) ? -1 : 1);
    return true;
  }
  if (focused()) {
    if (args.usage == key::kLeft) { cycle(-1); return true; }
    if (args.usage == key::kRight) { cycle(1); return true; }
  }
  return false;
}

}  // namespace gui
