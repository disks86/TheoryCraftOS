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

#include "Desktop/WindowsScreen.hpp"
#include "Gui/Painter.hpp"
#include "Hardware/Keyboard/KeyCodes.hpp"
#include "Utility/AllocatedView.hpp"

namespace desktop {

namespace {

constexpr uint8_t kMouseButtons[3] = {hardware::mouse_button::kLeft, hardware::mouse_button::kRight, hardware::mouse_button::kMiddle};

bool contains(const gui::Rect2D& r, int32_t x, int32_t y) {
  return x >= r.x && y >= r.y && x < r.x + static_cast<int32_t>(r.width) && y < r.y + static_cast<int32_t>(r.height);
}

}  // namespace

// ------------------------------------------------------------------ the picture

WindowsScreenView::~WindowsScreenView() { release_view(allocator(), pixels_); }

bool WindowsScreenView::set_picture(ArrayView<const uint32_t> pixels, uint32_t width, uint32_t height) {
  gui::UiGuard lock(this);
  const uint64_t count = uint64_t{width} * height;
  if (pixels.size() < count) return false;
  if (pixels_.size() != count) {
    release_view(allocator(), pixels_);
    width_ = height_ = 0;
    pixels_ = allocate_view<uint32_t>(allocator(), count);
    if (pixels_.size() != count) return false;
  }
  pixels_.copy_from(pixels.first(count));
  width_ = width;
  height_ = height;
  invalidate();
  return true;
}

void WindowsScreenView::paint(gui::Painter& painter) {
  painter.fill({0, 0, bounds_.width, bounds_.height}, 0xFF000000u);
  if (pixels_.empty()) return;
  graphics::Surface s;
  s.pixels = pixels_;
  s.width = s.pitch = width_;
  s.height = height_;
  painter.image(s, {0, 0, width_, height_}, 0, 0);
}

// ------------------------------------------------------------------ the window

WindowsScreen::WindowsScreen(allocators::IAllocator& allocator) : Frame(allocator), view_(allocator) {
  set_title("Windows");
  set_padding(gui::Insets::all(0));
  set_escape_closes(false);      // (Escape is the Windows programs' key)
  view_.set_dock(gui::DockStyle::Fill);
  add(&view_);
}

WindowsScreen::~WindowsScreen() { release_view(allocator(), staging_); }

bool WindowsScreen::open(gui::WindowManager& manager) {
  refresh_ = true;
  gui::UiGuard lock(&manager);
  if (showing()) {
    activate();
    return true;
  }
  const uint32_t w = (view_.picture_width() ? view_.picture_width() : 1024) + kFrameWidth;
  const uint32_t h = (view_.picture_height() ? view_.picture_height() : 768) + kFrameHeight;
  const gui::Rect2D area = manager.dialog_area();
  set_bounds({area.x + 24, area.y + 24, w, h});
  return manager.add_window(this);
}

void WindowsScreen::pump(gui::WindowManager& manager) {
  if (!session_) return;
  if (!sized_) {      // the Windows programs' screen fits the work area with the window's frame around it (and a margin), before any of them draws on it
    sized_ = true;
    const gui::Rect2D area = manager.dialog_area();
    const int32_t margin = 48;
    const int32_t w = static_cast<int32_t>(area.width) - kFrameWidth - margin, h = static_cast<int32_t>(area.height) - kFrameHeight - margin;
    if (w >= 320 && h >= 200) session_->set_screen_size(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
  }
  const bool by_hand = __atomic_exchange_n(&refresh_, false, __ATOMIC_ACQ_REL);
  const bool changed = session_->take_screen_changed();
  if (!changed && !by_hand) return;
  const uint32_t width = session_->screen_width(), height = session_->screen_height();
  const uint64_t count = uint64_t{width} * height;
  if (staging_.size() != count) {
    release_view(allocator(), staging_);
    staging_ = allocate_view<uint32_t>(allocator(), count);
    if (staging_.size() != count) return;
  }
  uint32_t w = 0, h = 0;
  if (!session_->copy_screen(staging_, &w, &h) || uint64_t{w} * h != count) return;      // (resized in between: the next change brings it)
  const bool resized = w != view_.picture_width() || h != view_.picture_height();
  if (!view_.set_picture(staging_, w, h)) return;
  if (!opened_once_ && changed) {      // the first picture a Windows program draws opens the window; after the user closed it, the Start menu does
    opened_once_ = true;
    open(manager);
  } else if (resized && showing()) {
    gui::UiGuard lock(&manager);
    const gui::Rect2D b = bounds();
    set_bounds({b.x, b.y, w + kFrameWidth, h + kFrameHeight});
  }
}

bool WindowsScreen::picture_area(gui::Rect2D* area) const {
  if (!showing() || state() == State::Minimized) return false;
  const gui::Point origin = view_.window_origin();
  const gui::Rect2D b = view_.bounds();
  const uint32_t w = view_.picture_width() < b.width ? view_.picture_width() : b.width;
  const uint32_t h = view_.picture_height() < b.height ? view_.picture_height() : b.height;
  if (w == 0 || h == 0) return false;
  *area = {origin.x, origin.y, w, h};
  return true;
}

void WindowsScreen::release_grab() {
  for (uint32_t i = 0; i < 3; ++i)
    if (buttons_ & kMouseButtons[i]) session_->mouse_button(i + 1, false);      // (a button held when the pointer goes is let go in the Windows programs too)
  buttons_ = 0;
  grabbed_ = false;
}

// ------------------------------------------------------------------ input

void WindowsScreen::tap_key(const hardware::KeyEvent& event, uint8_t modifiers) {
  if (!session_) return;
  const uint8_t bit = static_cast<uint8_t>(1u << (event.usage & 7));
  uint8_t& slot = keys_down_[event.usage >> 3];
  if (!event.pressed) {      // a release goes where the press went
    if (!(slot & bit)) return;
    slot = static_cast<uint8_t>(slot & ~bit);
    session_->key(event.usage, false, modifiers);
    return;
  }
  if (!showing() || !active() || state() == State::Minimized) return;
  if ((modifiers & hardware::modifier::kControl) && (modifiers & (hardware::modifier::kLeftAlt | hardware::modifier::kRightAlt)) && grabbed_) {      // Ctrl+Alt: the pointer is the desktop's again
    release_grab();
    released_ = true;
  }
  slot = static_cast<uint8_t>(slot | bit);
  session_->key(event.usage, true, modifiers);
}

void WindowsScreen::tap_mouse(const hardware::MouseEvent& event, int32_t* x, int32_t* y) {
  if (!session_) return;
  gui::Rect2D area;
  if (!picture_area(&area)) {
    if (grabbed_) release_grab();
    return;
  }
  const bool inside = contains(area, *x, *y);
  const uint8_t pressed_now = static_cast<uint8_t>(event.buttons & ~buttons_);
  if (released_) {
    if (!inside || !pressed_now) return;
    released_ = false;      // a click on the picture takes the pointer back
  }
  if (!grabbed_) {
    if (!inside) return;
    grabbed_ = true;
    session_->mouse_position(*x - area.x, *y - area.y);      // entering: the Windows cursor goes where the pointer came in
  } else if (!inside && buttons_ == 0 && (event.buttons & 7) == 0 && !session_->cursor_confined()) {
    grabbed_ = false;      // leaving the picture with no button held and nothing confining the cursor: the pointer is the desktop's again
    return;
  } else if (event.absolute) {
    session_->mouse_position(*x - area.x, *y - area.y);
  } else if (event.dx != 0 || event.dy != 0) {
    session_->mouse_motion(event.dx, event.dy);
  }
  const uint8_t changed = static_cast<uint8_t>((event.buttons ^ buttons_) & 7);
  for (uint32_t i = 0; i < 3; ++i)
    if (changed & kMouseButtons[i]) session_->mouse_button(i + 1, (event.buttons & kMouseButtons[i]) != 0);
  buttons_ = static_cast<uint8_t>(event.buttons & 7);
  if (event.wheel != 0) session_->mouse_wheel(event.wheel);
  // The pointer shows where the Windows cursor is (a program may have confined or moved it).
  int32_t cx = 0, cy = 0;
  session_->cursor(&cx, &cy);
  cx = cx < 0 ? 0 : cx >= static_cast<int32_t>(area.width) ? static_cast<int32_t>(area.width) - 1 : cx;
  cy = cy < 0 ? 0 : cy >= static_cast<int32_t>(area.height) ? static_cast<int32_t>(area.height) - 1 : cy;
  *x = area.x + cx;
  *y = area.y + cy;
}

}  // namespace desktop
