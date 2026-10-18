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
// Several monitors: the manager can be told the work areas of the monitors it spreads over (set_areas(), the primary monitor's first; without
// any, the whole manager is the one area). Maximizing, docking and tiling then work on the area a window is mostly on, a window is kept on some
// area, and dialogs open in the area of the active window (or the primary one).
//
// Modal windows: while one is open every other window is disabled (ignores input, drawn inactive,
// skipped by Tab), and the modal stays on top.
#pragma once
#include "Gui/Control.hpp"
#include "Gui/Windows/Frame.hpp"

namespace gui {

class WindowManager : public Control {
 public:
  explicit WindowManager(allocators::IAllocator& allocator) : Control(allocator), order_(allocator), areas_(allocator) {}

  // Adds a window on top of the others and activates it. Its bounds (relative to the manager) are
  // used as given, kept inside the area. False if it is already managed or memory ran out.
  bool add_window(Frame* frame, bool modal = false);
  // Takes the window out without telling it (see close_window()).
  bool remove_window(Frame* frame);
  // What the close button does: takes the window out, then runs its on_closed handler.
  void close_window(Frame* frame);

  // The tile button: arranges the windows in a grid over the area, a quarter each - the topmost four that are not minimized and can be resized.
  // Fewer windows share the area (one fills it, two take a half each, three have two quarters over a half). `origin` is the window whose
  // button was pressed; if it is tiled already the tiled windows all go back to where they were.
  void tile(Frame* origin);
  // Alt+Tab: brings the window at the bottom of the stack to the front and activates it (restoring it if minimized), so repeating it goes through
  // all of them; backwards, the window in front goes to the bottom. Does nothing while a modal window is open.
  void cycle(bool forward);

  // The work areas (in the manager's coordinates), the primary's first. Windows are moved to fit; nothing is lost when the list changes.
  void set_areas(const Rect2D* areas, uint32_t count);
  uint32_t area_count() const { return static_cast<uint32_t>(areas_.count()); }
  // The area `index` (the whole manager if none are set).
  Rect2D area(uint32_t index) const;
  // The area that most of the window is on (or is nearest to).
  Rect2D area_for(const Frame* frame) const;
  // Where dialogs go: the area of the active window, else the primary area.
  Rect2D dialog_area() const;

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
  Rect2D dock_bounds(Frame::Dock edge, const Rect2D& area) const;
  Rect2D tile_bounds(uint32_t slot, uint32_t count, const Rect2D& area) const;
  Rect2D area_of_rect(const Rect2D& r) const;

  List<Frame*> order_;  // creation order
  List<Rect2D> areas_;
  Frame* active_ = nullptr;
};

}  // namespace gui
