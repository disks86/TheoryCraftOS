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

// A top-level window: a bevelled frame with a title bar, minimize / maximize / close buttons in its top
// right corner, and a client area for ordinary controls (add them to the frame; they are laid out
// by dock style inside client_rect()). Frames live in a WindowManager, which stacks them, makes
// one active and does what the buttons ask. Each button can be disabled: it is then drawn greyed and
// ignores clicks (a dialog disables minimize and maximize).
//
// The title bar drags the window; the borders resize it (unless set_resizable(false)).
#pragma once
#include "Gui/Control.hpp"
#include "Utility/String.hpp"

namespace gui {

class WindowManager;

class Frame : public Control {
 public:
  static constexpr int32_t kBorder = 6, kTitleHeight = 18, kButtonWidth = 16, kButtonHeight = 14;
  static constexpr int32_t kMinWidth = 140, kMinHeight = 60;

  enum class State : uint8_t { Normal, Minimized, Maximized };
  enum class Part : uint8_t { None, Minimize, Maximize, Close, TitleBar, Client, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };

  explicit Frame(allocators::IAllocator& allocator) : Control(allocator), title_(allocator) {}

  void set_title(StringView title) { UiGuard lock(this); title_.assign(title); invalidate(); }
  StringView title() const { return title_.view(); }

  // The three title bar buttons (all enabled by default), and whether the borders resize.
  void set_minimize_enabled(bool enabled) { UiGuard lock(this); can_minimize_ = enabled; invalidate(); }
  void set_maximize_enabled(bool enabled) { UiGuard lock(this); can_maximize_ = enabled; invalidate(); }
  void set_close_enabled(bool enabled) { UiGuard lock(this); can_close_ = enabled; invalidate(); }
  bool minimize_enabled() const { return can_minimize_; }
  bool maximize_enabled() const { return can_maximize_; }
  bool close_enabled() const { return can_close_; }
  void set_resizable(bool resizable) { UiGuard lock(this); resizable_ = resizable; }
  // Escape closes the window (dialogs).
  void set_escape_closes(bool closes) { UiGuard lock(this); escape_closes_ = closes; }
  // Fired (see Handler: on the window's handler thread, if it has one) after the window was closed and
  // removed from its manager.
  void on_closed(Handler handler) { UiGuard lock(this); closed_ = handler; }

  State state() const { return state_; }
  bool active() const { return active_; }
  bool modal() const { return modal_; }
  WindowManager* manager() const { return __atomic_load_n(&manager_, __ATOMIC_ACQUIRE); }  // safe from any thread
  // What the manager does for the buttons; no-ops while the frame is not in a manager.
  void minimize();
  void maximize();   // or restore, if already maximized
  void restore();
  void close();
  void activate();

  // Where a point (frame coordinates) lies: a button, the title bar, a border, the client area.
  Part part_at(int32_t x, int32_t y) const;
  Rect2D button_rect(Part button) const;

  // A double arrow over the borders and corners that resize the window (and while one is being dragged).
  CursorShape cursor_shape(int32_t x, int32_t y) const override;
  Rect2D client_rect() const override;
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 private:
  friend class WindowManager;

  bool button_enabled(Part button) const;
  void draw_button(Painter& painter, Part button);

  String title_;
  Handler closed_;
  WindowManager* manager_ = nullptr;
  State state_ = State::Normal;
  State before_minimize_ = State::Normal;
  Rect2D restore_bounds_;
  bool can_minimize_ = true, can_maximize_ = true, can_close_ = true, resizable_ = true;
  bool escape_closes_ = false;
  bool active_ = false, modal_ = false;
  // Mouse interaction in progress.
  Part pressed_ = Part::None;  // a title bar button held down
  bool pressed_inside_ = false;
  Part dragging_ = Part::None;  // TitleBar (moving) or a border (resizing)
  int32_t grab_x_ = 0, grab_y_ = 0;
  Rect2D grab_bounds_;
};

}  // namespace gui
