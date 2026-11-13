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
#include "Gui/Window.hpp"
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

void Win32Window::on_drag_over(StringView text, int32_t x, int32_t y, const gui::Control*) {
  const gui::Rect2D b = bounds();      // (the window manager's coordinates are the programs' screen)
  drag_taken_ = session_.desktop_drag(window_, b.x + x, b.y + y, text);
}

void Win32Window::on_drag_leave() {
  drag_taken_ = false;
  session_.desktop_drag_end();
}

bool Win32Window::accepts_drop(StringView, int32_t, int32_t, const gui::Control*) const { return drag_taken_; }

void Win32Window::on_drop(StringView, int32_t, int32_t, const gui::Control*) {
  drag_taken_ = false;
  if (!session_.desktop_drop()) session_.desktop_drag_end();
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
    send = !display_sent_ || !same(bounds, sent_bounds_) || manager.reserve_serial() != sent_reserve_serial_;
    if (send) {
      const gui::Point o = manager.window_origin();
      if (monitor_count_ == 0) monitors[monitor_count++] = {{0, 0, bounds.width, bounds.height}, windows_rect(manager.area(0)), windows_rect(manager.full_area(0))};
      for (uint32_t i = 0; i < monitor_count_; ++i) {
        const gui::Rect2D r = regions_[i], a = areas_[i];      // (desktop window coordinates; the window manager's area are a shift of them)
        const gui::Rect2D full = {a.x - o.x, a.y - o.y, a.width, a.height};
        monitors[monitor_count++] = {{r.x - o.x, r.y - o.y, r.width, r.height}, windows_rect(manager.without_reserved(full)), windows_rect(full)};
      }
      display_sent_ = true;
      sent_bounds_ = bounds;
      sent_reserve_serial_ = manager.reserve_serial();
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
  int32_t menu_x = 0, menu_y = 0;
  if (const uint32_t window = session_->take_window_menu(&menu_x, &menu_y)) {      // (a program that draws its own title bar asks for the window menu)
    gui::UiGuard lock(&manager);
    Win32Window* h = host(window);
    if (h && h->manager() == &manager) {
      const gui::Rect2D client = h->view().bounds();
      h->show_window_menu(client.x + menu_x, client.y + menu_y);
    }
  }
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
  pump_program_drag(manager);
  pump_program_drop(manager);
  if (changed_.count() > 0) {
    session_->windows(&order_);
    gui::UiGuard lock(&manager);
    restack(manager);
  }
  gui::UiGuard lock(&manager);
  for (const IWindowsSession::Repaint& r : repainted_.view())
    if (Win32Window* h = host(r.window)) h->view_.invalidate();
}

// A program's drag let go over the desktop: its items are dropped where it was let go, one per pump (a drop waits for its control's handler thread before
// the next can go); the session hears whether any was taken once all have gone.
void Win32Windows::pump_program_drop(gui::WindowManager& manager) {
  if (!dropping_) {
    if (!session_->take_program_drop(&drop_items_, &drop_x_, &drop_y_)) return;
    dropping_ = true;
    drop_next_ = 0;
    drop_taken_ = false;
  }
  gui::Window* window = nullptr;
  gui::Point origin;
  {
    gui::UiGuard lock(&manager);
    window = manager.window();
    origin = manager.window_origin();
  }
  const StringView all = drop_items_.view();
  while (window && drop_next_ < all.size()) {
    if (window->drop_pending()) return;      // (the next pump goes on)
    uint64_t end = drop_next_;
    while (end < all.size() && all[end] != '\n') ++end;
    const StringView item = all.substr(drop_next_, end - drop_next_);
    drop_next_ = end + 1;
    if (!item.empty() && window->drop_at(origin.x + drop_x_, origin.y + drop_y_, item)) drop_taken_ = true;
  }
  dropping_ = false;
  drop_items_.clear();
  session_->program_drop_done(drop_taken_);
}

// A program's drag over the desktop, before it is let go: the desktop's window shows its first item at the pointer (with how many more there are) and says
// whether it would take that one, which the program hears.
void Win32Windows::pump_program_drag(gui::WindowManager& manager) {
  int32_t x = 0, y = 0;
  const bool over = !dropping_ && session_->program_drag(&drag_items_, &x, &y);
  gui::Window* window = nullptr;
  gui::Point origin;
  {
    gui::UiGuard lock(&manager);
    window = manager.window();
    origin = manager.window_origin();
  }
  if (!over || !window) {
    if (drag_window_) drag_window_->outside_drag_end();
    drag_window_ = nullptr;
    return;
  }
  const StringView all = drag_items_.view();
  uint64_t end = 0;
  while (end < all.size() && all[end] != '\n') ++end;
  const StringView first = all.first(end);
  uint32_t more = 0;
  for (uint64_t i = end; i < all.size(); ++i) if (all[i] == '\n') ++more;
  drag_label_.clear();
  if (more > 0) {
    drag_label_.append(first.substr(first.rfind('/') + 1));
    drag_label_.append(StringView(" (+"));
    drag_label_.append_uint(more);
    drag_label_.push_back(')');
  }
  if (drag_window_ && drag_window_ != window) drag_window_->outside_drag_end();
  drag_window_ = window;
  session_->program_drag_answer(window->outside_drag_at(origin.x + x, origin.y + y, first, drag_label_.view()));
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
  using Layer = gui::Frame::Layer;
  const int8_t layer = info.layer != 0 ? info.layer : info.topmost ? int8_t(1) : int8_t(0);
  const Layer want_layer = layer <= -2 ? Layer::Background : layer == -1 ? Layer::Bottom : layer == 0 ? Layer::Normal : layer == 1 ? Layer::Top : Layer::Overlay;
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
  h->set_layer(want_layer);
  h->set_reserve(info.reserve_edge <= 4 ? static_cast<gui::Frame::Edge>(info.reserve_edge) : gui::Frame::Edge::None, info.reserve);
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
  if (compose_key(event.usage, modifiers)) return;      // (taken by the input method: its release is not handed over either)
  slot = static_cast<uint8_t>(slot | bit);
  session_->key(event.usage, true, modifiers);
}

namespace {

// The value of a hex digit key (letters a-f, the digit row, the keypad), or -1.
int hex_of(uint8_t usage) {
  if (usage >= 0x04 && usage <= 0x09) return 10 + (usage - 0x04);
  if (usage >= 0x1E && usage <= 0x26) return 1 + (usage - 0x1E);
  if (usage == 0x27 || usage == 0x62) return 0;
  if (usage >= 0x59 && usage <= 0x61) return 1 + (usage - 0x59);
  return -1;
}

// `code` in UTF-8 into `out` (4 bytes); its length, 0 for no character (a surrogate, past U+10FFFF, or a control character below U+0020).
uint32_t utf8_of(uint32_t code, char (&out)[4]) {
  if (code < 0x20 || code == 0x7F || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF) return 0;
  if (code < 0x80) { out[0] = static_cast<char>(code); return 1; }
  if (code < 0x800) { out[0] = static_cast<char>(0xC0 | code >> 6); out[1] = static_cast<char>(0x80 | (code & 0x3F)); return 2; }
  if (code < 0x10000) {
    out[0] = static_cast<char>(0xE0 | code >> 12), out[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3F)), out[2] = static_cast<char>(0x80 | (code & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | code >> 18), out[1] = static_cast<char>(0x80 | ((code >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((code >> 6) & 0x3F)), out[3] = static_cast<char>(0x80 | (code & 0x3F));
  return 4;
}

}  // namespace

// Unicode entry, as IBus and GTK have it: Ctrl+Shift+U starts it (when the window with the keyboard takes composed text), hex digits (up to six) make the
// code point, shown by the program as "u1f600" being composed; Space or Enter puts the character in, Backspace takes a digit back, Escape (or any other
// key, which then goes on to the program) gives up. Modifier keys always go on, so the program's idea of them stays right.
bool Win32Windows::compose_key(uint8_t usage, uint8_t modifiers) {
  using namespace hardware::modifier;
  if (usage >= 0xE0 && usage <= 0xE7) return false;
  IWindowsSession::TextInputState state;
  if (!composing_) {
    const bool ctrl = modifiers & kControl, shift = modifiers & kShift, alt = modifiers & (kLeftAlt | kRightAlt);
    if (usage != 0x18 || !ctrl || !shift || alt) return false;      // (U)
    if (!session_->text_input(&state) || !state.enabled) return false;
    composing_ = true;
    compose_window_ = state.window;
    compose_code_ = 0;
    compose_digits_ = 0;
    if (!session_->text_preedit("u", 1)) composing_ = false;
    return composing_;
  }
  // the window with the keyboard changed, or stopped taking text: the composing is over and the key is the program's
  if (!session_->text_input(&state) || !state.enabled || state.window != compose_window_) {
    composing_ = false;
    return false;
  }
  auto show = [&]() {
    char text[8] = {'u'};
    uint32_t n = 1;
    for (int shift = 4 * (compose_digits_ - 1); shift >= 0; shift -= 4) text[n++] = "0123456789abcdef"[(compose_code_ >> shift) & 15];
    session_->text_preedit(StringView(text, n), n);
  };
  const int digit = hex_of(usage);
  if (digit >= 0 && !(modifiers & (kControl | kLeftAlt | kRightAlt))) {
    if (compose_digits_ < 6) {
      compose_code_ = compose_code_ * 16 + static_cast<uint32_t>(digit);
      ++compose_digits_;
    }
    show();
    return true;
  }
  if (usage == 0x2A) {      // Backspace
    if (compose_digits_ == 0) {
      composing_ = false;
      session_->text_preedit(StringView(), 0);
    } else {
      compose_code_ >>= 4;
      --compose_digits_;
      show();
    }
    return true;
  }
  if (usage == 0x2C || usage == 0x28 || usage == 0x58) {      // Space, Enter, keypad Enter
    composing_ = false;
    char utf8[4];
    const uint32_t n = compose_digits_ > 0 ? utf8_of(compose_code_, utf8) : 0;
    if (n > 0) session_->text_commit(StringView(utf8, n));
    else session_->text_preedit(StringView(), 0);
    return true;
  }
  composing_ = false;
  session_->text_preedit(StringView(), 0);
  return usage == 0x29;      // (Escape is taken; anything else goes on)
}

bool Win32Windows::tap_touch(const hardware::TouchEvent& event, int32_t x, int32_t y) {
  if (!session_ || !manager_ || !attached_) return false;
  gui::Point origin;
  Win32Window* over = nullptr;
  {
    gui::UiGuard lock(manager_);
    origin = manager_->window_origin();
    if (event.phase == hardware::TouchPhase::Down) {
      gui::Rect2D area;
      over = host_under(*manager_, x - origin.x, y - origin.y, &area);
    }
  }
  const int32_t sx = x - origin.x, sy = y - origin.y;      // (the programs' screen = the window manager's area)
  if (event.phase == hardware::TouchPhase::Down) {
    if (!over || !session_->touch(event.phase, event.id, over->window(), sx, sy)) return false;
    touches_.push_back(event.id);
    gui::UiGuard lock(manager_);
    if (host(over->window()) == over) over->activate();      // (a touch on a window brings it forward, as a click does)
    return true;
  }
  for (uint64_t i = 0; i < touches_.count(); ++i) {
    if (touches_[i] != event.id) continue;
    session_->touch(event.phase, event.id, 0, sx, sy);
    if (event.phase == hardware::TouchPhase::Up || event.phase == hardware::TouchPhase::Cancel) touches_.remove_at(i);
    return true;
  }
  return false;
}

bool Win32Windows::tap_pen(const hardware::PenEvent& event, int32_t x, int32_t y) {
  if (!session_ || !manager_ || !attached_) return false;
  gui::Point origin;
  Win32Window* over = nullptr;
  {
    gui::UiGuard lock(manager_);
    origin = manager_->window_origin();
    gui::Rect2D area;
    over = host_under(*manager_, x - origin.x, y - origin.y, &area);
  }
  if (!pen_taken_ && (!over || !event.in_range)) return false;
  PenInput p;
  p.in_range = event.in_range;
  p.tip = event.tip;
  p.inverted = event.inverted;
  p.buttons = event.buttons;
  p.window = over ? over->window() : 0;
  p.x = x - origin.x;
  p.y = y - origin.y;
  p.pressure = event.max_pressure ? static_cast<uint16_t>(uint64_t{event.pressure > event.max_pressure ? event.max_pressure : event.pressure} * 65535u / event.max_pressure)
                                  : (event.tip ? 65535 : 0);
  p.tilt_x = event.tilt_x;
  p.tilt_y = event.tilt_y;
  const bool was = pen_taken_;
  const bool taken = session_->pen(p);
  pen_taken_ = taken && event.in_range;
  if (taken && !was && event.tip && over) {      // (the tip touching a window brings it forward, as a click does)
    gui::UiGuard lock(manager_);
    if (host(over->window()) == over) over->activate();
  }
  return taken || was;
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
    // the desktop's own drag (a file from the File Explorer) is the desktop's: the programs hear of it through desktop_drag, never as their mouse
    if (const gui::Window* w = manager_->window(); w && w->dragging() && !grabbed_) return;
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
