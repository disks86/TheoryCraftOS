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

// A monitor that was plugged in: its place is to the right of the others, unless that makes the desktop bigger than can be built.
bool SystemMonitors::add_to_layout(uint32_t id) {
  const Monitor* m = find(id);
  if (!m || layout_.count() >= MonitorLayout::kMaxMonitors) return false;
  uint64_t pixels = 0;
  for (uint32_t i = 0; i < layout_.count(); ++i)
    if (layout_.at(i).enabled) pixels += uint64_t{layout_.at(i).physical_width} * layout_.at(i).physical_height;
  const MonitorMode mode = m->current_mode();
  if (pixels + uint64_t{mode.width} * mode.height > kMaxDesktopPixels) return false;
  return layout_.add(id, mode.width, mode.height);
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
  if (layout_.count() == 1) {   // one display: it is shown at the size the screen has, whatever its EDID prefers
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

uint32_t SystemMonitors::physical_placements(ScanoutPlacement* out, uint32_t capacity) const {
  // Order: by place on the desktop, left to right and then top to bottom.
  uint32_t order[MonitorLayout::kMaxMonitors];
  uint32_t n = 0;
  for (uint32_t i = 0; i < layout_.count() && n < MonitorLayout::kMaxMonitors; ++i)
    if (layout_.at(i).enabled) order[n++] = i;
  for (uint32_t i = 1; i < n; ++i)
    for (uint32_t k = i; k > 0; --k) {
      const MonitorRect &a = layout_.at(order[k]).rect, &b = layout_.at(order[k - 1]).rect;
      if (!(a.x < b.x || (a.x == b.x && a.y < b.y))) break;
      const uint32_t t = order[k];
      order[k] = order[k - 1];
      order[k - 1] = t;
    }
  uint32_t x = 0, count = 0;
  for (uint32_t i = 0; i < n && count < capacity; ++i) {
    const MonitorSlot& s = layout_.at(order[i]);
    out[count++] = {s.id, x, 0, s.physical_width, s.physical_height};
    x += s.physical_width;
  }
  return count;
}

bool SystemMonitors::needs_compositing() const {
  ScanoutPlacement places[MonitorLayout::kMaxMonitors];
  const uint32_t n = physical_placements(places, MonitorLayout::kMaxMonitors);
  for (uint32_t i = 0; i < n; ++i) {
    const int32_t slot = layout_.find(places[i].connector);
    const MonitorSlot& s = layout_.at(static_cast<uint32_t>(slot));
    if (s.scale_percent != 100 || s.rect.x != static_cast<int32_t>(places[i].x) || s.rect.y != static_cast<int32_t>(places[i].y)) return true;
  }
  return false;
}

bool SystemMonitors::show() {
  if (!source_ || !multi_) return false;
  ScanoutPlacement places[IMonitorSource::kMaxConnectors];
  const uint32_t n = physical_placements(places, IMonitorSource::kMaxConnectors);
  uint32_t width = 0, height = 0;
  for (uint32_t i = 0; i < n; ++i) {
    width += places[i].width;
    if (places[i].height > height) height = places[i].height;
  }
  if (!source_->apply_layout(places, n, width, height)) return false;
  surface_width_ = width;
  surface_height_ = height;
  return true;
}

bool SystemMonitors::apply() {
  layout_.normalize();
  const MonitorRect box = layout_.bounds();
  desktop_width_ = box.width;
  desktop_height_ = box.height;
  if (!source_ || !multi_) {   // one display: the screen, switched to the size the monitor is to show
    const bool ok = sync_device_mode();
    surface_width_ = gpu_ ? gpu_->width() : 0;
    surface_height_ = gpu_ ? gpu_->height() : 0;
    applied_ = true;
    ++generation_;
    return ok;
  }
  applied_ = show();
  if (!applied_) {   // the device refused: it goes on showing one display, the size it has
    const MonitorSlot* keep = layout_.primary();
    const uint32_t keep_id = keep ? keep->id : 0;
    for (uint32_t i = layout_.count(); i > 0; --i)
      if (layout_.at(i - 1).id != keep_id) layout_.remove(layout_.at(i - 1).id);
    layout_.resize(keep_id, gpu_->width(), gpu_->height());
    multi_ = false;
    desktop_width_ = surface_width_ = gpu_->width();
    desktop_height_ = surface_height_ = gpu_->height();
  }
  ++generation_;
  return applied_;
}

// A device that shows one display changes the size of its screen when the monitor is to show another: done here, when the desktop is rebuilt
// (nothing draws then), not when the setting is chosen. False if the device would not.
bool SystemMonitors::sync_device_mode() {
  if (!gpu_ || !source_) return true;
  for (uint32_t i = 0; i < layout_.count(); ++i) {
    const MonitorSlot& s = layout_.at(i);
    if (!s.enabled) continue;
    if (s.physical_width == gpu_->width() && s.physical_height == gpu_->height()) return true;
    return source_->set_display_mode(s.id, s.physical_width, s.physical_height);
  }
  return true;
}

bool SystemMonitors::reapply() {
  if (!applied_ || !multi_) return false;
  layout_.normalize();
  const MonitorRect box = layout_.bounds();
  desktop_width_ = box.width;
  desktop_height_ = box.height;
  const bool ok = show();
  ++generation_;
  return ok;
}

bool SystemMonitors::show_single_display() {
  if (!source_ || !multi_ || !applied_) return false;
  const MonitorSlot* p = layout_.primary();
  if (!p) return false;
  const ScanoutPlacement one{p->id, 0, 0, p->physical_width, p->physical_height};
  const bool ok = source_->apply_layout(&one, 1, p->physical_width, p->physical_height);
  if (ok) {
    desktop_width_ = surface_width_ = p->physical_width;
    desktop_height_ = surface_height_ = p->physical_height;
  }
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
      if (info.connected && !add_to_layout(i)) ++waiting_;
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
        if (!add_to_layout(i)) ++waiting_;
      }
    } else {
      known->set_connected(false);
      if (slot >= 0) layout_.set_enabled(i, false);
      else if (waiting_) --waiting_;
    }
  }
  if (changed) {
    layout_.repair();   // (monitors that were held together by one that is gone are put in a row)
    reapply();
  }
  return changed;
}


