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
// A frame can also be a tab of a TabFrame (a window that holds several frames, one shown at a time): it then has no title bar, border or
// buttons of its own, only its client area, which fills the tab area of the host. Everything that works on the window - minimize, maximize,
// dock, activate, state(), active(), manager() - then means the host window (close() closes just the tab); host() tells which one.
// The title bar drags the window; the borders resize it (unless set_resizable(false)). Four more buttons left of those dock the window to a
// half of the desktop area - the left or right half, or the upper or lower one - and pressing the one of the edge the window is docked at
// puts it back. A sixth button tiles all the windows (up to four, the topmost ones) into a grid, a quarter of the area each. They are drawn for windows that can be maximized and resized. Moving or resizing a docked window frees it; maximizing it
// and restoring that brings back the size it had before it was docked.
#pragma once
#include "Gui/Control.hpp"
#include "Utility/String.hpp"

namespace gui {

class WindowManager;
class TabFrame;

class Frame : public Control {
 public:
  static constexpr int32_t kBorder = 6, kTitleHeight = 18, kButtonWidth = 16, kButtonHeight = 14;
  static constexpr int32_t kMinWidth = 140, kMinHeight = 60;

  enum class State : uint8_t { Normal, Minimized, Maximized };
  enum class Dock : uint8_t { None, Left, Right, Top, Bottom, Tile };
  enum class Part : uint8_t { None, Minimize, Maximize, Close, TitleBar, Client, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight,
                              DockLeft, DockRight, DockTop, DockBottom, Tile };

  explicit Frame(allocators::IAllocator& allocator) : Control(allocator), title_(allocator) {}

  void set_title(StringView title) {
    UiGuard lock(this);
    title_.assign(title);
    invalidate();
    if (host_) host_changed_title();
  }
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

  State state() const;
  // Where the window goes back to: its bounds, or - maximized, docked or tiled (or minimized from one of those) - the ones from before.
  Rect2D normal_bounds() const;
  // The state a minimized window comes back to (Normal or Maximized).
  State state_before_minimize() const;
  // The half of the desktop the window is docked to (None: free).
  Dock docked() const { return docked_; }
  // Whether the dock buttons show: the window can be maximized and resized.
  bool dock_enabled() const { return can_maximize_ && resizable_; }
  bool active() const;
  // The tabbed window this frame is a tab of (nullptr: it is a window of its own).
  Frame* host() const;
  bool embedded() const { return host_ != nullptr; }
  // This frame as a tabbed window (nullptr: it is not one).
  virtual TabFrame* as_tabs() { return nullptr; }
  // A number the program that made the frame can give it to tell the kinds of windows apart (a new tab opens a window of the same kind).
  void set_app_kind(uint8_t kind) { app_kind_ = kind; }
  uint8_t app_kind() const { return app_kind_; }
  // The size the frame had as a window of its own (before it became a tab); empty if it has not been a tab yet.
  Rect2D home_bounds() const { return home_; }
  bool modal() const { return modal_; }
  WindowManager* manager() const { return __atomic_load_n(&manager_, __ATOMIC_ACQUIRE); }  // safe from any thread
  // What the manager does for the buttons; no-ops while the frame is not in a manager.
  void minimize();
  void maximize();   // or restore, if already maximized
  void restore();
  void close();
  // Fullscreen: the frame fills the window manager's area with no title bar, border or tab strip (a tab does this for the window that holds it); the state it
  // had comes back when it is switched off.
  void set_fullscreen(bool on);
  bool fullscreen() const;
  void activate();
  // Docks the window to `edge`, or puts it back where it was when it is docked there already (Dock::None also frees it).
  void dock(Dock edge);
  // The tile button: the windows are arranged in a grid (see WindowManager::tile()); pressed again by a tiled window, they all go back.
  void tile();

  // Where a point (frame coordinates) lies: a button, the title bar, a border, the client area.
  Part part_at(int32_t x, int32_t y) const;
  Rect2D button_rect(Part button) const;

  // A double arrow over the borders and corners that resize the window (and while one is being dragged).
  CursorShape cursor_shape(int32_t x, int32_t y) const override;
  Rect2D client_rect() const override;
  bool handler_lane() const override { return true; }
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;

 protected:
  // Told when the manager the frame is in changes (a window manager added or removed it): a tabbed window passes that on to its tabs.
  virtual void manager_changed() {}
  // Told when the window manager closes the frame (after it was taken out): a tabbed window closes its tabs.
  virtual void window_closed(Window* /*window*/) {}
  // A tab changed its title / the host is asked to...
  virtual void tab_title_changed(Frame* /*tab*/) {}

  // The tab-side hooks of TabFrame; the closed handler of a frame is run the way the window manager runs it.
  void run_closed_handler(Window* window);

 private:
  friend class WindowManager;
  friend class TabFrame;
  void host_changed_title();

  bool button_enabled(Part button) const;
  void draw_button(Painter& painter, Part button);
  static bool is_dock_part(Part part) { return part >= Part::DockLeft && part <= Part::DockBottom; }
  static Dock dock_of(Part part) { return static_cast<Dock>(static_cast<uint8_t>(part) - static_cast<uint8_t>(Part::DockLeft) + 1); }

  String title_;
  Handler closed_;
  WindowManager* manager_ = nullptr;
  State state_ = State::Normal;
  State before_minimize_ = State::Normal;
  Rect2D restore_bounds_;
  bool can_minimize_ = true, can_maximize_ = true, can_close_ = true, resizable_ = true;
  bool escape_closes_ = false;
  Dock docked_ = Dock::None;
  uint8_t tile_slot_ = 0, tile_count_ = 0;   // the place in the grid while docked_ is Tile
  TabFrame* host_ = nullptr;
  bool fullscreen_ = false, fullscreen_maximized_ = false;
  uint8_t app_kind_ = 0;
  Rect2D home_;
  bool active_ = false, modal_ = false;
  // Mouse interaction in progress.
  Part pressed_ = Part::None;  // a title bar button held down
  bool pressed_inside_ = false;
  Part dragging_ = Part::None;  // TitleBar (moving) or a border (resizing)
  int32_t grab_x_ = 0, grab_y_ = 0;
  Rect2D grab_bounds_;
};

}  // namespace gui
