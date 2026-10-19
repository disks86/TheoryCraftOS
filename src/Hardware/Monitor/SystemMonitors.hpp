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

// Finds every monitor of the machine's display device and arranges them on the desktop. initialize() asks the device (its IMonitorSource) for
// its connectors, reads the EDID of each monitor that is plugged in, and puts them side by side in a row, each at the size its EDID calls
// preferred; apply() then makes the display device show it that way: one big desktop surface with each monitor showing its rectangle. Devices
// that can drive only one display (or tell nothing about monitors) give a single monitor, the whole screen.
//
// What each monitor shows can be changed: its resolution (among the modes its EDID lists that the display device can also show - nothing else is
// offered), its refresh rate (the rates the EDID lists for that resolution, when the device can set one), its scale (100 % ... 400 %: the
// pixel density of the desktop, from which the size of its rectangle follows) and its place among the other monitors (dragged next to one of
// them). apply() then makes the display device show it: the "surface" - one big picture holding every monitor's pixels in a row - is built to
// fit, and the desktop the programs see ("logical", the monitors' rectangles at their scales and places) is mapped onto it by whoever presents
// the pictures (MonitorDisplay). The size of both can change at any time; a monitor pulled out keeps its place (disabled) and gets it back when
// plugged in again; one that was not there at start-up is listed but not used until the next start, because there is no room for it.
#pragma once
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Hardware/Monitor/Monitor.hpp"
#include "Hardware/Monitor/MonitorLayout.hpp"
#include "ObjectContainer/List.hpp"

namespace hardware {

class SystemMonitors {
 public:
  // The largest desktop surface that is built, in pixels (a bigger arrangement is cut back to each monitor's smallest mode).
  static constexpr uint64_t kMaxDesktopPixels = 16u << 20;
  static constexpr uint32_t kMaxMonitorWidth = 3840, kMaxMonitorHeight = 2400;

  // `gpu` may be null (no monitors then). It must outlive this object.
  SystemMonitors(IGraphicsProcessingUnit* gpu, allocators::IAllocator& allocator) : gpu_(gpu), monitors_(allocator), layout_(allocator) {}

  void initialize();

  // The monitors found (connected or not), in connector order; the layout's ids are their ids.
  uint32_t count() const { return static_cast<uint32_t>(monitors_.count()); }
  const Monitor& monitor(uint32_t index) const { return monitors_[index]; }
  const Monitor* find(uint32_t id) const;
  uint32_t connected_count() const;
  MonitorLayout& layout() { return layout_; }
  const MonitorLayout& layout() const { return layout_; }
  // True if the display device can show more than one monitor.
  bool multi_display() const { return multi_; }

  // Makes the display device show the layout: the surface is built to hold every enabled monitor's pixels in a row, and each monitor shows its
  // part of it. The desktop (the layout's bounds) keeps its own size. False if the device cannot (it keeps showing what it showed, and only the
  // primary monitor is in use).
  bool apply();
  // Where each enabled monitor's pixels are on the surface: a row in the order of the monitors' places (left to right, then top to bottom), tops
  // aligned. Fills `out` and returns how many.
  uint32_t physical_placements(ScanoutPlacement* out, uint32_t capacity) const;
  // The size of the surface (the pixels of all the monitors side by side) as last applied; and whether the desktop has to be drawn to one size and
  // shown on the surface at another (some monitor scaled, or the monitors not in a row of equal tops): then a MonitorDisplay does the copying.
  uint32_t surface_width() const { return surface_width_; }
  uint32_t surface_height() const { return surface_height_; }
  bool needs_compositing() const;

  // --- What a monitor shows. ids are the monitors' ids (their connectors).
  // The resolutions that can be chosen: the monitor's native one first (when it has one), then from the largest down; each size once, with the
  // refresh rate that goes with it. Sizes in the monitor's EDID that the display device cannot show are left out, as are sizes under 640x480 and over
  // kMaxMonitorWidth x kMaxMonitorHeight; a monitor without an EDID offers the modes of the device. The one in use is always in the list.
  uint32_t compatible_resolutions(uint32_t id, MonitorMode* out, uint32_t capacity) const;
  // The refresh rates (thousandths of a hertz, highest first) the monitor takes at that resolution and the device can set. A device that cannot set
  // the rate gives just the one in use; 0 stands for "not known".
  uint32_t compatible_refresh_rates(uint32_t id, uint32_t width, uint32_t height, uint32_t* out, uint32_t capacity) const;
  bool can_set_refresh(uint32_t id) const { return source_ && source_->can_set_refresh(id); }
  // The scales (percent) that leave the desktop of the monitor at least 640x480 (at its resolution, or at `width` x `height` when given): 100, 125, 150,
  // 175, 200, 250, 300, 400.
  uint32_t compatible_scales(uint32_t id, uint32_t* out, uint32_t capacity, uint32_t width = 0, uint32_t height = 0) const;
  uint32_t scale_of(uint32_t id) const;
  // Each fails (and changes nothing) if the value is not among the compatible ones, or the device refuses. The layout is changed; call apply() to show it.
  bool set_resolution(uint32_t id, uint32_t width, uint32_t height);
  bool set_refresh(uint32_t id, uint32_t refresh_millihz);
  bool set_scale(uint32_t id, uint32_t scale_percent);
  // Drops the monitor with its top left near (x, y) of the desktop, next to the others (see MonitorLayout::place_adjacent()).
  bool move_monitor(uint32_t id, int32_t x, int32_t y);

  // --- Going back: everything above that apply() shows can be saved and put back (a bad resolution is taken back when the user does not keep it).
  struct State {
    uint32_t count = 0;
    MonitorSlot slots[MonitorLayout::kMaxMonitors];
    MonitorMode modes[MonitorLayout::kMaxMonitors];   // the mode of the monitor with the slot's id
    bool valid() const { return count != 0; }
  };
  void save(State* out) const;
  // Puts the layout and the modes back (the device is told for single-display devices); call apply() after.
  bool restore(const State& state);
  // Shows only the primary monitor again, as a screen of its own size (for the console when the desktop is over). False if the device shows one
  // display anyway.
  bool show_single_display();
  // Shows the layout again after it was edited (monitors reordered, another one made primary); the desktop surface keeps its size, so only the
  // rectangles the monitors show change. False if the device cannot.
  bool reapply();
  // Looks for monitors that came or went. True if anything changed; the layout is updated (and shown) to match.
  bool poll();
  // The size of the desktop - the layout's bounds, in the units the programs draw in - as last applied (0 before apply()).
  uint32_t desktop_width() const { return desktop_width_; }
  uint32_t desktop_height() const { return desktop_height_; }
  // Monitors that were plugged in after start-up and found no room (the desktop would be too big).
  uint32_t waiting_for_restart() const { return waiting_; }
  // Counts up whenever the set of monitors in use changes.
  uint32_t generation() const { return generation_; }

 private:
  void add_connector(uint32_t index, const ConnectorInfo& info);
  Monitor* find_mutable(uint32_t id);
  bool add_to_layout(uint32_t id);
  uint32_t best_refresh(const Monitor& monitor, uint32_t width, uint32_t height) const;
  MonitorMode mode_for(const Monitor& monitor) const;
  bool show();

  IGraphicsProcessingUnit* gpu_;
  IMonitorSource* source_ = nullptr;
  List<Monitor> monitors_;
  MonitorLayout layout_;
  bool multi_ = false;
  bool applied_ = false;
  uint32_t desktop_width_ = 0, desktop_height_ = 0, surface_width_ = 0, surface_height_ = 0;
  uint32_t waiting_ = 0, generation_ = 0;
};

}  // namespace hardware
