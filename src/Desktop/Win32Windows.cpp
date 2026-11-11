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

gui::CursorShape Win32WindowView::cursor_shape(int32_t, int32_t) const { return owner_.pointer_shape(); }
bool Win32WindowView::cursor_image(graphics::Surface* image, gui::Point* hot) const { return owner_.pointer_image(image, hot); }

Win32Window::Win32Window(allocators::IAllocator& allocator, IWindowsSession& session, uint32_t window, const Win32Windows& owner)
    : Frame(allocator), session_(session), window_(window), view_(allocator, session, window, owner) {
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
    drag_ = nullptr;
  }
  session_ = session;
  manager_ = manager;
  attached_ = display_sent_ = false;
  grabbed_ = released_ = false;
  buttons_ = 0;
}

namespace {

// The desktop's pointer for a system cursor (OCR_ id).
gui::CursorShape shape_of(uint32_t system_id) {
  switch (system_id) {
    case 32513: return gui::CursorShape::IBeam;
    case 32514: case 32650: return gui::CursorShape::Wait;      // (OCR_WAIT, OCR_APPSTARTING)
    case 32515: return gui::CursorShape::Crosshair;
    case 32642: return gui::CursorShape::ResizeDiagonalDown;     // OCR_SIZENWSE
    case 32643: return gui::CursorShape::ResizeDiagonalUp;       // OCR_SIZENESW
    case 32644: return gui::CursorShape::ResizeHorizontal;
    case 32645: return gui::CursorShape::ResizeVertical;
    case 32646: return gui::CursorShape::Move;
    case 32648: return gui::CursorShape::No;
    case 32649: return gui::CursorShape::Hand;
    default: return gui::CursorShape::Arrow;
  }
}

}  // namespace

bool Win32Windows::pointer_image(graphics::Surface* image, gui::Point* hot) const {
  if (pointer_shape_ != gui::CursorShape::Image || pointer_pixels_.count() < uint64_t{pointer_w_} * pointer_h_) return false;
  image->pixels = pointer_pixels_.view();
  image->width = image->pitch = pointer_w_;
  image->height = pointer_h_;
  *hot = pointer_hot_;
  return true;
}

void Win32Windows::follow_pointer(gui::WindowManager& manager) {
  IWindowsSession::Pointer p;
  session_->pointer(&p);
  gui::CursorShape shape = p.hidden ? gui::CursorShape::Hidden : shape_of(p.system_id);
  List<uint32_t> pixels(allocator_);
  uint32_t w = 0, h = 0;
  int32_t hx = 0, hy = 0;
  bool fresh_picture = false;
  if (!p.hidden && p.cursor != 0) {
    gui::UiGuard lock(&manager);
    if (p.cursor == pointer_cursor_ && pointer_shape_ != gui::CursorShape::Hidden) shape = pointer_shape_;      // (the picture is kept while the handle stays)
    else fresh_picture = true;
  }
  if (fresh_picture) {
    uint32_t system_id = 32512;
    if (session_->cursor_picture(p.cursor, &system_id, &pixels, &w, &h, &hx, &hy) && pixels.count() >= uint64_t{w} * h && w != 0) shape = gui::CursorShape::Image;
    else shape = shape_of(system_id);
  }
  gui::UiGuard lock(&manager);
  const bool changed = shape != pointer_shape_ || fresh_picture;
  if (fresh_picture) {
    pointer_cursor_ = p.cursor;
    pointer_pixels_.clear();
    for (uint32_t px : pixels.view()) pointer_pixels_.push_back(px);
    pointer_w_ = w;
    pointer_h_ = h;
    pointer_hot_ = {hx, hy};
  }
  pointer_shape_ = shape;
  if (changed) manager.invalidate();      // (the pointer is drawn with the frame)
}

void Win32Windows::set_monitors(ArrayView<const gui::Rect2D> regions, ArrayView<const gui::Rect2D> areas) {
  monitor_count_ = 0;
  for (uint64_t i = 0; i < regions.size() && i < areas.size() && monitor_count_ < 8; ++i) {
    regions_[monitor_count_] = regions[i];
    areas_[monitor_count_] = areas[i];
    ++monitor_count_;
  }
  display_sent_ = false;
}

