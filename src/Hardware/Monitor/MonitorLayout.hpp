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

// How the monitors are arranged on the desktop: the desktop is one big picture and every monitor shows a rectangle of it. The default
// arrangement is a row, left to right in connector order, tops aligned; a monitor can be dragged anywhere next to another (place_adjacent()), and
// any monitor made the primary one (the one with the taskbar, where windows and dialogs open).
//
// A monitor has a pixel size - the mode it runs at - and a scale (100 % by default; 200 % makes everything twice as big, the pixel density of the
// desktop half of the monitor's): its rectangle on the desktop has the size of its pixels divided by the scale, the "logical" size the programs
// see. Rectangles never overlap and always touch each other, so the pointer can walk from any monitor to any other. A monitor that is unplugged keeps its place but is disabled, so the picture does
// not change size under the programs - its windows are moved to the monitors that are left.
#pragma once
#include "Hardware/Monitor/MonitorTypes.hpp"
#include "ObjectContainer/List.hpp"

namespace hardware {

struct MonitorSlot {
  uint32_t id = 0;          // the monitor's id (its connector)
  MonitorRect rect;         // where it is on the desktop
  bool primary = false;
  bool enabled = true;      // false: unplugged, its place is kept
  uint32_t physical_width = 0, physical_height = 0;   // the monitor's mode in pixels; rect is this divided by scale_percent
  uint32_t scale_percent = 100;
};

class MonitorLayout {
 public:
  static constexpr uint32_t kMaxMonitors = 16;

  explicit MonitorLayout(allocators::IAllocator& allocator) : slots_(allocator) {}

  void clear() { slots_.clear(); }
  // Adds a monitor of this size to the right of the others (tops aligned). The first one becomes the primary. False if full or `id` is taken.
  bool add(uint32_t id, uint32_t width, uint32_t height);
  // Takes the monitor out (the primary passes to the first one left).
  bool remove(uint32_t id);

  uint32_t count() const { return static_cast<uint32_t>(slots_.count()); }
  uint32_t enabled_count() const;
  const MonitorSlot& at(uint32_t index) const { return slots_[index]; }
  int32_t find(uint32_t id) const;
  // The primary monitor's slot, or the first enabled one, or the first; null if there are none.
  const MonitorSlot* primary() const;

  bool set_primary(uint32_t id);
  bool set_enabled(uint32_t id, bool enabled);
  // Changes a monitor's size; the row is packed again (monitors keep their order).
  bool resize(uint32_t id, uint32_t width, uint32_t height);
  // Puts the monitor at `position` (0 = leftmost) in the row and packs the row again.
  bool move_to_position(uint32_t id, uint32_t position);
  // Places a monitor anywhere (for arrangements other than a row); call normalize() when done.
  bool set_position(uint32_t id, int32_t x, int32_t y);
  // The size of the monitor's rectangle for a mode and a scale (never below 1).
  static uint32_t logical_size(uint32_t pixels, uint32_t scale_percent);
  // Sets the monitor's mode (in pixels) and scale; its rectangle gets the logical size, keeping its corner, and moves to the nearest free place next
  // to the others if that makes it overlap one. False if the monitor is not there, a size is 0 or the scale is outside 100..400.
  bool set_mode(uint32_t id, uint32_t width, uint32_t height, uint32_t scale_percent);
  // Drops the monitor with its top left as near to (x, y) as it can go: next to another monitor on any side, not over one, keeping all of them
  // connected, and snapped to line up with the edges of the one it touches when within a few pixels. False (nothing moves) if there is no such place.
  bool place_adjacent(uint32_t id, int32_t x, int32_t y);
  // Replaces the whole layout (to go back to one that was saved).
  bool assign(const MonitorSlot* slots, uint32_t count);
  // True if the enabled monitors form one piece (each can be reached from the others over touching rectangles).
  bool connected() const;
  // After a monitor went away: if the ones that are left fell apart, they are put in a row again.
  void repair();
  // Moves everything so that the bounding box starts at (0, 0).
  void normalize();
  // Lines the monitors up in a row, in their current left-to-right order, tops aligned.
  void pack_row();

  // The smallest rectangle that holds all the monitors (enabled or not): the size of the desktop.
  MonitorRect bounds() const;
  // The enabled monitor under a point; -1 if there is none.
  int32_t monitor_at(int32_t x, int32_t y) const;
  // The enabled monitor nearest to a point (the one that has it if there is one); -1 if no monitor is enabled.
  int32_t nearest(int32_t x, int32_t y) const;
  // The enabled monitor that shows most of a rectangle, or the nearest to its centre when it is off every monitor; -1 if none.
  int32_t best_for(const MonitorRect& rect) const;
  // Moves a point to the nearest place on an enabled monitor (unchanged if it is on one). False if no monitor is enabled.
  bool clamp(int32_t* x, int32_t* y) const;

 private:
  List<MonitorSlot> slots_;
};

}  // namespace hardware
