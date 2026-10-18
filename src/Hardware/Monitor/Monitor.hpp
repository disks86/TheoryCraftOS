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

// The monitor the system found behind a connector: built from the connector's information and its EDID.
#pragma once
#include "Hardware/Monitor/IMonitor.hpp"
#include "Hardware/Monitor/IMonitorSource.hpp"

namespace hardware {

class Monitor final : public IMonitor {
 public:
  static constexpr uint32_t kMaxModes = EdidInfo::kMaxTimings;

  Monitor() = default;
  // Takes the connector's description and the EDID bytes (may be empty). `fallback_number` names a monitor that does not say ("Display 2").
  void set(uint32_t id, const ConnectorInfo& connector, ConstByteView edid_bytes, uint32_t fallback_number);
  void set_connected(bool connected) { connected_ = connected; }
  // The mode the monitor is shown at (the desktop layout decides).
  void set_current_mode(const MonitorMode& mode) { current_ = mode; }
  // A monitor with no information of its own (no connector list: the whole screen is one display).
  void set_unknown(uint32_t id, uint32_t width, uint32_t height);

  uint32_t id() const override { return id_; }
  const char* name() const override { return name_; }
  ConnectorType connector_type() const override { return type_; }
  const char* connector_name() const override { return connector_; }
  bool connected() const override { return connected_; }
  const EdidInfo* edid() const override { return has_edid_ ? &edid_ : nullptr; }
  uint32_t mode_count() const override { return mode_count_; }
  bool mode_info(uint32_t index, MonitorMode* out) const override;
  MonitorMode current_mode() const override { return current_; }
  uint32_t width_mm() const override { return has_edid_ ? edid_.width_mm : 0; }
  uint32_t height_mm() const override { return has_edid_ ? edid_.height_mm : 0; }

  // The mode to use when nothing else is asked for: the EDID's preferred (native) one, else the size the connector reported, else `fallback`.
  MonitorMode default_mode(const MonitorMode& fallback) const;

 private:
  uint32_t id_ = 0;
  char name_[64] = "Display";
  char connector_[16] = {};
  ConnectorType type_ = ConnectorType::Unknown;
  bool connected_ = false;
  bool has_edid_ = false;
  EdidInfo edid_;
  MonitorMode modes_[kMaxModes];
  uint32_t mode_count_ = 0;
  MonitorMode current_;
  uint32_t reported_width_ = 0, reported_height_ = 0;
};

}  // namespace hardware