// --- What a monitor shows ------------------------------------------------------------------------------

Monitor* SystemMonitors::find_mutable(uint32_t id) {
  for (uint64_t i = 0; i < monitors_.count(); ++i)
    if (monitors_[i].id() == id) return &monitors_[i];
  return nullptr;
}

uint32_t SystemMonitors::best_refresh(const Monitor& monitor, uint32_t width, uint32_t height) const {
  const EdidInfo* edid = monitor.edid();
  if (!edid) return 0;
  uint32_t best = 0;
  bool preferred = false;
  for (uint32_t i = 0; i < edid->timing_count; ++i) {
    const EdidTiming& t = edid->timings[i];
    if (t.width != width || t.height != height) continue;
    if (t.preferred) {
      best = t.refresh_millihz;
      preferred = true;
    } else if (!preferred && t.refresh_millihz > best) {
      best = t.refresh_millihz;
    }
  }
  return best;
}

uint32_t SystemMonitors::compatible_resolutions(uint32_t id, MonitorMode* out, uint32_t capacity) const {
  const Monitor* m = find(id);
  if (!m || !capacity) return 0;
  uint32_t n = 0;
  auto add = [&](uint32_t w, uint32_t h, bool preferred) {
    if (w < 640 || h < 480 || w > kMaxMonitorWidth || h > kMaxMonitorHeight) return;
    if (source_ && !source_->mode_available(id, w, h)) return;
    for (uint32_t k = 0; k < n; ++k)
      if (out[k].width == w && out[k].height == h) {
        out[k].preferred = out[k].preferred || preferred;
        return;
      }
    if (n >= capacity) return;
    out[n] = MonitorMode();
    out[n].width = w;
    out[n].height = h;
    out[n].refresh_millihz = best_refresh(*m, w, h);
    out[n].preferred = preferred;
    ++n;
  };
  const EdidInfo* edid = m->edid();
  if (edid && edid->timing_count) {
    for (uint32_t i = 0; i < edid->timing_count; ++i) add(edid->timings[i].width, edid->timings[i].height, edid->timings[i].preferred);
  } else if (source_) {
    MonitorMode device[32];
    const uint32_t count = source_->device_modes(id, device, 32);
    for (uint32_t i = 0; i < count; ++i) add(device[i].width, device[i].height, false);
  }
  const MonitorMode now = m->current_mode();
  if (now.width && now.height) {   // what runs now is, by being there, compatible
    bool there = false;
    for (uint32_t k = 0; k < n; ++k) there = there || (out[k].width == now.width && out[k].height == now.height);
    if (!there && n < capacity) {
      out[n] = now;
      out[n].preferred = false;
      ++n;
    }
  }
  // The native mode first, then the rest from the largest to the smallest (a stable insertion sort: the lists are short).
  for (uint32_t i = 1; i < n; ++i)
    for (uint32_t k = i; k > 0; --k) {
      const MonitorMode &a = out[k], &b = out[k - 1];
      const uint64_t pa = uint64_t{a.width} * a.height, pb = uint64_t{b.width} * b.height;
      const bool before = (a.preferred && !b.preferred) || (a.preferred == b.preferred && pa > pb);
      if (!before) break;
      const MonitorMode t = out[k];
      out[k] = out[k - 1];
      out[k - 1] = t;
    }
  return n;
}

