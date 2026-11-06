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

#include "Desktop/Win32Windows.hpp"
#include "Gui/Painter.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"

namespace desktop {

namespace {

constexpr uint8_t kMouseButtons[3] = {hardware::mouse_button::kLeft, hardware::mouse_button::kRight, hardware::mouse_button::kMiddle};

bool same(const gui::Rect2D& a, const gui::Rect2D& b) { return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height; }
gui::Rect2D gui_rect(const WindowsRect& r) { return {r.x, r.y, r.width, r.height}; }
WindowsRect windows_rect(const gui::Rect2D& r) { return {r.x, r.y, r.width, r.height}; }

}  // namespace

// ------------------------------------------------------------------ one window

void Win32WindowView::paint(gui::Painter& painter) {
  uint32_t w = 0, h = 0;
  if (!session_.draw_window(window_, painter.renderer(), painter.origin_x(), painter.origin_y(), &w, &h)) w = h = 0;
  // what the surface does not cover yet (the frame was sized and the program has not caught up): black
  if (bounds_.width > w) painter.fill({static_cast<int32_t>(w), 0, bounds_.width - w, bounds_.height}, 0xFF000000u);
  if (bounds_.height > h) painter.fill({0, static_cast<int32_t>(h), w < bounds_.width ? w : bounds_.width, bounds_.height - h}, 0xFF000000u);
}

Win32Window::Win32Window(allocators::IAllocator& allocator, IWindowsSession& session, uint32_t window)
    : Frame(allocator), session_(session), window_(window), view_(allocator, session, window) {
  set_padding(gui::Insets::all(0));
  set_escape_closes(false);      // (Escape is the program's key)
  set_opaque_client(true);       // (the program's picture covers the client area)
  view_.set_dock(gui::DockStyle::Fill);
  add(&view_);
}

bool Win32Window::close_requested() {
  session_.window_command(window_, IWindowsSession::Command::Close);
  return false;
}

bool Win32Window::picture_area(gui::Rect2D* area) const {
  if (!manager() || state() == State::Minimized || !visible()) return false;
  const gui::Rect2D b = bounds(), v = view_.bounds();
  if (v.width == 0 || v.height == 0) return false;
  *area = {b.x + v.x, b.y + v.y, v.width, v.height};
  return true;
}

// ------------------------------------------------------------------ the windows

Win32Windows::~Win32Windows() { set_session(nullptr, manager_); }

void Win32Windows::set_session(IWindowsSession* session, gui::WindowManager* manager) {
  if (manager_ && hosts_.count() > 0) {
    gui::UiGuard lock(manager_);
    for (UniquePointer<Win32Window>& h : hosts_.view()) manager_->remove_window(h.get());
    hosts_.clear();
    last_active_ = nullptr;
  }
  session_ = session;
  manager_ = manager;
  attached_ = false;
  grabbed_ = released_ = false;
  buttons_ = 0;
}

Win32Window* Win32Windows::host(uint32_t window) const {
  for (const UniquePointer<Win32Window>& h : hosts_.view()) if (h->window_ == window) return h.get();
  return nullptr;
}

void Win32Windows::pump(gui::WindowManager& manager) {
  if (!session_) return;
  const bool first = !attached_;
  if (first) {      // win32k's screen becomes the window manager's area, before a program shows a window
    gui::Rect2D b, work;
    {
      gui::UiGuard lock(&manager);
      b = manager.bounds();
      work = manager.area(0);
    }
    if (b.width == 0 || b.height == 0) return;
    session_->attach(b.width, b.height, windows_rect(work), gui::Frame::kBorder, gui::Frame::kTitleHeight + 1);
    attached_ = true;
  }
  session_->take_changes(&changed_, &repainted_);
  if (first) session_->windows(&changed_);      // (the windows shown before: all of them)
  {
    gui::UiGuard lock(&manager);
    follow_frames(manager);      // (first: what the user did since the last pump is asked of the programs before their older reports are applied)
  }
  String title(allocator_);
  for (uint32_t window : changed_.view()) {
    WindowsWindowInfo info;
    const bool exists = session_->window_info(window, &info, &title);
    gui::UiGuard lock(&manager);
    if (!exists || !info.visible) remove(manager, window);
    else apply(manager, window, info, title);
  }
  gui::UiGuard lock(&manager);
  for (const IWindowsSession::Repaint& r : repainted_.view())
    if (Win32Window* h = host(r.window)) h->view_.invalidate();
}

void Win32Windows::remove(gui::WindowManager& manager, uint32_t window) {
  for (uint64_t i = 0; i < hosts_.count(); ++i) {
    if (hosts_[i]->window_ != window) continue;
    manager.remove_window(hosts_[i].get());
    if (last_active_ == hosts_[i].get()) last_active_ = manager.active();
    hosts_.remove_at(i);
    return;
  }
}

void Win32Windows::apply(gui::WindowManager& manager, uint32_t window, const WindowsWindowInfo& info, const String& title) {
  using State = gui::Frame::State;
  Win32Window* h = host(window);
  const bool fresh = h == nullptr;
  if (fresh) {
    UniquePointer<Win32Window> made = UniquePointer<Win32Window>::make(allocator_, allocator_, *session_, window);
    if (!made || !hosts_.push_back(util::move(made))) return;
    h = hosts_[hosts_.count() - 1].get();
  }
  h->set_title(title.view());
  h->set_decorated(info.caption);
  h->set_resizable(info.sizable);
  h->set_minimize_enabled(info.minimize_box);
  h->set_maximize_enabled(info.maximize_box);
  h->set_in_taskbar(!info.popup && !info.tool && !info.owned);
  const gui::Rect2D r = gui_rect(info.window);
  const State want = info.minimized ? State::Minimized : info.maximized ? State::Maximized : State::Normal;
  if (fresh) {
    h->set_bounds(r);
    h->set_activatable(false);      // (added without taking the activation: the program says whether it has it)
    if (!manager.add_window(h)) {
      hosts_.remove_at(hosts_.count() - 1);
      return;
    }
    h->known_ = h->sent_ = h->bounds();
  }
  h->set_activatable(!info.popup);
  // the state: a minimized frame comes back to what it was before (which may be maximized), then goes where the program is (unless the user just changed it and
  // the program has not caught up)
  if (h->state_hold_ > 0 && want != h->sent_state_) return;
  h->state_hold_ = 0;
  if (want != h->state()) {
    if (h->state() == State::Minimized && want != State::Minimized) manager.activate(h);
    if (want == State::Minimized) manager.minimize(h);
    else if (want == State::Maximized && h->state() != State::Maximized) manager.maximize(h);
    else if (want == State::Normal && h->state() != State::Normal) manager.restore(h);
  }
  h->known_state_ = h->sent_state_ = h->state();
  if (want == State::Normal && h->state() == State::Normal && (h->hold_ == 0 || same(r, h->sent_))) {      // (while the user moves the frame, an older rectangle of the program's does not move it back)
    if (!same(h->bounds(), r)) h->set_bounds(r);
    h->known_ = h->sent_ = r;
    h->hold_ = 0;
  }
  if (info.active && !info.popup && want != State::Minimized && manager.active() != h) manager.activate(h);
  if (info.active || fresh) last_active_ = manager.active();
}

// What the user did to the frames since the last pump goes to the programs (under the UI lock: the session takes the kernel's lock after it, the allowed order).
void Win32Windows::follow_frames(gui::WindowManager& manager) {
  gui::Frame* active = manager.active();
  if (active != last_active_) {
    Win32Window* now = nullptr;
    bool was_host = false;
    for (UniquePointer<Win32Window>& h : hosts_.view()) {
      if (h.get() == active) now = h.get();
      if (h.get() == last_active_) was_host = true;
    }
    if (now) session_->activate_window(now->window_);
    else if (was_host) session_->activate_window(0);      // a desktop window took the activation from a program's
    last_active_ = active;
  }
  for (UniquePointer<Win32Window>& p : hosts_.view()) {
    Win32Window& h = *p;
    if (h.hold_ > 0) --h.hold_;
    if (h.state_hold_ > 0) --h.state_hold_;
    const gui::Frame::State st = h.state();
    if (st != h.known_state_) {
      const IWindowsSession::Command c = st == gui::Frame::State::Minimized   ? IWindowsSession::Command::Minimize
                                         : st == gui::Frame::State::Maximized ? IWindowsSession::Command::Maximize
                                                                              : IWindowsSession::Command::Restore;
      session_->window_command(h.window_, c);
      h.known_state_ = h.sent_state_ = st;
      h.state_hold_ = kHoldPumps;
    }
    const gui::Rect2D b = h.bounds();
    if (st == gui::Frame::State::Normal && !same(b, h.known_) && !same(b, h.sent_)) {
      session_->move_window(h.window_, windows_rect(b));
      h.sent_ = b;
      h.hold_ = kHoldPumps;
    }
  }
}

// ------------------------------------------------------------------ input

Win32Window* Win32Windows::host_under(gui::WindowManager& manager, int32_t x, int32_t y, gui::Rect2D* area) const {
  for (uint32_t i = manager.child_count(); i > 0; --i) {
    const gui::Frame* f = static_cast<const gui::Frame*>(manager.child(i - 1));
    if (!f->visible() || f->state() == gui::Frame::State::Minimized || !gui::contains(f->bounds(), x, y)) continue;
    for (const UniquePointer<Win32Window>& h : hosts_.view())
      if (h.get() == f) return h->picture_area(area) && gui::contains(*area, x, y) ? h.get() : nullptr;
    return nullptr;      // a desktop window is on top there
  }
  return nullptr;
}

void Win32Windows::release_buttons() {
  for (uint32_t i = 0; i < 3; ++i)
    if (buttons_ & kMouseButtons[i]) session_->mouse_button(i + 1, false);      // (a button held when the pointer goes is let go in the programs too)
  buttons_ = 0;
}

void Win32Windows::tap_key(const hardware::KeyEvent& event, uint8_t modifiers) {
  if (!session_ || !manager_ || !attached_) return;
  const uint8_t bit = static_cast<uint8_t>(1u << (event.usage & 7));
  uint8_t& slot = keys_down_[event.usage >> 3];
  if (!event.pressed) {      // a release goes where the press went
    if (!(slot & bit)) return;
    slot = static_cast<uint8_t>(slot & ~bit);
    session_->key(event.usage, false, modifiers);
    return;
  }
  bool to_programs = false;
  {
    gui::UiGuard lock(manager_);
    const gui::Frame* active = manager_->active();
    for (const UniquePointer<Win32Window>& h : hosts_.view()) if (h.get() == active) to_programs = true;
  }
  if (!to_programs) return;
  if ((modifiers & hardware::modifier::kControl) && (modifiers & (hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt)) && grabbed_ &&
      session_->cursor_confined()) {      // Ctrl+Alt: a confined pointer is the desktop's again
    release_buttons();
    grabbed_ = false;
    released_ = true;
  }
  slot = static_cast<uint8_t>(slot | bit);
  session_->key(event.usage, true, modifiers);
}

void Win32Windows::tap_mouse(const hardware::MouseEvent& event, int32_t* x, int32_t* y) {
  if (!session_ || !manager_ || !attached_) return;
  gui::Point origin;
  Win32Window* over = nullptr;
  bool program_active = false;
  {
    gui::UiGuard lock(manager_);
    origin = manager_->window_origin();
    gui::Rect2D area;
    over = host_under(*manager_, *x - origin.x, *y - origin.y, &area);
    const gui::Frame* active = manager_->active();
    for (const UniquePointer<Win32Window>& h : hosts_.view()) if (h.get() == active) program_active = true;
  }
  const int32_t sx = *x - origin.x, sy = *y - origin.y;      // win32k's screen = the window manager's area
  const uint8_t pressed_now = static_cast<uint8_t>(event.buttons & ~buttons_ & 7);
  if (released_) {
    if (!over || !pressed_now) return;
    released_ = false;      // a click on a Windows window takes the pointer back
  }
  const bool held = grabbed_ && (buttons_ != 0 || (program_active && session_->cursor_confined()) || session_->capture_window() != 0);
  if (!over && !held) {
    if (grabbed_) {
      release_buttons();
      grabbed_ = false;      // the pointer left the programs' windows with nothing keeping it: it is the desktop's again
    }
    return;
  }
  if (!grabbed_ || event.absolute) session_->mouse_position(sx, sy);      // (entering: the cursor goes where the pointer came in)
  else if (event.dx != 0 || event.dy != 0) session_->mouse_motion(event.dx, event.dy);
  grabbed_ = true;
  const uint8_t changed = static_cast<uint8_t>((event.buttons ^ buttons_) & 7);
  for (uint32_t i = 0; i < 3; ++i)
    if (changed & kMouseButtons[i]) session_->mouse_button(i + 1, (event.buttons & kMouseButtons[i]) != 0);
  buttons_ = static_cast<uint8_t>(event.buttons & 7);
  if (event.wheel != 0) session_->mouse_wheel(event.wheel);
  // The pointer shows where the Windows cursor is (a program may have confined or moved it).
  int32_t cx = 0, cy = 0;
  session_->cursor(&cx, &cy);
  *x = origin.x + cx;
  *y = origin.y + cy;
}

}  // namespace desktop
