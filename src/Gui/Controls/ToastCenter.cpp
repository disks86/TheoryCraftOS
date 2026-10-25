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

#include "Gui/Controls/ToastCenter.hpp"

namespace gui {

namespace {

// Calls `each` with every line of `text` (without its '\n'); an empty text has none.
template <class F>
void for_each_line(StringView text, F&& each) {
  if (text.empty()) return;
  uint64_t start = 0;
  for (uint64_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '\n') {
      each(text.substr(start, i - start));
      start = i + 1;
    }
  }
}

}  // namespace

int32_t ToastCenter::height_of(const Toast& toast) {
  const int32_t rows = static_cast<int32_t>(toast.lines) + (toast.title.empty() ? 0 : 1);
  return rows * Painter::kGlyphHeight + 2 * kPad + 2;
}

uint32_t ToastCenter::color_of(NoticeKind kind) {
  switch (kind) {
    case NoticeKind::Warning: return kWarningColor;
    case NoticeKind::Error: return kErrorColor;
    default: return kInfoColor;
  }
}

int32_t ToastCenter::stack_height() const {
  int32_t total = 0;
  for (uint64_t i = 0; i < toasts_.count(); ++i) total += height_of(toasts_[i]) + (i ? kGap : 0);
  return total;
}

uint32_t ToastCenter::post(StringView title, StringView message, NoticeKind kind, uint32_t duration_ms, Handler on_click) {
  if (title.empty() && message.empty()) return 0;
  UiGuard lock(this);
  Toast toast(allocator());
  StringView lines[kMaxLines];
  String storage(allocator());
  const uint32_t line_count = NoticeDialog::wrap(message, lines, kMaxLines, &storage);
  for (uint32_t i = 0; i < line_count; ++i) {
    if (i && !toast.body.push_back('\n')) return 0;
    if (!toast.body.append(lines[i])) return 0;
  }
  const bool cut = title.size() > kTitleChars;
  if (!toast.title.assign(cut ? title.substr(0, kTitleChars - 3) : title)) return 0;
  if (cut && !toast.title.append("...")) return 0;
  toast.lines = line_count;
  toast.kind = kind;
  toast.duration_us = uint64_t{duration_ms} * 1000;
  toast.on_click = on_click;
  toast.id = next_id_;
  if (!toasts_.push_back(static_cast<Toast&&>(toast))) return 0;
  const uint32_t id = next_id_++;
  if (next_id_ == 0) next_id_ = 1;
  while (toasts_.count() > kMaxToasts) toasts_.remove_at(0);
  invalidate();
  return id;
}

bool ToastCenter::dismiss(uint32_t id) {
  UiGuard lock(this);
  for (uint64_t i = 0; i < toasts_.count(); ++i) {
    if (toasts_[i].id != id) continue;
    toasts_.remove_at(i);
    invalidate();
    return true;
  }
  return false;
}

void ToastCenter::clear() {
  UiGuard lock(this);
  if (toasts_.count() == 0) return;
  toasts_.clear();
  invalidate();
}

uint32_t ToastCenter::tick(uint64_t uptime_us) {
  UiGuard lock(this);
  uint32_t gone = 0;
  for (uint64_t i = 0; i < toasts_.count();) {
    Toast& t = toasts_[i];
    if (t.duration_us == 0) { ++i; continue; }   // stays until clicked
    if (t.deadline_us == 0) {
      t.deadline_us = uptime_us + t.duration_us;
      if (t.deadline_us == 0) t.deadline_us = 1;
    }
    if (uptime_us >= t.deadline_us) {
      toasts_.remove_at(i);
      ++gone;
    } else {
      ++i;
    }
  }
  if (gone) invalidate();
  return gone;
}

bool ToastCenter::showing(uint32_t id) const {
  for (uint64_t i = 0; i < toasts_.count(); ++i)
    if (toasts_[i].id == id) return true;
  return false;
}

StringView ToastCenter::title_of(uint32_t id) const {
  for (uint64_t i = 0; i < toasts_.count(); ++i)
    if (toasts_[i].id == id) return toasts_[i].title.view();
  return StringView();
}