uint32_t SystemMonitors::compatible_refresh_rates(uint32_t id, uint32_t width, uint32_t height, uint32_t* out, uint32_t capacity) const {
  const Monitor* m = find(id);
  if (!m || !capacity) return 0;
  const MonitorMode now = m->current_mode();
  if (!can_set_refresh(id)) {   // the device cannot change it: the rate that is in use (when it is the same size)
    out[0] = now.width == width && now.height == height ? now.refresh_millihz : best_refresh(*m, width, height);
    return 1;
  }
  uint32_t n = 0;
  const EdidInfo* edid = m->edid();
  for (uint32_t i = 0; edid && i < edid->timing_count; ++i) {
    const EdidTiming& t = edid->timings[i];
    if (t.width != width || t.height != height || n >= capacity) continue;
    bool seen = false;
    for (uint32_t k = 0; k < n; ++k) seen = seen || out[k] == t.refresh_millihz;
    if (!seen) out[n++] = t.refresh_millihz;
  }
  for (uint32_t i = 1; i < n; ++i)   // highest first
    for (uint32_t k = i; k > 0 && out[k] > out[k - 1]; --k) {
      const uint32_t t = out[k];
      out[k] = out[k - 1];
      out[k - 1] = t;
    }
  if (n == 0) out[n++] = 0;
  return n;
}

uint32_t SystemMonitors::compatible_scales(uint32_t id, uint32_t* out, uint32_t capacity, uint32_t width, uint32_t height) const {
  const int32_t slot = layout_.find(id);
  if (slot < 0) return 0;
  MonitorSlot s = layout_.at(static_cast<uint32_t>(slot));
  if (width && height) {
    s.physical_width = width;
    s.physical_height = height;
  }
  static const uint32_t kScales[] = {100, 125, 150, 175, 200, 250, 300, 400};
  uint32_t n = 0;
  for (uint32_t scale : kScales)
    if (n < capacity && MonitorLayout::logical_size(s.physical_width, scale) >= 640 && MonitorLayout::logical_size(s.physical_height, scale) >= 480) out[n++] = scale;
  if (n == 0 && capacity) out[n++] = 100;
  return n;
}

