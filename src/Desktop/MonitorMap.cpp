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

#include "Desktop/MonitorMap.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/Move.hpp"

namespace desktop {

using namespace gui;

void MonitorMap::clear() {
  UiGuard lock(this);
  boxes_.clear();
  if (dragging_ != kNone) dragging_ = pressed_ = kNone;
  invalidate();
}

bool MonitorMap::add_box(uint32_t id, const hardware::MonitorRect& rect, StringView name, bool primary) {
  UiGuard lock(this);
  Box box(allocator());
  box.id = id;
  box.rect = rect;
  box.primary = primary;
  box.name.assign(name);
  if (!boxes_.push_back(util::move(box))) return false;
  invalidate();
  return true;
}

int32_t MonitorMap::index_of(uint32_t id) const {
  for (uint64_t i = 0; i < boxes_.count(); ++i)
    if (boxes_[i].id == id) return static_cast<int32_t>(i);
  return -1;
}

void MonitorMap::select(uint32_t id) {
  UiGuard lock(this);
  if (id != kNone && index_of(id) < 0) return;
  if (id == selected_) return;
  selected_ = id;
  invalidate();
  fire(selection_changed_);
}

// The picture is the monitors' bounds with half a monitor of room on every side, fitted to the control.
MonitorMap::View MonitorMap::compute_view() const {
  View v;
  if (boxes_.count() == 0) return v;
  int32_t l = boxes_[0].rect.x, t = boxes_[0].rect.y, r = boxes_[0].rect.right(), b = boxes_[0].rect.bottom();
  int32_t max_w = 0, max_h = 0;
  for (uint64_t i = 0; i < boxes_.count(); ++i) {
    const hardware::MonitorRect& m = boxes_[i].rect;
    if (m.x < l) l = m.x;
    if (m.y < t) t = m.y;
    if (m.right() > r) r = m.right();
    if (m.bottom() > b) b = m.bottom();
    if (static_cast<int32_t>(m.width) > max_w) max_w = static_cast<int32_t>(m.width);
    if (static_cast<int32_t>(m.height) > max_h) max_h = static_cast<int32_t>(m.height);
  }
  const int32_t pad_x = max_w / 2, pad_y = max_h / 2;
  v.base_x = l - pad_x;
  v.base_y = t - pad_y;
  const int64_t world_w = int64_t{r - l} + 2 * pad_x, world_h = int64_t{b - t} + 2 * pad_y;
  const int64_t view_w = int64_t{bounds_.width} - 2 * kMargin, view_h = int64_t{bounds_.height} - 2 * kMargin;
  if (view_w <= 0 || view_h <= 0 || world_w <= 0 || world_h <= 0) return v;
  int64_t sx = view_w * 65536 / world_w, sy = view_h * 65536 / world_h;
  v.scale = sx < sy ? sx : sy;
  if (v.scale > 65536 / 3) v.scale = 65536 / 3;   // (a small desktop is not blown up)
  if (v.scale < 1) v.scale = 1;
  v.origin_x = kMargin + static_cast<int32_t>((view_w - (world_w * v.scale >> 16)) / 2);
  v.origin_y = kMargin + static_cast<int32_t>((view_h - (world_h * v.scale >> 16)) / 2);
  return v;
}

Rect2D MonitorMap::screen_rect(const View& v, const hardware::MonitorRect& r) const {
  const int32_t x = v.origin_x + static_cast<int32_t>((int64_t{r.x - v.base_x} * v.scale) >> 16);
  const int32_t y = v.origin_y + static_cast<int32_t>((int64_t{r.y - v.base_y} * v.scale) >> 16);
  const int32_t x1 = v.origin_x + static_cast<int32_t>((int64_t{r.right() - v.base_x} * v.scale) >> 16);
  const int32_t y1 = v.origin_y + static_cast<int32_t>((int64_t{r.bottom() - v.base_y} * v.scale) >> 16);
  const int32_t w = x1 - x < 12 ? 12 : x1 - x, h = y1 - y < 12 ? 12 : y1 - y;
  return {x, y, static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
}

bool MonitorMap::box_rect(uint32_t id, Rect2D* out) const {
  const int32_t i = index_of(id);
  if (i < 0) return false;
  *out = screen_rect(dragging_ == kNone ? compute_view() : frozen_, boxes_[static_cast<uint64_t>(i)].rect);
  return true;
}

int32_t MonitorMap::box_at(int32_t x, int32_t y, const View& view) const {
  for (uint64_t i = boxes_.count(); i > 0; --i)
    if (contains(screen_rect(view, boxes_[i - 1].rect), x, y)) return static_cast<int32_t>(i - 1);
  return -1;
}

void MonitorMap::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.shadow);
  painter.bevel(all, true);
  const View view = dragging_ == kNone ? compute_view() : frozen_;
  painter.push_frame({2, 2, bounds_.width > 4 ? bounds_.width - 4 : 0, bounds_.height > 4 ? bounds_.height - 4 : 0});
  for (int pass = 0; pass < 2; ++pass) {   // (the monitor being dragged is drawn last, on top, at the pointer)
    for (uint64_t i = 0; i < boxes_.count(); ++i) {
      const Box& b = boxes_[i];
      const bool drag = b.id == dragging_;
      if (drag != (pass == 1)) continue;
      Rect2D r = screen_rect(view, b.rect);
      if (drag) {
        r.x = pointer_x_ - grab_dx_ - 2;
        r.y = pointer_y_ - grab_dy_ - 2;
      }
      const bool selected = b.id == selected_;
      const Rect2D box{r.x - 2, r.y - 2, r.width, r.height};
      (void)box;
      painter.fill({r.x - 2, r.y - 2, r.width, r.height}, selected ? t.selection : t.face);
      painter.bevel({r.x - 2, r.y - 2, r.width, r.height}, false);
      if (b.primary) painter.fill({r.x, r.y - 2 + static_cast<int32_t>(r.height) - 7, r.width > 4 ? r.width - 4 : 0, 4}, selected ? t.selection_text : t.dark);   // the taskbar
      const uint32_t ink = selected ? t.selection_text : t.text;
      String label(allocator());
      label.append_uint(b.id + 1);
      painter.text(r.x - 2 + (static_cast<int32_t>(r.width) - Painter::text_width(label.view())) / 2, r.y - 2 + (static_cast<int32_t>(r.height) - Painter::kGlyphHeight) / 2 - 6, label.view(), ink);
      if (r.width > 48 && r.height > 40) {   // the name, as much as fits
        painter.push_frame({r.x, r.y - 2 + static_cast<int32_t>(r.height) - 24, r.width > 4 ? r.width - 4 : 0, 16});
        const int32_t w = Painter::text_width(b.name.view());
        painter.text(w < static_cast<int32_t>(r.width) - 4 ? (static_cast<int32_t>(r.width) - 4 - w) / 2 : 0, 0, b.name.view(), ink);
        painter.pop_frame();
      }
      if (selected && focused()) painter.focus_frame({r.x, r.y, r.width > 4 ? r.width - 4 : 0, r.height > 4 ? r.height - 4 : 0});
    }
  }
  painter.pop_frame();
}

