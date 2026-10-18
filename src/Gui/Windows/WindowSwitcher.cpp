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

#include "Gui/Windows/WindowSwitcher.hpp"

namespace gui {

WindowSwitcher::WindowSwitcher(allocators::IAllocator& allocator, WindowManager& manager) : Control(allocator), manager_(manager), menu_(allocator) {
  restore_item_ = menu_.add_item("Restore", {&WindowSwitcher::run_restore, this});
  minimize_item_ = menu_.add_item("Minimize", {&WindowSwitcher::run_minimize, this});
  maximize_item_ = menu_.add_item("Maximize", {&WindowSwitcher::run_maximize, this});
  menu_.add_separator();
  close_item_ = menu_.add_item("Close", {&WindowSwitcher::run_close, this});
  menu_.on_opening(Handler::immediate(&WindowSwitcher::opening, this));
  set_context_menu(&menu_);
}

// The menu belongs to the window whose button was clicked (the active window when it comes from the keyboard).
void WindowSwitcher::prepare_context_menu(int32_t x, int32_t y) {
  const int32_t button = x < 0 ? -1 : button_at(x, y);
  menu_frame_ = button >= 0 ? frame_at(static_cast<uint32_t>(button)) : manager_.active();
}

void WindowSwitcher::opening(void* self, Control&) {
  auto* s = static_cast<WindowSwitcher*>(self);
  Frame* f = s->menu_frame_;
  const bool have = f != nullptr;
  s->restore_item_->set_enabled(have && f->state() != Frame::State::Normal);
  s->minimize_item_->set_enabled(have && f->minimize_enabled() && f->state() != Frame::State::Minimized);
  s->maximize_item_->set_enabled(have && f->maximize_enabled() && f->state() != Frame::State::Maximized);
  s->close_item_->set_enabled(have && f->close_enabled());
}

void WindowSwitcher::run_restore(void* self, Control&) {
  if (Frame* f = static_cast<WindowSwitcher*>(self)->menu_frame_) {
    f->restore();
    f->activate();
  }
}
void WindowSwitcher::run_minimize(void* self, Control&) {
  if (Frame* f = static_cast<WindowSwitcher*>(self)->menu_frame_) f->minimize();
}
void WindowSwitcher::run_maximize(void* self, Control&) {
  if (Frame* f = static_cast<WindowSwitcher*>(self)->menu_frame_) {
    f->maximize();
    f->activate();
  }
}
void WindowSwitcher::run_close(void* self, Control&) {
  if (Frame* f = static_cast<WindowSwitcher*>(self)->menu_frame_) f->close();
}

uint32_t WindowSwitcher::button_count() const {
  uint32_t n = 0;
  for (uint32_t i = 0; i < manager_.window_count(); ++i) n += !manager_.window_at(i)->modal();
  return n;
}

Frame* WindowSwitcher::frame_at(uint32_t button) const {
  uint32_t n = 0;
  for (uint32_t i = 0; i < manager_.window_count(); ++i) {
    Frame* f = manager_.window_at(i);
    if (f->modal()) continue;
    if (n++ == button) return f;
  }
  return nullptr;
}

Rect2D WindowSwitcher::button_rect(uint32_t button) const {
  const int32_t n = static_cast<int32_t>(button_count());
  if (n == 0) return {};
  int32_t width = (static_cast<int32_t>(bounds_.width) - (n - 1) * kGap) / n;
  if (width > kMaxButtonWidth) width = kMaxButtonWidth;
  if (width < kMinButtonWidth) width = kMinButtonWidth;
  return {static_cast<int32_t>(button) * (width + kGap), 0, static_cast<uint32_t>(width), bounds_.height};
}

int32_t WindowSwitcher::button_at(int32_t x, int32_t y) const {
  for (uint32_t i = 0; i < button_count(); ++i)
    if (contains(button_rect(i), x, y)) return static_cast<int32_t>(i);
  return -1;
}

void WindowSwitcher::paint(Painter& painter) {
  const Theme& t = painter.theme();
  for (uint32_t i = 0; i < button_count(); ++i) {
    const Frame* f = frame_at(i);
    const Rect2D r = button_rect(i);
    const bool shown = f == manager_.active() && f->state() != Frame::State::Minimized;
    const bool down = shown || pressed_ == static_cast<int32_t>(i);
    painter.fill(r, shown ? t.light : t.face);
    painter.bevel(r, down);
    painter.push_frame({r.x + 3, r.y + 3, r.width > 6 ? r.width - 6 : 0, r.height > 6 ? r.height - 6 : 0});
    painter.text(down ? 3 : 2, (static_cast<int32_t>(r.height) - 6 - Painter::kGlyphHeight) / 2 + (down ? 1 : 0), f->title(),
                 f->state() == Frame::State::Minimized ? t.shadow : t.text);
    painter.pop_frame();
  }
}

void WindowSwitcher::on_mouse_down(const MouseArgs& args) {
  if (args.button != MouseButton::Left || manager_.has_modal()) return;
  pressed_ = button_at(args.x, args.y);
  invalidate();
}

void WindowSwitcher::on_mouse_up(const MouseArgs& args) {
  const int32_t hit = button_at(args.x, args.y);
  const int32_t was = pressed_;
  pressed_ = -1;
  invalidate();
  if (args.button != MouseButton::Left || hit < 0 || hit != was || manager_.has_modal()) return;
  Frame* f = frame_at(static_cast<uint32_t>(hit));
  if (!f) return;
  if (f == manager_.active() && f->state() != Frame::State::Minimized) manager_.minimize(f);
  else manager_.activate(f);
}

void WindowSwitcher::on_mouse_wheel(const MouseArgs& args) {
  if (args.wheel == 0 || manager_.has_modal()) return;
  const int32_t count = static_cast<int32_t>(button_count());
  if (count == 0) return;
  int32_t current = -1;   // the button of the window in front (none when it is minimized or nothing is active)
  for (int32_t i = 0; i < count; ++i)
    if (frame_at(static_cast<uint32_t>(i)) == manager_.active()) current = i;
  if (current >= 0 && manager_.active()->state() == Frame::State::Minimized) current = -1;
  int32_t target;
  if (current < 0) target = args.wheel < 0 ? 0 : count - 1;
  else target = current + (args.wheel < 0 ? 1 : -1);
  if (target < 0 || target >= count) return;
  if (Frame* f = frame_at(static_cast<uint32_t>(target))) manager_.activate(f);
}

}  // namespace gui
