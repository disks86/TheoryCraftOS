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

#include "Gui/Controls/Accordion.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/Move.hpp"

namespace gui {

int32_t Accordion::add_section(StringView title, Control* body, int32_t body_height) {
  UiGuard lock(this);
  if (!body) return -1;
  if (body_height <= 0) body_height = body->preferred_size().height > 0 ? body->preferred_size().height : kDefaultBodyHeight;
  String name(allocator());
  if (!name.assign(title) || !titles_.push_back(util::move(name))) return -1;
  const bool first = titles_.count() == 1;
  if (!bodies_.push_back(body) || !heights_.push_back(body_height) || !open_.push_back(first ? 1 : 0) || !add(body)) {
    // Undo whatever of the parallel lists was already extended.
    titles_.remove_at(titles_.count() - 1);
    if (bodies_.count() > titles_.count()) bodies_.remove_at(bodies_.count() - 1);
    if (heights_.count() > titles_.count()) heights_.remove_at(heights_.count() - 1);
    if (open_.count() > titles_.count()) open_.remove_at(open_.count() - 1);
    return -1;
  }
  body->set_visible(first);
  if (first) focus_ = 0;
  invalidate();
  return static_cast<int32_t>(titles_.count()) - 1;
}

bool Accordion::expanded(int32_t index) const {
  const uint8_t* o = index >= 0 ? open_.get(static_cast<uint64_t>(index)) : nullptr;
  return o && *o;
}

int32_t Accordion::expanded_count() const {
  int32_t n = 0;
  for (const uint8_t o : open_.view()) n += o ? 1 : 0;
  return n;
}

// Sets one section's state and its body's visibility, without any bookkeeping about the others; records the index if it changed.
void Accordion::apply_open(int32_t index, bool expanded) {
  uint8_t* o = open_.get(static_cast<uint64_t>(index));
  if (!o || (*o != 0) == expanded) return;
  *o = expanded ? 1 : 0;
  bodies_[static_cast<uint64_t>(index)]->set_visible(expanded);
  last_changed_ = index;
}

void Accordion::set_expanded(int32_t index, bool expanded) {
  UiGuard lock(this);
  if (index < 0 || static_cast<uint64_t>(index) >= open_.count()) return;
  const int32_t before = last_changed_;
  last_changed_ = -1;
  if (expanded && exclusive_) {
    for (int32_t i = 0; i < section_count(); ++i) {
      if (i != index) apply_open(i, false);
    }
  }
  apply_open(index, expanded);
  if (last_changed_ < 0) {
    last_changed_ = before;
    return;
  }
  last_changed_ = index;
  invalidate();
  fire(changed_);
}

void Accordion::expand_all() {
  UiGuard lock(this);
  if (exclusive_) return;
  for (int32_t i = 0; i < section_count(); ++i) set_expanded(i, true);
}

void Accordion::collapse_all() {
  UiGuard lock(this);
  for (int32_t i = 0; i < section_count(); ++i) set_expanded(i, false);
}

void Accordion::set_exclusive(bool exclusive) {
  UiGuard lock(this);
  exclusive_ = exclusive;
  if (!exclusive) return;
  bool seen = false;
  for (int32_t i = 0; i < section_count(); ++i) {
    if (!expanded(i)) continue;
    if (seen) set_expanded(i, false);
    seen = true;
  }
}

Rect2D Accordion::header_rect(int32_t index) const {
  if (index < 0 || index >= section_count()) return {0, 0, 0, 0};
  int32_t y = 0;
  for (int32_t i = 0; i < index; ++i) y += kHeaderHeight + (expanded(i) ? heights_[static_cast<uint64_t>(i)] : 0);
  return {0, y, bounds_.width, static_cast<uint32_t>(kHeaderHeight)};
}

Rect2D Accordion::body_rect(int32_t index) const {
  if (!expanded(index)) return {0, 0, 0, 0};
  const Rect2D h = header_rect(index);
  const int32_t w = static_cast<int32_t>(bounds_.width) - 2 * kBorder;
  return {kBorder, h.y + kHeaderHeight, static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(heights_[static_cast<uint64_t>(index)])};
}

int32_t Accordion::header_at(int32_t x, int32_t y) const {
  for (int32_t i = 0; i < section_count(); ++i) {
    if (contains(header_rect(i), x, y)) return i;
  }
  return -1;
}

int32_t Accordion::content_height() const {
  int32_t total = 0;
  for (int32_t i = 0; i < section_count(); ++i) total += kHeaderHeight + (expanded(i) ? heights_[static_cast<uint64_t>(i)] : 0);
  return total;
}

// Runs on every frame (Window::render() lays out first): it only places, and changes no visibility.
void Accordion::layout() {
  for (int32_t i = 0; i < section_count(); ++i) {
    if (!expanded(i)) continue;
    Control* body = bodies_[static_cast<uint64_t>(i)];
    place(*body, body_rect(i));
    body->layout();
  }
}

void Accordion::paint(Painter& painter) {
  const Theme& t = painter.theme();
  painter.fill({0, 0, bounds_.width, bounds_.height}, t.face);
  const uint32_t ink = enabled() ? t.text : t.disabled_text;
  for (int32_t i = 0; i < section_count(); ++i) {
    const Rect2D h = header_rect(i);
    const bool open = expanded(i);
    painter.fill(h, t.face);
    painter.bevel(h, false);
    // The arrow: pointing down while open, right while closed.
    const int32_t cx = 12, cy = h.y + kHeaderHeight / 2;
    for (int32_t k = 0; k < 4; ++k) {
      if (open) painter.line(cx - 3 + k, cy - 2 + k, cx + 3 - k, cy - 2 + k, ink);
      else painter.line(cx - 2 + k, cy - 3 + k, cx - 2 + k, cy + 3 - k, ink);
    }
    painter.push_frame(h);
    painter.text(24, (kHeaderHeight - Painter::kGlyphHeight) / 2, section_title(i), ink);
    painter.pop_frame();
    if (focused() && i == focus_) painter.focus_frame({h.x + 3, h.y + 3, h.width > 6 ? h.width - 6 : 0, static_cast<uint32_t>(kHeaderHeight - 6)});
    if (open) {
      const Rect2D b = body_rect(i);
      painter.bevel({0, b.y, bounds_.width, b.height}, true);
    }
  }
}

void Accordion::move_focus(int32_t index) {
  if (index < 0 || index >= section_count() || index == focus_) return;
  focus_ = index;
  invalidate();
}

void Accordion::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left) return;
  const int32_t i = header_at(args.x, args.y);
  if (i < 0) return;
  move_focus(i);
  toggle(i);
}

bool Accordion::on_key(const KeyArgs& args) {
  using namespace hardware;
  const int32_t n = section_count();
  if (n == 0 || !focused()) return false;
  switch (args.usage) {
    case key::kDown: move_focus(focus_ + 1 < n ? focus_ + 1 : focus_); return true;
    case key::kUp: move_focus(focus_ > 0 ? focus_ - 1 : focus_); return true;
    case 0x4A: move_focus(0); return true;       // Home
    case 0x4D: move_focus(n - 1); return true;   // End
    case key::kEnter:
    case key::kSpace: toggle(focus_); return true;
    default: return false;
  }
}

}  // namespace gui
