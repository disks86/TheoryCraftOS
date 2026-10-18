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

#include "Hardware/Monitor/Monitor.hpp"

namespace hardware {

const char* connector_type_name(ConnectorType type) {
  switch (type) {
    case ConnectorType::Vga: return "VGA";
    case ConnectorType::Dvi: return "DVI";
    case ConnectorType::Hdmi: return "HDMI";
    case ConnectorType::DisplayPort: return "DisplayPort";
    case ConnectorType::Internal: return "Internal";
    case ConnectorType::Composite: return "Composite";
    case ConnectorType::Component: return "Component";
    case ConnectorType::Virtual: return "Virtual";
    default: return "Unknown";
  }
}

namespace {
void copy_text(char* out, uint32_t capacity, const char* text) {
  uint32_t n = 0;
  for (; text[n] && n + 1 < capacity; ++n) out[n] = text[n];
  out[n] = 0;
}

void append_number(char* out, uint32_t capacity, uint32_t value) {
  uint32_t n = 0;
  while (out[n]) ++n;
  char digits[10];
  uint32_t d = 0;
  do digits[d++] = static_cast<char>('0' + value % 10); while (value /= 10);
  while (d && n + 1 < capacity) out[n++] = digits[--d];
  out[n] = 0;
}
}  // namespace

void Monitor::set(uint32_t id, const ConnectorInfo& connector, ConstByteView edid_bytes, uint32_t fallback_number) {
  id_ = id;
  type_ = connector.type;
  connected_ = connector.connected;
  copy_text(connector_, sizeof connector_, connector.name);
  reported_width_ = connector.preferred_width;
  reported_height_ = connector.preferred_height;
  has_edid_ = !edid_bytes.empty() && parse_edid(edid_bytes, &edid_);
  mode_count_ = 0;
  if (has_edid_) {
    edid_display_name(edid_, name_, sizeof name_);
    for (uint32_t i = 0; i < edid_.timing_count && mode_count_ < kMaxModes; ++i) {
      const EdidTiming& t = edid_.timings[i];
      MonitorMode& m = modes_[mode_count_++];
      m.width = t.width;
      m.height = t.height;
      m.refresh_millihz = t.refresh_millihz;
      m.interlaced = t.interlaced;
      m.preferred = t.preferred;
    }
  } else {
    copy_text(name_, sizeof name_, "Display ");
    append_number(name_, sizeof name_, fallback_number);
  }
}

void Monitor::set_unknown(uint32_t id, uint32_t width, uint32_t height) {
  id_ = id;
  connected_ = true;
  has_edid_ = false;
  type_ = ConnectorType::Unknown;
  connector_[0] = 0;
  copy_text(name_, sizeof name_, "Display");
  reported_width_ = width;
  reported_height_ = height;
  current_.width = width;
  current_.height = height;
  current_.preferred = true;
  modes_[0] = current_;
  mode_count_ = 1;
}

bool Monitor::mode_info(uint32_t index, MonitorMode* out) const {
  if (index >= mode_count_ || !out) return false;
  *out = modes_[index];
  return true;
}

MonitorMode Monitor::default_mode(const MonitorMode& fallback) const {
  EdidTiming t;
  if (has_edid_ && edid_.best_timing(&t)) {
    MonitorMode m;
    m.width = t.width;
    m.height = t.height;
    m.refresh_millihz = t.refresh_millihz;
    m.interlaced = t.interlaced;
    m.preferred = true;
    return m;
  }
  if (reported_width_ && reported_height_) {
    MonitorMode m;
    m.width = reported_width_;
    m.height = reported_height_;
    m.preferred = true;
    return m;
  }
  return fallback;
}

}  // namespace hardware
