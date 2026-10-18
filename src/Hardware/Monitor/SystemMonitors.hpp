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
// The desktop surface is sized once, by apply(). Later changes keep that size: a monitor pulled out keeps its place (disabled) and gets it back
// when plugged in again; one that was not there at start-up is listed but not used until the next start, because there is no room for it.
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

  // Makes the display device show the layout: the desktop surface gets the size of the layout's bounds (which has to start at 0, 0) and each
  // enabled monitor shows its rectangle. False if the device cannot (it keeps showing what it showed, and only the first monitor is in use).
  bool apply();
  // Shows only the primary monitor again, as a screen of its own size (for the console when the desktop is over). False if the device shows one
  // display anyway.
  bool show_single_display();
  // Shows the layout again after it was edited (monitors reordered, another one made primary); the desktop surface keeps its size, so only the
  // rectangles the monitors show change. False if the device cannot.
  bool reapply();
  // Looks for monitors that came or went. True if anything changed; the layout is updated (and shown) to match.
  bool poll();
  // The size of the desktop surface as last applied (0 before apply()).
  uint32_t desktop_width() const { return desktop_width_; }
  uint32_t desktop_height() const { return desktop_height_; }
  // Monitors that were plugged in after start-up and found no room.
  uint32_t waiting_for_restart() const { return waiting_; }
  // Counts up whenever the set of monitors in use changes.
  uint32_t generation() const { return generation_; }

 private:
  void add_connector(uint32_t index, const ConnectorInfo& info);
  MonitorMode mode_for(const Monitor& monitor) const;
  bool show();

  IGraphicsProcessingUnit* gpu_;
  IMonitorSource* source_ = nullptr;
  List<Monitor> monitors_;
  MonitorLayout layout_;
  bool multi_ = false;
  bool applied_ = false;
  uint32_t desktop_width_ = 0, desktop_height_ = 0;
  uint32_t waiting_ = 0, generation_ = 0;
};

}  // namespace hardware
