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

// A window that holds several frames as tabs, like a browser window: a strip of tabs under the title bar (each with its title and a close
// button, and a "+" button after the last one), and the selected tab's frame filling the rest. The title bar shows the selected tab's
// title. Tabs are Frames that are not windows of their own (see Frame): the host adds them as children, shows one at a time, and tells them
// what manager they are in, so dialogs that a tab opens find the desktop.
//
//   - Clicking a tab selects it, its close button closes it (the last tab closes the window), "+" fires on_new_tab (the program decides what
//     a new tab holds), Ctrl+T does the same, Ctrl+Tab / Ctrl+Shift+Tab go to the next / previous tab.
//   - A tab can be dragged (see Window::begin_drag) by its label: dropped on the strip of this or another tab window it moves there, at the
//     place where it was dropped; dropped anywhere else - and the window has other tabs - the host fires on_tab_torn_off, torn_tab() being the
//     tab and torn_point() where the pointer was (window coordinates), and the program puts the tab in a window of its own.
#pragma once
#include "Gui/Windows/Frame.hpp"
#include "ObjectContainer/List.hpp"

namespace gui {

class TabFrame : public Frame {
 public:
  static constexpr int32_t kStripHeight = 24, kMaxTabWidth = 170, kMinTabWidth = 70, kNewTabWidth = 24, kCloseSize = 12;

  explicit TabFrame(allocators::IAllocator& allocator) : Frame(allocator), tabs_(allocator) {}

  // Adds `tab` (a frame that is in no manager; one that is a tab of another host moves here) at `index` (-1: at the end) and selects it.
  bool add_tab(Frame* tab, int32_t index = -1);
  // Takes the tab out and hands it back without closing it. A tab next to it is selected. An empty window stays open (see close_tab).
  bool remove_tab(Frame* tab);
  // Takes the tab out and runs its closed handler; closes the window when it was the last one.
  void close_tab(Frame* tab);
  void select_tab(Frame* tab);

  TabFrame* as_tabs() override { return this; }
  // Selects the next tab (the first after the last), or the previous one.
  void select_next(bool forward) { step_selection(forward ? 1 : -1); }
  Frame* selected() const { return selected_; }
  uint32_t tab_count() const { return static_cast<uint32_t>(tabs_.count()); }
  Frame* tab_at(uint32_t index) const { return index < tabs_.count() ? tabs_[index] : nullptr; }
  int32_t index_of(const Frame* tab) const;

  // Fired by the "+" button and Ctrl+T.
  void on_new_tab(Handler handler) { UiGuard lock(this); new_tab_ = handler; }
  void on_tab_torn_off(Handler handler) { UiGuard lock(this); torn_off_ = handler; }
  Frame* torn_tab() const { return torn_tab_; }
  Point torn_point() const { return {torn_x_, torn_y_}; }
  // The tab being dragged (nullptr: none).
  Frame* dragging_tab() const { return dragging_tab_; }

  // Geometry, in this frame's coordinates (for painting and tests).
  Rect2D strip_rect() const;
  Rect2D tab_rect(uint32_t index) const;
  Rect2D close_rect(uint32_t index) const;
  Rect2D new_tab_button_rect() const;
  // The tab under a point, or -1.
  int32_t tab_at_point(int32_t x, int32_t y) const;

  Rect2D client_rect() const override;
  void layout() override;
  void paint(Painter& painter) override;
  void on_mouse_down(const MouseArgs& args) override;
  void on_mouse_move(const MouseArgs& args) override;
  void on_mouse_up(const MouseArgs& args) override;
  bool on_key(const KeyArgs& args) override;
  bool accepts_drop(StringView text, int32_t x, int32_t y, const Control* source) const override;
  void on_drop(StringView text, int32_t x, int32_t y, const Control* source) override;
  void on_drag_ended(bool dropped, int32_t x, int32_t y) override;

  friend class Frame;

 protected:
  void manager_changed() override;
  void window_closed(Window* window) override;
  void tab_title_changed(Frame* tab) override;

 private:
  enum class Press : uint8_t { None, Tab, Close, NewTab };

  void apply_selection();
  int32_t tab_width() const;
  void step_selection(int32_t direction);

  List<Frame*> tabs_;
  Frame* selected_ = nullptr;
  Handler new_tab_, torn_off_;
  Frame* torn_tab_ = nullptr;
  int32_t torn_x_ = 0, torn_y_ = 0;
  Frame* dragging_tab_ = nullptr;
  Press press_ = Press::None;
  Frame* press_tab_ = nullptr;
  int32_t press_x_ = 0, press_y_ = 0;
};

}  // namespace gui
