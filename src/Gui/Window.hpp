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

// The root of a control tree: sized to the screen (or part of it), draws an
// optional title bar, turns raw input (mouse position/buttons, key presses)
// into control events and owns focus, mouse capture and the single popup
// (drop-down list or menu) that is drawn above everything else.
//
// Typical use: build controls, add them to the window, then per frame feed input
// (InputPump does that from the keyboard and mouse drivers) and call render() when
// dirty() says something changed.
#pragma once
#include "Gui/Control.hpp"
#include "Utility/String.hpp"

namespace gui {

class Window final : public Control {
 public:
  Window(allocators::IAllocator& allocator, graphics::IRenderer2D& renderer, const Theme& theme,
         const graphics::Extent2D& size);

  // The title bar is drawn when the title is not empty.
  void set_title(StringView title);
  static constexpr int32_t kTitleHeight = 20;

  Rect2D client_rect() const override;
  void paint(Painter& painter) override;

  // Lays out, draws everything (children, popup, cursor) and presents. Clears dirty().
  void render();
  bool dirty() const { return dirty_; }
  void mark_dirty() { dirty_ = true; }
  // Draws a small arrow at the mouse position (for relative-mouse setups with no hardware cursor).
  void show_cursor(bool show) { cursor_visible_ = show; mark_dirty(); }

  // --- Input, in window coordinates.
  void mouse_move(int32_t x, int32_t y);
  void mouse_button(int32_t x, int32_t y, MouseButton button, bool pressed);
  void mouse_wheel(int32_t x, int32_t y, int32_t delta);
  void key(const KeyArgs& args);  // a key press (or repeat)

  // --- Focus. Tab / Shift+Tab move it through focusable controls in tree order.
  Control* focus() const { return focus_; }
  void set_focus(Control* control);
  void focus_next(bool forward);

  // --- One popup at a time, drawn above the tree and given input first. `owner` is told when it
  // closes. (x, y) is where its top left should go; it is moved to stay inside the window. The popup
  // is not part of the tree and is not owned by the window.
  bool open_popup(Control* popup, Control* owner, int32_t x, int32_t y);
  void close_popup();
  Control* popup() const { return popup_; }
  Control* popup_owner() const { return popup_owner_; }

  // Called by Control::remove so no pointer to a removed control lingers.
  void control_removed(Control* control);

 private:
  Control* target_at(int32_t x, int32_t y, Point* origin);
  void deliver_move(Control* target, const Point& origin, int32_t x, int32_t y);
  static bool is_inside(const Control* ancestor, const Control* c);
  uint32_t collect_focusable(Control* from, Control** out, uint32_t count, uint32_t capacity);

  graphics::IRenderer2D& renderer_;
  const Theme& theme_;
  String title_;
  Control* focus_ = nullptr;
  Control* hover_ = nullptr;
  Control* captured_ = nullptr;
  Control* popup_ = nullptr;
  Control* popup_owner_ = nullptr;
  Point cursor_;
  bool cursor_visible_ = false;
  bool dirty_ = true;
};

}  // namespace gui
