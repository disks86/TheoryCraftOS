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

// What the monitor layer talks about: the kind of connector a monitor hangs on, a video mode, a rectangle of the desktop.
#pragma once
#include <stdint.h>

namespace hardware {

enum class ConnectorType : uint8_t { Unknown, Vga, Dvi, Hdmi, DisplayPort, Internal, Composite, Component, Virtual };
const char* connector_type_name(ConnectorType type);   // "VGA", "DVI", "HDMI", "DisplayPort", "Internal", ...

// A video mode: pixels and the refresh rate in thousandths of a hertz (59940 = 59.94 Hz; 0: not known).
struct MonitorMode {
  uint32_t width = 0, height = 0;
  uint32_t refresh_millihz = 0;
  bool interlaced = false;
  bool preferred = false;   // the monitor's own choice (its native mode)
};

// A rectangle of the desktop - the one big picture that all the monitors show a part of - in pixels. x and y may be negative while a layout is
// being edited; a finished layout starts at (0, 0).
struct MonitorRect {
  int32_t x = 0, y = 0;
  uint32_t width = 0, height = 0;

  int32_t right() const { return x + static_cast<int32_t>(width); }
  int32_t bottom() const { return y + static_cast<int32_t>(height); }
  bool contains(int32_t px, int32_t py) const { return px >= x && py >= y && px < right() && py < bottom(); }
  bool empty() const { return width == 0 || height == 0; }
  // The area of the overlap with `other`, in pixels.
  uint64_t overlap(const MonitorRect& other) const {
    const int32_t l = x > other.x ? x : other.x, t = y > other.y ? y : other.y;
    const int32_t r = right() < other.right() ? right() : other.right(), b = bottom() < other.bottom() ? bottom() : other.bottom();
    return r > l && b > t ? uint64_t{static_cast<uint32_t>(r - l)} * static_cast<uint32_t>(b - t) : 0;
  }
  bool operator==(const MonitorRect& o) const { return x == o.x && y == o.y && width == o.width && height == o.height; }
};

}  // namespace hardware