void MonitorMap::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  const View view = compute_view();
  const int32_t i = box_at(args.x, args.y, view);
  if (i < 0) return;
  pressed_ = boxes_[static_cast<uint64_t>(i)].id;
  press_x_ = args.x;
  press_y_ = args.y;
  frozen_ = view;
  const Rect2D r = screen_rect(view, boxes_[static_cast<uint64_t>(i)].rect);
  grab_dx_ = args.x - r.x;
  grab_dy_ = args.y - r.y;
  select(pressed_);
}

void MonitorMap::on_mouse_move(const MouseArgs& args) {
  if (pressed_ == kNone) return;
  pointer_x_ = args.x;
  pointer_y_ = args.y;
  if (dragging_ == kNone) {
    const int32_t dx = args.x - press_x_, dy = args.y - press_y_;
    if (dx * dx + dy * dy < 5 * 5) return;
    dragging_ = pressed_;
  }
  invalidate();
}

void MonitorMap::on_mouse_up(const MouseArgs& args) {
  const uint32_t dragged = dragging_;
  const View view = frozen_;
  pressed_ = dragging_ = kNone;
  invalidate();
  if (dragged == kNone || args.button != MouseButton::Left) return;
  // Where the dragged rectangle's top left was, on the desktop.
  moved_id_ = dragged;
  moved_x_ = view.base_x + static_cast<int32_t>(((int64_t{args.x - grab_dx_ - view.origin_x}) << 16) / view.scale);
  moved_y_ = view.base_y + static_cast<int32_t>(((int64_t{args.y - grab_dy_ - view.origin_y}) << 16) / view.scale);
  fire(moved_);
}

bool MonitorMap::on_key(const KeyArgs& args) {
  using namespace hardware;
  if (boxes_.count() == 0) return false;
  const int32_t at = selected_ == kNone ? -1 : index_of(selected_);
  if (args.usage == key::kRight) select(boxes_[static_cast<uint64_t>(at + 1 < static_cast<int32_t>(boxes_.count()) ? at + 1 : at < 0 ? 0 : at)].id);
  else if (args.usage == key::kLeft) select(boxes_[static_cast<uint64_t>(at > 0 ? at - 1 : 0)].id);
  else return false;
  return true;
}

}  // namespace desktop