Win32Window* Win32Windows::host(uint32_t window) const {
  for (const UniquePointer<Win32Window>& h : hosts_.view()) if (h->window_ == window) return h.get();
  return nullptr;
}

void Win32Windows::pump(gui::WindowManager& manager) {
  if (!session_) return;
  const bool first = !attached_;
  // the display: win32k's screen is the window manager's area, its monitors the desktop's (again whenever they change)
  IWindowsSession::Monitor monitors[8];
  uint32_t monitor_count = 0;
  gui::Rect2D bounds;
  bool send = false;
  {
    gui::UiGuard lock(&manager);
    bounds = manager.bounds();
    if (bounds.width == 0 || bounds.height == 0) return;
    send = !display_sent_ || !same(bounds, sent_bounds_);
    if (send) {
      const gui::Point o = manager.window_origin();
      if (monitor_count_ == 0) monitors[monitor_count++] = {{0, 0, bounds.width, bounds.height}, windows_rect(manager.area(0))};
      for (uint32_t i = 0; i < monitor_count_; ++i) {
        const gui::Rect2D r = regions_[i], a = areas_[i];      // (desktop window coordinates; the window manager's area are a shift of them)
        monitors[monitor_count++] = {{r.x - o.x, r.y - o.y, r.width, r.height}, {a.x - o.x, a.y - o.y, a.width, a.height}};
      }
      display_sent_ = true;
      sent_bounds_ = bounds;
    }
  }
  if (first) {
    session_->attach(gui::Frame::kBorder, gui::Frame::kTitleHeight + 1);
    attached_ = true;
  }
  if (send) session_->set_display(bounds.width, bounds.height, ArrayView<const IWindowsSession::Monitor>(monitors, monitor_count));
  session_->take_changes(&changed_, &repainted_);
  if (first) session_->windows(&changed_);
  uint32_t edge = 0;
  if (const uint32_t window = session_->take_drag(&edge)) {
    gui::UiGuard lock(&manager);
    Win32Window* h = host(window);
    if (h && h->manager() == &manager && h->state() == gui::Frame::State::Normal) {
      drag_ = h;
      drag_edge_ = edge;
      drag_x_ = pointer_x_;
      drag_y_ = pointer_y_;
      drag_from_ = h->bounds();
    }
  }      // (the windows shown before: all of them)
  {
    gui::UiGuard lock(&manager);
    follow_frames(manager);      // (first: what the user did since the last pump is asked of the programs before their older reports are applied)
  }
  String title(allocator_);
  for (uint32_t window : changed_.view()) {
    WindowsWindowInfo info;
    const bool exists = session_->window_info(window, &info, &title);
    // the icon's picture, when it is not the one the frame has (taken outside the UI lock: the session takes the kernel's)
    bool new_icon = false;
    List<uint32_t> icon(allocator_);
    uint32_t iw = 0, ih = 0;
    if (exists && info.visible) {
      uint32_t current = 0;
      {
        gui::UiGuard lock(&manager);
        const Win32Window* h = host(window);
        current = h ? h->icon_ : 0;
        new_icon = !h || current != info.icon;
      }
      int32_t hx = 0, hy = 0;
      uint32_t system_id = 0;
      if (new_icon && info.icon != 0 && !session_->cursor_picture(info.icon, &system_id, &icon, &iw, &ih, &hx, &hy)) icon.clear();
    }
    gui::UiGuard lock(&manager);
    if (!exists || !info.visible) remove(manager, window);
    else apply(manager, window, info, title);
    if (new_icon) {
      if (Win32Window* h = host(window)) {
        h->icon_ = info.icon;
        h->set_icon(icon.view(), iw, ih);      // (a system icon has no picture: none is shown)
      }
    }
  }
  if (hosts_.count() > 0) follow_pointer(manager);
  if (changed_.count() > 0) {
    session_->windows(&order_);
    gui::UiGuard lock(&manager);
    restack(manager);
  }
  gui::UiGuard lock(&manager);
  for (const IWindowsSession::Repaint& r : repainted_.view())
    if (Win32Window* h = host(r.window)) h->view_.invalidate();
}

