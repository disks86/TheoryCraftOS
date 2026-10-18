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

// A stacking window manager: the container that holds the Frames of the desktop. Children are in
// stacking order (the last one is on top and is hit first); pressing the mouse anywhere on a window raises
// and activates it. The manager keeps a second list in creation order for the taskbar's window
// switcher. Windows are positioned freely inside the manager's area (the desktop above the taskbar).
//
// Modal windows: while one is open every other window is disabled (ignores input, drawn inactive,
// skipped by Tab), and the modal stays on top.
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Windows/Frame.hpp"

namespace gui {

class WindowManager : public Control {
 public:
  explicit WindowManager(allocators::IAllocator& allocator) : Control(allocator), order_(allocator) {}

  // Adds a window on top of the others and activates it. Its bounds (relative to the manager) are
  // used as given, kept inside the area. False if it is already managed or memory ran out.
  bool add_window(Frame* frame, bool modal = false);
  // Takes the window out without telling it (see close_window()).
  bool remove_window(Frame* frame);
  // What the close button does: takes the window out, then runs its on_closed handler.
  void close_window(Frame* frame);

  void activate(Frame* frame);  // raises it (restoring it first if minimized) and makes it active
  void minimize(Frame* frame);
  void maximize(Frame* frame);  // toggles with the restored size
  void restore(Frame* frame);
  // Docks the window to a half of the area (see Frame::dock). The same edge again, or Dock::None, puts it back.
  void dock(Frame* frame, Frame::Dock edge);

  Frame* active() const { return active_; }
  uint32_t window_count() const { return static_cast<uint32_t>(order_.count()); }
  Frame* window_at(uint32_t creation_index) const { return creation_index < order_.count() ? order_[creation_index] : nullptr; }
  bool has_modal() const;
  // The stacking order: 0 = bottom.
  uint32_t stacking_index(const Frame* frame) const;

  void layout() override;
  void on_descendant_pressed(Control* child) override;

 private:
  void refresh();                 // enabled flags, active window, focus scope after any change
  Frame* top_usable() const;      // the topmost visible, enabled window
  void keep_inside(Frame* frame);
  Rect2D dock_bounds(Frame::Dock edge) const;

  List<Frame*> order_;  // creation order
  Frame* active_ = nullptr;
};

}  // namespace gui
