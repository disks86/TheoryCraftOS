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
// arrangement is a row, left to right in connector order, tops aligned; the order can be changed and any monitor made the primary one (the
// one with the taskbar, where windows and dialogs open). A monitor that is unplugged keeps its place but is disabled, so the picture does
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