// The programs' order among their frames: their top window goes directly above the frame of theirs that is highest on the desktop (where the user or the program
// last raised one), each next one directly below the one before it; the desktop's own windows keep their places relative to that frame.
void Win32Windows::restack(gui::WindowManager& manager) {
  List<Win32Window*> frames(allocator_);      // bottom first
  for (uint32_t window : order_.view()) {
    Win32Window* h = host(window);
    if (h && h->manager() == &manager) frames.push_back(h);
  }
  if (frames.count() < 2) return;
  Win32Window* highest = frames[0];
  for (Win32Window* f : frames.view()) if (manager.stacking_index(f) > manager.stacking_index(highest)) highest = f;
  Win32Window* previous = nullptr;
  for (uint64_t i = frames.count(); i > 0; --i) {
    Win32Window* f = frames[i - 1];
    if (!previous) {
      if (f != highest) manager.place_next_to(f, highest, true);
    } else if (manager.stacking_index(f) > manager.stacking_index(previous)) {
      manager.place_next_to(f, previous, false);
    }
    previous = f;
  }
}

void Win32Windows::remove(gui::WindowManager& manager, uint32_t window) {
  for (uint64_t i = 0; i < hosts_.count(); ++i) {
    if (hosts_[i]->window_ != window) continue;
    manager.remove_window(hosts_[i].get());
    if (last_active_ == hosts_[i].get()) last_active_ = manager.active();
    if (drag_ == hosts_[i].get()) drag_ = nullptr;
    hosts_.remove_at(i);
    return;
  }
}

void Win32Windows::apply(gui::WindowManager& manager, uint32_t window, const WindowsWindowInfo& info, const String& title) {
  using State = gui::Frame::State;
  Win32Window* h = host(window);
  const bool fresh = h == nullptr;
  if (fresh) {
    UniquePointer<Win32Window> made = UniquePointer<Win32Window>::make(allocator_, allocator_, *session_, window, *this);
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

void Win32Windows::drag_to(int32_t x, int32_t y) {
  constexpr int32_t kMin = 32;
  const int32_t dx = x - drag_x_, dy = y - drag_y_;
  int32_t left = drag_from_.x, top = drag_from_.y, right = left + static_cast<int32_t>(drag_from_.width), bottom = top + static_cast<int32_t>(drag_from_.height);
  if (drag_edge_ == 9) {
    left += dx, right += dx, top += dy, bottom += dy;
  } else {
    const uint32_t e = drag_edge_;
    if (e == 1 || e == 4 || e == 7) left = left + dx < right - kMin ? left + dx : right - kMin;
    if (e == 2 || e == 5 || e == 8) right = right + dx > left + kMin ? right + dx : left + kMin;
    if (e == 3 || e == 4 || e == 5) top = top + dy < bottom - kMin ? top + dy : bottom - kMin;
    if (e == 6 || e == 7 || e == 8) bottom = bottom + dy > top + kMin ? bottom + dy : top + kMin;
  }
  drag_->set_bounds({left, top, static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)});
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
  bool dragging = false, drag_done = false;
  {
    gui::UiGuard lock(manager_);
    pointer_x_ = *x;
    pointer_y_ = *y;
    if (drag_) {      // the frame follows the pointer; the program hears of it as the frame moves, not as mouse input
      dragging = true;
      drag_to(*x, *y);
      if (!(event.buttons & hardware::mouse_button::kLeft)) {
        drag_ = nullptr;
        drag_done = true;
      }
    }
    origin = manager_->window_origin();
    gui::Rect2D area;
    over = host_under(*manager_, *x - origin.x, *y - origin.y, &area);
    const gui::Frame* active = manager_->active();
    for (const UniquePointer<Win32Window>& h : hosts_.view()) if (h.get() == active) program_active = true;
  }
  if (dragging) {
    if (drag_done && (buttons_ & hardware::mouse_button::kLeft)) {      // (the press that began it is let go in the program too)
      session_->mouse_button(1, false);
      buttons_ = static_cast<uint8_t>(buttons_ & ~hardware::mouse_button::kLeft);
    }
    return;
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
      session_->mouse_left();
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