StringView ToastCenter::message_of(uint32_t id) const {
  for (uint64_t i = 0; i < toasts_.count(); ++i)
    if (toasts_[i].id == id) return toasts_[i].body.view();
  return StringView();
}

NoticeKind ToastCenter::kind_of(uint32_t id) const {
  for (uint64_t i = 0; i < toasts_.count(); ++i)
    if (toasts_[i].id == id) return toasts_[i].kind;
  return NoticeKind::Information;
}

Rect2D ToastCenter::toast_bounds(uint32_t index) const {
  if (index >= toasts_.count()) return Rect2D{};
  int32_t y = bounds_.y;
  for (uint32_t i = 0; i < index; ++i) y += height_of(toasts_[i]) + kGap;
  return {bounds_.x, y, static_cast<uint32_t>(kWidth), static_cast<uint32_t>(height_of(toasts_[index]))};
}

void ToastCenter::layout() {
  const Control* p = parent();
  if (!p) return;
  Rect2D area = has_area_ ? area_ : Rect2D{0, 0, p->bounds().width, p->bounds().height};
  if (toasts_.count() == 0) {
    place(*this, Rect2D{0, 0, 0, 0});
    return;
  }
  int32_t w = kWidth, h = stack_height();
  const int32_t right = area.x + static_cast<int32_t>(area.width) - kMargin, bottom = area.y + static_cast<int32_t>(area.height) - kMargin;
  int32_t x = right - w, y = bottom - h;
  if (x < area.x) x = area.x;   // (a surface narrower or lower than a toast: the toast is cut, not moved off)
  if (y < area.y) y = area.y;
  place(*this, Rect2D{x, y, static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
}

void ToastCenter::paint(Painter& painter) {
  const Theme& t = painter.theme();
  int32_t y = 0;
  for (uint64_t i = 0; i < toasts_.count(); ++i) {
    const Toast& toast = toasts_[i];
    const int32_t h = height_of(toast);
    const Rect2D box{0, y, static_cast<uint32_t>(kWidth), static_cast<uint32_t>(h)};
    painter.fill(box, kBackground);
    painter.outline(box, t.dark);
    painter.fill({1, y + 1, static_cast<uint32_t>(kStripe), static_cast<uint32_t>(h - 2)}, color_of(toast.kind));
    const int32_t tx = 1 + kStripe + kPad;
    int32_t ty = y + 1 + kPad;
    if (!toast.title.empty()) {
      painter.text(tx, ty, toast.title.view(), t.text);
      painter.text(tx + 1, ty, toast.title.view(), t.text);   // (heavier: the title)
      painter.text(kWidth - kPad - 1 - Painter::kGlyphWidth, ty, "x", t.shadow);
      ty += Painter::kGlyphHeight;
    }
    for_each_line(toast.body.view(), [&](StringView line) {
      painter.text(tx, ty, line, t.text);
      ty += Painter::kGlyphHeight;
    });
    y += h + kGap;
  }
}

uint32_t ToastCenter::toast_at(int32_t y) const {
  int32_t top = 0;
  for (uint64_t i = 0; i < toasts_.count(); ++i) {
    const int32_t h = height_of(toasts_[i]);
    if (y >= top && y < top + h) return static_cast<uint32_t>(i);
    top += h + kGap;
  }
  return static_cast<uint32_t>(toasts_.count());
}

void ToastCenter::on_mouse_down(const MouseArgs& args) {
  UiGuard lock(this);
  const uint32_t index = toast_at(args.y);
  if (index >= toasts_.count()) return;
  const Toast& toast = toasts_[index];
  const bool close_box = args.x >= kWidth - kPad - 2 * Painter::kGlyphWidth && args.y < toast_bounds(index).y - bounds_.y + 1 + kPad + Painter::kGlyphHeight;
  const bool activate = args.button == MouseButton::Left && !close_box;
  const Handler handler = toast.on_click;
  toasts_.remove_at(index);
  invalidate();
  if (activate && handler) fire(handler);
}

}  // namespace gui
