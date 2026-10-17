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

#include "Gui/Controls/BreadcrumbBar.hpp"
#include "Gui/Window.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/Move.hpp"

namespace gui {

bool BreadcrumbBar::EditBox::on_key(const KeyArgs& args) {
  if (args.usage == hardware::key::kEnter) {
    owner_.commit_edit();
    return true;
  }
  if (args.usage == hardware::key::kEscape) {
    owner_.cancel_edit();
    return true;
  }
  return TextBox::on_key(args);
}

void BreadcrumbBar::EditBox::on_focus_changed(bool focused) {
  if (!focused) owner_.cancel_edit();
}

BreadcrumbBar::BreadcrumbBar(allocators::IAllocator& allocator)
    : Control(allocator), path_(allocator), requested_(allocator), ends_(allocator), segments_(allocator), edit_(allocator, *this) {
  path_.assign("/");
  edit_.set_visible(false);
  add(&edit_);
  rebuild();
}

// "/a/b" from "a//b/ " and the like; an empty result means the root "/".
bool BreadcrumbBar::normalize(StringView in, String* out) {
  out->clear();
  uint64_t from = 0, to = in.size();
  while (from < to && in[from] == ' ') ++from;
  while (to > from && in[to - 1] == ' ') --to;
  StringView rest = in.substr(from, to - from);
  bool any = false;
  while (!rest.empty()) {
    uint64_t slash = rest.find('/');
    const uint64_t length = slash == StringView::npos ? rest.size() : slash;
    if (length > 0) {
      if (!out->push_back('/') || !out->append(rest.first(length))) return false;
      any = true;
    }
    rest = slash == StringView::npos ? StringView() : rest.without_prefix(slash + 1);
  }
  return any || out->assign("/");
}

void BreadcrumbBar::set_path(StringView path) {
  UiGuard lock(this);
  String next(allocator());
  if (!normalize(path, &next)) return;
  path_ = util::move(next);
  hover_ = -1;
  rebuild();
  invalidate();
}

StringView BreadcrumbBar::segment_label(uint32_t index) const {
  if (index >= segments_.count()) return StringView();
  const Segment& s = segments_[index];
  if (s.ellipsis) return "...";
  return s.label_length ? path_.view().substr(s.label_start, s.label_length) : StringView("/");
}

// Lays the folders out left to right; if they do not all fit the first ones give way to an ellipsis.
void BreadcrumbBar::rebuild() {
  ends_.clear();
  const StringView p = path_.view();
  for (uint32_t i = 1; p.size() > 1 && i <= p.size(); ++i)
    if (i == p.size() || p[i] == '/') ends_.push_back(i);

  struct Entry {
    uint32_t path_length, label_start, label_length;
  };
  // Entry 0 is the root, then one per folder.
  const uint32_t folders = static_cast<uint32_t>(ends_.count());
  auto entry = [&](uint32_t i) {
    if (i == 0) return Entry{0, 0, 0};
    const uint32_t end = ends_[i - 1], start = i == 1 ? 1 : ends_[i - 2] + 1;
    return Entry{end, start, end - start};
  };
  auto width_of = [&](uint32_t i) {
    const Entry e = entry(i);
    return (e.label_length ? static_cast<int32_t>(e.label_length) * Painter::kGlyphWidth : Painter::kGlyphWidth) + 2 * kPad;
  };
  const int32_t room = static_cast<int32_t>(bounds_.width) - 2 * kPad;
  // If everything fits, show it all; otherwise keep as many folders from the end as fit beside the ellipsis.
  uint32_t first = 0;
  int32_t total = 0;
  for (uint32_t i = 0; i <= folders; ++i) total += width_of(i) + kSeparatorWidth;
  if (total > room) {
    int32_t used = kEllipsisWidth + kSeparatorWidth;
    first = folders;  // the last folder is always shown
    for (uint32_t i = folders + 1; i-- > 0;) {
      used += width_of(i) + kSeparatorWidth;
      if (i < folders && used > room) break;
      first = i;
    }
    if (first == 0) first = 1;  // with the ellipsis there is no room left for the root
  }
  segments_.clear();
  int32_t x = kPad;
  if (first > 0) {  // the ellipsis stands for everything before `first`; it goes to the folder just before it
    Segment s;
    s.x = x;
    s.width = kEllipsisWidth;
    s.path_length = entry(first - 1).path_length;
    s.ellipsis = true;
    segments_.push_back(s);
    x += kEllipsisWidth + kSeparatorWidth;
  }
  for (uint32_t i = first; i <= folders; ++i) {
    const Entry e = entry(i);
    Segment s;
    s.x = x;
    s.width = width_of(i);
    s.path_length = e.path_length;
    s.label_start = e.label_start;
    s.label_length = e.label_length;
    segments_.push_back(s);
    x += s.width + kSeparatorWidth;
  }
}

void BreadcrumbBar::layout() {
  rebuild();
  const int32_t w = static_cast<int32_t>(bounds_.width), h = static_cast<int32_t>(bounds_.height);
  edit_.set_bounds({2, 2, static_cast<uint32_t>(w > 4 ? w - 4 : 0), static_cast<uint32_t>(h > 4 ? h - 4 : 0)});
  Control::layout();
}

void BreadcrumbBar::paint(Painter& painter) {
  const Theme& t = painter.theme();
  const Rect2D all{0, 0, bounds_.width, bounds_.height};
  painter.fill(all, t.field);
  painter.bevel(all, true);
  if (editing_) return;  // the text box covers the inside
  const uint32_t color = enabled() ? t.text : t.disabled_text;
  const int32_t h = static_cast<int32_t>(bounds_.height);
  painter.push_frame({2, 2, bounds_.width > 4 ? bounds_.width - 4 : 0, bounds_.height > 4 ? bounds_.height - 4 : 0});
  for (uint32_t i = 0; i < segments_.count(); ++i) {
    const Segment& s = segments_[i];
    const int32_t x = s.x - 2, top = 1, height = h - 6;
    if (static_cast<int32_t>(i) == hover_) {
      painter.fill({x, top, static_cast<uint32_t>(s.width), static_cast<uint32_t>(height > 0 ? height : 0)}, t.light);
      painter.outline({x, top, static_cast<uint32_t>(s.width), static_cast<uint32_t>(height > 0 ? height : 0)}, t.shadow);
    }
    painter.text(x + kPad, (height - Painter::kGlyphHeight) / 2 + top, segment_label(i), color);
    if (i + 1 < segments_.count()) {  // the chevron between this segment and the next
      const int32_t cx = x + s.width + kSeparatorWidth / 2, cy = top + height / 2;
      painter.line(cx - 2, cy - 4, cx + 1, cy, color);
      painter.line(cx + 1, cy, cx - 2, cy + 4, color);
    }
  }
  painter.pop_frame();
}

int32_t BreadcrumbBar::segment_at(int32_t x, int32_t y) const {
  if (y < 2 || y >= static_cast<int32_t>(bounds_.height) - 2) return -1;
  for (uint32_t i = 0; i < segments_.count(); ++i)
    if (x >= segments_[i].x && x < segments_[i].x + segments_[i].width) return static_cast<int32_t>(i);
  return -1;
}

void BreadcrumbBar::on_mouse_move(const MouseArgs& args) {
  const int32_t hit = editing_ ? -1 : segment_at(args.x, args.y);
  if (hit != hover_) {
    hover_ = hit;
    invalidate();
  }
}

void BreadcrumbBar::on_mouse_leave() {
  if (hover_ != -1) {
    hover_ = -1;
    invalidate();
  }
}

void BreadcrumbBar::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left || editing_) return;
  const int32_t hit = segment_at(args.x, args.y);
  if (hit < 0) {
    begin_edit();
    return;
  }
  const Segment& s = segments_[static_cast<uint32_t>(hit)];
  request(s.path_length ? path_.view().first(s.path_length) : StringView("/"));
}

void BreadcrumbBar::request(StringView path) {
  String next(allocator());
  if (!normalize(path, &next)) return;
  requested_ = util::move(next);
  fire(navigate_);
}

void BreadcrumbBar::begin_edit() {
  UiGuard lock(this);
  if (editing_) return;
  editing_ = true;
  hover_ = -1;
  edit_.set_text(path_.view());
  edit_.set_visible(true);
  if (Window* w = window()) w->set_focus(&edit_);
  invalidate();
}

void BreadcrumbBar::cancel_edit() {
  UiGuard lock(this);
  if (!editing_) return;
  editing_ = false;
  edit_.set_visible(false);
  invalidate();
}

void BreadcrumbBar::commit_edit() {
  UiGuard lock(this);
  if (!editing_) return;
  const String typed = [&] {
    String copy(allocator());
    copy.assign(edit_.text());
    return copy;
  }();
  cancel_edit();
  request(typed.view());
}

}  // namespace gui