uint32_t SystemMonitors::scale_of(uint32_t id) const {
  const int32_t slot = layout_.find(id);
  return slot < 0 ? 100 : layout_.at(static_cast<uint32_t>(slot)).scale_percent;
}

bool SystemMonitors::set_resolution(uint32_t id, uint32_t width, uint32_t height) {
  Monitor* m = find_mutable(id);
  const int32_t slot = layout_.find(id);
  if (!m || slot < 0 || !source_) return false;
  MonitorMode modes[64];
  const uint32_t count = compatible_resolutions(id, modes, 64);
  bool ok = false;
  for (uint32_t i = 0; i < count; ++i) ok = ok || (modes[i].width == width && modes[i].height == height);
  if (!ok) return false;
  MonitorMode mode = m->current_mode();
  mode.width = width;
  mode.height = height;
  mode.refresh_millihz = best_refresh(*m, width, height);
  mode.preferred = false;
  EdidTiming native;
  if (m->edid() && m->edid()->best_timing(&native)) mode.preferred = native.width == width && native.height == height;
  m->set_current_mode(mode);
  // The scale in use must still leave a usable desktop: otherwise the largest scale that does.
  uint32_t scale = layout_.at(static_cast<uint32_t>(slot)).scale_percent;
  layout_.set_mode(id, width, height, 100);
  uint32_t scales[8];
  const uint32_t scale_count = compatible_scales(id, scales, 8);
  bool scale_ok = false;
  uint32_t fallback = 100;
  for (uint32_t i = 0; i < scale_count; ++i) {
    scale_ok = scale_ok || scales[i] == scale;
    if (scales[i] <= scale) fallback = scales[i];
  }
  layout_.set_mode(id, width, height, scale_ok ? scale : fallback);
  return true;
}

bool SystemMonitors::set_refresh(uint32_t id, uint32_t refresh_millihz) {
  Monitor* m = find_mutable(id);
  if (!m || !source_ || !source_->can_set_refresh(id)) return false;
  const MonitorMode now = m->current_mode();
  uint32_t rates[16];
  const uint32_t count = compatible_refresh_rates(id, now.width, now.height, rates, 16);
  bool ok = false;
  for (uint32_t i = 0; i < count; ++i) ok = ok || rates[i] == refresh_millihz;
  if (!ok || !source_->set_refresh(id, refresh_millihz)) return false;
  MonitorMode mode = now;
  mode.refresh_millihz = refresh_millihz;
  m->set_current_mode(mode);
  return true;
}

bool SystemMonitors::set_scale(uint32_t id, uint32_t scale_percent) {
  const int32_t slot = layout_.find(id);
  if (slot < 0) return false;
  uint32_t scales[8];
  const uint32_t count = compatible_scales(id, scales, 8);
  bool ok = false;
  for (uint32_t i = 0; i < count; ++i) ok = ok || scales[i] == scale_percent;
  if (!ok) return false;
  const MonitorSlot& s = layout_.at(static_cast<uint32_t>(slot));
  return layout_.set_mode(id, s.physical_width, s.physical_height, scale_percent);
}

bool SystemMonitors::move_monitor(uint32_t id, int32_t x, int32_t y) { return layout_.place_adjacent(id, x, y); }

void SystemMonitors::save(State* out) const {
  out->count = layout_.count();
  for (uint32_t i = 0; i < out->count; ++i) {
    out->slots[i] = layout_.at(i);
    const Monitor* m = find(layout_.at(i).id);
    out->modes[i] = m ? m->current_mode() : MonitorMode();
  }
}

bool SystemMonitors::restore(const State& state) {
  if (!state.valid() || !layout_.assign(state.slots, state.count)) return false;
  bool ok = true;
  for (uint32_t i = 0; i < state.count; ++i) {
    Monitor* m = find_mutable(state.slots[i].id);
    if (!m) continue;
    m->set_current_mode(state.modes[i]);
  }
  return ok;
}

}  // namespace hardware
