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

// One monitor, as the rest of the system sees it: what it is, what it can show, and what it is showing.
#pragma once
#include "Hardware/Monitor/Edid.hpp"
#include "Hardware/Monitor/MonitorTypes.hpp"

namespace hardware {

class IMonitor {
 public:
  // The number of the connector the monitor hangs on; stays the same for as long as the monitor is there.
  virtual uint32_t id() const = 0;
  // "Dell U2720Q" - from the EDID; "Display 1" when the monitor does not say.
  virtual const char* name() const = 0;
  virtual ConnectorType connector_type() const = 0;
  virtual const char* connector_name() const = 0;   // "HDMI-1"
  virtual bool connected() const = 0;
  // The monitor's EDID (nullptr: it gave none).
  virtual const EdidInfo* edid() const = 0;

  // The modes it can show (from its EDID; one mode, the current, for a monitor without).
  virtual uint32_t mode_count() const = 0;
  virtual bool mode_info(uint32_t index, MonitorMode* out) const = 0;
  // The mode it is shown at, in pixels.
  virtual MonitorMode current_mode() const = 0;
  // The physical size in millimetres (0: not known).
  virtual uint32_t width_mm() const = 0;
  virtual uint32_t height_mm() const = 0;

 protected:
  constexpr IMonitor() = default;
  ~IMonitor() = default;
};

}  // namespace hardware
