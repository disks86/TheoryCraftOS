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

// What a graphics device offers for finding and driving monitors beyond "the screen": the connectors it has, what is plugged into them
// (their EDID), and - on devices that can show different parts of one picture on different displays - the placement of the monitors.
// Graphics drivers implement it (virtio-gpu has several scanouts; the UEFI framebuffer has one display) and hand it out through
// IGraphicsProcessingUnit::monitor_source().
#pragma once
#include "Hardware/Monitor/MonitorTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

struct ConnectorInfo {
  bool connected = false;
  ConnectorType type = ConnectorType::Unknown;
  char name[16] = {};                          // "HDMI-1", "Virtual-0"
  uint32_t preferred_width = 0, preferred_height = 0;   // what the device itself says is the display's size (0: not known)
};

// Where one monitor shows what part of the desktop: the monitor behind connector `connector` shows the `width` x `height` rectangle at (x, y).
struct ScanoutPlacement {
  uint32_t connector = 0;
  uint32_t x = 0, y = 0, width = 0, height = 0;
};

class IMonitorSource {
 public:
  static constexpr uint32_t kMaxConnectors = 16;

  virtual uint32_t connector_count() const = 0;
  virtual bool connector_info(uint32_t connector, ConnectorInfo* out) const = 0;
  // The EDID of the monitor on the connector, up to out.size() bytes; returns the number of bytes (0: the monitor has none or nothing is connected).
  virtual uint32_t read_edid(uint32_t connector, ByteView out) = 0;
  // Looks at the connectors again (monitors plugged in or pulled out); true if anything differs from the last look.
  virtual bool refresh_connectors() { return false; }

  // --- Modes. A monitor's EDID says what it can show; these say what the graphics device can do with it.
  // True if the device can show the monitor at this size (default: any).
  virtual bool mode_available(uint32_t connector, uint32_t width, uint32_t height) const {
    (void)connector; (void)width; (void)height;
    return true;
  }
  // The modes the device itself offers for the monitor on the connector (used for a monitor that gave no EDID); returns how many (default: none).
  virtual uint32_t device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const {
    (void)connector; (void)out; (void)capacity;
    return 0;
  }
  // Whether the device can change the refresh rate of the monitor (most cannot: they take the one the firmware picked), and does so.
  virtual bool can_set_refresh(uint32_t connector) const { (void)connector; return false; }
  virtual bool set_refresh(uint32_t connector, uint32_t refresh_millihz) { (void)connector; (void)refresh_millihz; return false; }
  // A device that can show one display only: switches it to width x height now (the screen changes size). False if it has no such mode.
  virtual bool set_display_mode(uint32_t connector, uint32_t width, uint32_t height) {
    (void)connector; (void)width; (void)height;
    return false;
  }

  // True if the device can show separate rectangles of one big picture on separate monitors (apply_layout() works).
  virtual bool supports_multiple_displays() const { return false; }
  // Makes the picture `desktop_width` x `desktop_height` pixels (the display memory is replaced; what was drawn is lost) and shows on each placed
  // monitor its part of it. Connectors not listed show nothing. False if that cannot be done (nothing changes).
  virtual bool apply_layout(const ScanoutPlacement* placements, uint32_t count, uint32_t desktop_width, uint32_t desktop_height) {
    (void)placements; (void)count; (void)desktop_width; (void)desktop_height;
    return false;
  }

 protected:
  constexpr IMonitorSource() = default;
  ~IMonitorSource() = default;
};

}  // namespace hardware
