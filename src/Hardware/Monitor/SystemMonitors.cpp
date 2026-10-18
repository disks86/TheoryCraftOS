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

#include "Hardware/Monitor/SystemMonitors.hpp"

namespace hardware {

const Monitor* SystemMonitors::find(uint32_t id) const {
  for (uint64_t i = 0; i < monitors_.count(); ++i)
    if (monitors_[i].id() == id) return &monitors_[i];
  return nullptr;
}

uint32_t SystemMonitors::connected_count() const {
  uint32_t n = 0;
  for (uint64_t i = 0; i < monitors_.count(); ++i) n += monitors_[i].connected();
  return n;
}

// The size a monitor is shown at: its preferred mode, kept within what the surface can reasonably be.
MonitorMode SystemMonitors::mode_for(const Monitor& monitor) const {
  MonitorMode fallback;
  fallback.width = 1024;
  fallback.height = 768;
  MonitorMode m = monitor.default_mode(fallback);
  if (m.width == 0 || m.height == 0 || m.width > kMaxMonitorWidth || m.height > kMaxMonitorHeight) m = fallback;
  return m;
}

void SystemMonitors::add_connector(uint32_t index, const ConnectorInfo& info) {
  uint8_t edid[1024];
  const uint32_t length = info.connected && source_ ? source_->read_edid(index, ByteView(edid)) : 0;
  Monitor monitor;
  monitor.set(index, info, ConstByteView(edid, length), index + 1);
  monitor.set_current_mode(mode_for(monitor));
  monitors_.push_back(monitor);
}

void SystemMonitors::initialize() {
  monitors_.clear();
  layout_.clear();
  applied_ = false;
  multi_ = false;
  source_ = gpu_ ? gpu_->monitor_source() : nullptr;
  if (!gpu_) return;
  if (source_) {
    const uint32_t connectors = source_->connector_count() < IMonitorSource::kMaxConnectors ? source_->connector_count() : IMonitorSource::kMaxConnectors;
    for (uint32_t i = 0; i < connectors; ++i) {
      ConnectorInfo info;
      if (!source_->connector_info(i, &info)) continue;
      add_connector(i, info);
    }
    multi_ = source_->supports_multiple_displays();
  }
  if (monitors_.count() == 0) {   // a device with nothing to say: the screen is the one monitor
    Monitor monitor;
    monitor.set_unknown(0, gpu_->width(), gpu_->height());
    monitors_.push_back(monitor);
  }
  // The row: every monitor that is plugged in (only the first one on a device that cannot show several), the largest total that is built.
  uint64_t pixels = 0;
  for (uint64_t i = 0; i < monitors_.count(); ++i) {
    const Monitor& m = monitors_[i];
    if (!m.connected()) continue;
    if (!multi_ && layout_.count() >= 1) break;
    const MonitorMode mode = m.current_mode();
    pixels += uint64_t{mode.width} * mode.height;
    if (layout_.count() >= 1 && pixels > kMaxDesktopPixels) break;   // (no room for more)
    layout_.add(m.id(), mode.width, mode.height);
  }
  if (layout_.count() == 0 && monitors_.count()) {   // nothing reports as connected: show the first anyway
    const MonitorMode mode = monitors_[0].current_mode();
    layout_.add(monitors_[0].id(), mode.width ? mode.width : 1024, mode.height ? mode.height : 768);
  }
  if (!multi_ && layout_.count() == 1) {   // one display: it is shown at the size the screen has, whatever its EDID prefers
    const uint32_t id = layout_.at(0).id;
    layout_.resize(id, gpu_->width(), gpu_->height());
    for (uint64_t i = 0; i < monitors_.count(); ++i)
      if (monitors_[i].id() == id) {
        MonitorMode m = monitors_[i].current_mode();
        m.width = gpu_->width();
        m.height = gpu_->height();
        monitors_[i].set_current_mode(m);
      }
  }
  waiting_ = 0;
  ++generation_;
}

bool SystemMonitors::show() {
  if (!source_ || !multi_) return false;
  ScanoutPlacement places[IMonitorSource::kMaxConnectors];
  uint32_t n = 0;
  for (uint32_t i = 0; i < layout_.count() && n < IMonitorSource::kMaxConnectors; ++i) {
    const MonitorSlot& s = layout_.at(i);
    if (!s.enabled) continue;
    places[n++] = {s.id, static_cast<uint32_t>(s.rect.x), static_cast<uint32_t>(s.rect.y), s.rect.width, s.rect.height};
  }
  return source_->apply_layout(places, n, desktop_width_, desktop_height_);
}

bool SystemMonitors::apply() {
  layout_.normalize();
  if (!source_ || !multi_) {   // one display: the screen as it is
    desktop_width_ = gpu_ ? gpu_->width() : 0;
    desktop_height_ = gpu_ ? gpu_->height() : 0;
    applied_ = true;
    return false;
  }
  const MonitorRect box = layout_.bounds();
  desktop_width_ = box.width;
  desktop_height_ = box.height;
  applied_ = show();
  if (!applied_) {   // the device refused: it goes on showing one display, the size it has
    const MonitorSlot* keep = layout_.primary();
    const uint32_t keep_id = keep ? keep->id : 0;
    for (uint32_t i = layout_.count(); i > 0; --i)
      if (layout_.at(i - 1).id != keep_id) layout_.remove(layout_.at(i - 1).id);
    layout_.resize(keep_id, gpu_->width(), gpu_->height());
    multi_ = false;
    desktop_width_ = gpu_->width();
    desktop_height_ = gpu_->height();
  }
  ++generation_;
  return applied_;
}

bool SystemMonitors::show_single_display() {
  if (!source_ || !multi_ || !applied_) return false;
  const MonitorSlot* p = layout_.primary();
  if (!p) return false;
  const ScanoutPlacement one{p->id, 0, 0, p->rect.width, p->rect.height};
  const bool ok = source_->apply_layout(&one, 1, p->rect.width, p->rect.height);
  if (ok) {
    desktop_width_ = p->rect.width;
    desktop_height_ = p->rect.height;
  }
  return ok;
}

bool SystemMonitors::reapply() {
  if (!applied_ || !multi_) return false;
  const bool ok = show();
  ++generation_;
  return ok;
}

bool SystemMonitors::poll() {
  if (!source_ || !source_->refresh_connectors()) return false;
  bool changed = false;
  const uint32_t connectors = source_->connector_count() < IMonitorSource::kMaxConnectors ? source_->connector_count() : IMonitorSource::kMaxConnectors;
  for (uint32_t i = 0; i < connectors; ++i) {
    ConnectorInfo info;
    if (!source_->connector_info(i, &info)) continue;
    Monitor* known = nullptr;
    for (uint64_t k = 0; k < monitors_.count(); ++k)
      if (monitors_[k].id() == i) known = &monitors_[k];
    if (!known) {   // a connector that was not there before
      add_connector(i, info);
      if (info.connected) ++waiting_;
      changed = true;
      continue;
    }
    if (known->connected() == info.connected) continue;
    changed = true;
    const int32_t slot = layout_.find(i);
    if (info.connected) {   // plugged in: perhaps a different monitor than before
      uint8_t edid[1024];
      const uint32_t length = source_->read_edid(i, ByteView(edid));
      const MonitorMode kept = known->current_mode();
      known->set(i, info, ConstByteView(edid, length), i + 1);
      if (slot >= 0) {   // its place is still there: it shows at the size the place has
        MonitorMode m = kept;
        m.preferred = false;
        known->set_current_mode(m);
        layout_.set_enabled(i, true);
      } else {
        known->set_current_mode(mode_for(*known));
        ++waiting_;
      }
    } else {
      known->set_connected(false);
      if (slot >= 0) layout_.set_enabled(i, false);
      else if (waiting_) --waiting_;
    }
  }
  if (changed) {
    show();
    ++generation_;
  }
  return changed;
}

}  // namespace hardware
