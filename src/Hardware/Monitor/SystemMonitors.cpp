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

bool SystemMonitors::add_gpu(IGraphicsProcessingUnit* gpu, uint32_t gpu_index) {
  if (!gpu || gpu == gpu_ || source_count_ >= kMaxGpus) return false;
  for (uint32_t s = 1; s < source_count_; ++s)
    if (sources_[s].gpu == gpu) return false;
  Source& added = sources_[source_count_++];
  added = Source();
  added.gpu = gpu;
  added.gpu_index = gpu_index;
  return true;
}

const char* SystemMonitors::device_name(uint32_t id) const {
  const IGraphicsProcessingUnit* g = device_of(id);
  return g ? g->name() : "";
}

bool SystemMonitors::spans_gpus() const {
  for (uint32_t i = 0; i < layout_.count(); ++i)
    if (layout_.at(i).enabled && source_index(layout_.at(i).id) != 0) return true;
  return false;
}

uint32_t SystemMonitors::targets(MonitorTarget* out, uint32_t capacity) const {
  uint32_t n = 0;
  for (uint32_t i = 0; i < layout_.count() && n < capacity; ++i) {
    const MonitorSlot& slot = layout_.at(i);
    const uint32_t s = source_index(slot.id);
    if (!slot.enabled || s == 0) continue;
    out[n] = MonitorTarget();
    out[n].id = slot.id;
    out[n].gpu = sources_[s].gpu;
    out[n].gpu_index = sources_[s].gpu_index;
    ++n;
  }
  return n;
}

// "Display N" for a monitor that does not say its name: the display device's are numbered by connector, the other GPUs' after them.
uint32_t SystemMonitors::fallback_number(uint32_t id) const {
  const uint32_t source = source_index(id);
  uint32_t before = 0;
  for (uint64_t i = 0; i < monitors_.count(); ++i)
    if (source_index(monitors_[i].id()) < source) ++before;
  return before + connector_of(id) + 1;
}

void SystemMonitors::add_connector(uint32_t source, uint32_t index, const ConnectorInfo& info) {
  IMonitorSource* from = sources_[source].source;
  uint8_t edid[1024];
  const uint32_t length = info.connected && from ? from->read_edid(index, ByteView(edid)) : 0;
  const uint32_t id = source * kIdsPerGpu + index;
  Monitor monitor;
  monitor.set(id, info, ConstByteView(edid, length), fallback_number(id));
  monitor.set_current_mode(mode_for(monitor));
  monitor.set_gpu(sources_[source].gpu_index);
  monitors_.push_back(monitor);
}

// The monitors of one GPU: the connectors its monitor source lists, or the screen as the one monitor when it tells nothing.
void SystemMonitors::enumerate(uint32_t source) {
  Source& src = sources_[source];
  src.source = src.gpu ? src.gpu->monitor_source() : nullptr;
  src.multi = false;
  if (!src.gpu) return;
  const uint64_t before = monitors_.count();
  if (src.source) {
    const uint32_t connectors = src.source->connector_count() < IMonitorSource::kMaxConnectors ? src.source->connector_count() : IMonitorSource::kMaxConnectors;
    for (uint32_t i = 0; i < connectors; ++i) {
      ConnectorInfo info;
      if (!src.source->connector_info(i, &info)) continue;
      add_connector(source, i, info);
    }
    src.multi = src.source->supports_multiple_displays();
  }
  if (monitors_.count() == before) {   // a device with nothing to say: the screen is the one monitor
    Monitor monitor;
    monitor.set_unknown(source * kIdsPerGpu, src.gpu->width(), src.gpu->height());
    monitor.set_gpu(src.gpu_index);
    monitors_.push_back(monitor);
  }
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
  sources_[0].gpu = gpu_;
  enumerate(0);
  source_ = sources_[0].source;
  multi_ = sources_[0].multi;
  if (!gpu_) return;
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
    pixels += uint64_t{mode.width ? mode.width : 1024} * (mode.height ? mode.height : 768);
    layout_.add(monitors_[0].id(), mode.width ? mode.width : 1024, mode.height ? mode.height : 768);
  }
  // The other GPUs' monitors, to the right, GPU after GPU (while the desktop has room).
  for (uint32_t s = 1; s < source_count_; ++s) {
    const uint64_t first = monitors_.count();
    enumerate(s);
    uint32_t added = 0;
    for (uint64_t i = first; i < monitors_.count(); ++i) {
      const Monitor& m = monitors_[i];
      if (!m.connected()) continue;
      if (!sources_[s].multi && added >= 1) break;
      const MonitorMode mode = m.current_mode();
      if (pixels + uint64_t{mode.width} * mode.height > kMaxDesktopPixels || layout_.count() >= MonitorLayout::kMaxMonitors) break;
      pixels += uint64_t{mode.width} * mode.height;
      if (layout_.add(m.id(), mode.width, mode.height)) ++added;
    }
  }
  // A GPU that shows one display: it is shown at the size the screen has, whatever its EDID prefers.
  for (uint32_t s = 0; s < source_count_; ++s) {
    uint32_t count = 0, id = 0;
    for (uint32_t i = 0; i < layout_.count(); ++i)
      if (source_index(layout_.at(i).id) == s) {
        ++count;
        id = layout_.at(i).id;
      }
    if (count != 1) continue;
    const uint32_t width = sources_[s].gpu->width(), height = sources_[s].gpu->height();
    layout_.resize(id, width, height);
    if (Monitor* m = find_mutable(id)) {
      MonitorMode mode = m->current_mode();
      mode.width = width;
      mode.height = height;
      m->set_current_mode(mode);
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
  uint32_t x[kMaxGpus] = {}, count = 0;   // (each GPU's row on its own surface)
  for (uint32_t i = 0; i < n && count < capacity; ++i) {
    const MonitorSlot& s = layout_.at(order[i]);
    uint32_t& at = x[source_index(s.id)];
    out[count++] = {s.id, at, 0, s.physical_width, s.physical_height};
    at += s.physical_width;
  }
  return count;
}

uint32_t SystemMonitors::render_percent() const {
  uint32_t p = 100;
  for (uint32_t i = 0; i < layout_.count(); ++i) {
    const MonitorSlot& s = layout_.at(i);
    if (s.enabled && s.scale_percent > p) p = s.scale_percent;
  }
  return p > 400 ? 400 : p;
}

bool SystemMonitors::needs_compositing() const {
  if (spans_gpus()) return true;   // (the other GPUs' monitors are copied to their screens)
  ScanoutPlacement places[MonitorLayout::kMaxMonitors];
  const uint32_t n = physical_placements(places, MonitorLayout::kMaxMonitors);
  for (uint32_t i = 0; i < n; ++i) {
    const int32_t slot = layout_.find(places[i].connector);
    const MonitorSlot& s = layout_.at(static_cast<uint32_t>(slot));
    if (s.scale_percent != 100 || s.rect.x != static_cast<int32_t>(places[i].x) || s.rect.y != static_cast<int32_t>(places[i].y)) return true;
  }
  return false;
}

// Shows one GPU's monitors: its surface is made to hold their pixels in a row (from physical_placements()), and each shows its part.
bool SystemMonitors::show_source(uint32_t source) {
  Source& src = sources_[source];
  if (!src.source || !src.multi) return false;
  ScanoutPlacement all[MonitorLayout::kMaxMonitors], places[IMonitorSource::kMaxConnectors];
  const uint32_t count = physical_placements(all, MonitorLayout::kMaxMonitors);
  uint32_t n = 0, width = 0, height = 0;
  for (uint32_t i = 0; i < count && n < IMonitorSource::kMaxConnectors; ++i) {
    if (source_index(all[i].connector) != source) continue;
    places[n] = all[i];
    places[n].connector = connector_of(all[i].connector);
    width += places[n].width;
    if (places[n].height > height) height = places[n].height;
    ++n;
  }
  if (!src.source->apply_layout(places, n, width, height)) return false;
  src.surface_width = width;
  src.surface_height = height;
  if (source == 0) {
    surface_width_ = width;
    surface_height_ = height;
  }
  return true;
}

// The other GPUs show their monitors: several through their layout, one by switching the screen to its size. A GPU that refuses a layout goes on
// showing one display (its first monitor, at the size its screen has). False if any refused.
bool SystemMonitors::show_secondaries() {
  bool ok = true;
  for (uint32_t s = 1; s < source_count_; ++s) {
    Source& src = sources_[s];
    if (src.multi) {
      if (show_source(s)) continue;
      ok = false;
      uint32_t keep = 0;
      bool found = false;
      for (uint32_t i = layout_.count(); i > 0; --i) {   // (the first of its monitors stays)
        const uint32_t id = layout_.at(i - 1).id;
        if (source_index(id) != s) continue;
        if (found) layout_.remove(keep);
        keep = id;
        found = true;
      }
      src.multi = false;
      if (found) layout_.resize(keep, src.gpu->width(), src.gpu->height());
    } else if (!sync_device_mode(s)) {
      ok = false;
    }
    src.surface_width = src.gpu->width();
    src.surface_height = src.gpu->height();
  }
  return ok;
}

bool SystemMonitors::apply() {
  layout_.normalize();
  const MonitorRect box = layout_.bounds();
  desktop_width_ = box.width;
  desktop_height_ = box.height;
  bool ok;
  if (!source_ || !multi_) {   // one display: the screen, switched to the size the monitor is to show
    ok = sync_device_mode();
    surface_width_ = gpu_ ? gpu_->width() : 0;
    surface_height_ = gpu_ ? gpu_->height() : 0;
    applied_ = true;
  } else {
    applied_ = show();
    ok = applied_;
    if (!applied_) {   // the device refused: it goes on showing one display, the size it has
      const MonitorSlot* keep = layout_.primary();
      uint32_t keep_id = keep ? keep->id : 0;
      if (source_index(keep_id) != 0)   // (the primary is on another GPU: the display device keeps its first monitor)
        for (uint32_t i = 0; i < layout_.count(); ++i)
          if (source_index(layout_.at(i).id) == 0) {
            keep_id = layout_.at(i).id;
            break;
          }
      for (uint32_t i = layout_.count(); i > 0; --i)
        if (layout_.at(i - 1).id != keep_id && source_index(layout_.at(i - 1).id) == 0) layout_.remove(layout_.at(i - 1).id);
      layout_.resize(keep_id, gpu_->width(), gpu_->height());
      multi_ = false;
      sources_[0].multi = false;
      surface_width_ = gpu_->width();
      surface_height_ = gpu_->height();
      if (source_count_ == 1) {
        desktop_width_ = surface_width_;
        desktop_height_ = surface_height_;
      }
    }
  }
  if (source_count_ > 1) {
    if (!show_secondaries()) ok = false;
    layout_.repair();
    layout_.normalize();
    const MonitorRect all = layout_.bounds();
    desktop_width_ = all.width;
    desktop_height_ = all.height;
  }
  ++generation_;
  return ok;
}

// A device that shows one display changes the size of its screen when the monitor is to show another: done here, when the desktop is rebuilt
// (nothing draws then), not when the setting is chosen. False if the device would not.
bool SystemMonitors::sync_device_mode(uint32_t source) {
  const Source& src = sources_[source];
  if (!src.gpu || (source == 0 && !src.source)) return true;
  for (uint32_t i = 0; i < layout_.count(); ++i) {
    const MonitorSlot& s = layout_.at(i);
    if (!s.enabled || source_index(s.id) != source) continue;
    if (s.physical_width == src.gpu->width() && s.physical_height == src.gpu->height()) return true;
    if (src.source) return src.source->set_display_mode(connector_of(s.id), s.physical_width, s.physical_height);
    for (uint32_t m = 0; m < src.gpu->mode_count(); ++m) {   // (another GPU that tells nothing about monitors: one of its own modes)
      DisplayMode mode;
      if (src.gpu->mode_info(m, &mode) && mode.width == s.physical_width && mode.height == s.physical_height) return src.gpu->set_mode(m);
    }
    return false;
  }
  return true;
}

bool SystemMonitors::reapply() {
  if (!applied_ || (!multi_ && source_count_ == 1)) return false;
  layout_.normalize();
  const MonitorRect box = layout_.bounds();
  desktop_width_ = box.width;
  desktop_height_ = box.height;
  bool ok = !multi_ || show();
  for (uint32_t s = 1; s < source_count_; ++s)
    if (sources_[s].multi && !show_source(s)) ok = false;
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
  bool changed = false;
  for (uint32_t s = 0; s < source_count_; ++s)
    if (poll_source(s)) changed = true;
  if (changed) {
    layout_.repair();   // (monitors that were held together by one that is gone are put in a row)
    reapply();
  }
  return changed;
}

bool SystemMonitors::poll_source(uint32_t source) {
  IMonitorSource* from = sources_[source].source;
  if (!from || !from->refresh_connectors()) return false;
  bool changed = false;
  const uint32_t connectors = from->connector_count() < IMonitorSource::kMaxConnectors ? from->connector_count() : IMonitorSource::kMaxConnectors;
  for (uint32_t i = 0; i < connectors; ++i) {
    ConnectorInfo info;
    if (!from->connector_info(i, &info)) continue;
    const uint32_t id = source * kIdsPerGpu + i;
    Monitor* known = find_mutable(id);
    if (!known) {   // a connector that was not there before
      add_connector(source, i, info);
      if (info.connected && !add_to_layout(id)) ++waiting_;
      changed = true;
      continue;
    }
    if (known->connected() == info.connected) continue;
    changed = true;
    const int32_t slot = layout_.find(id);
    if (info.connected) {   // plugged in: perhaps a different monitor than before
      uint8_t edid[1024];
      const uint32_t length = from->read_edid(i, ByteView(edid));
      const MonitorMode kept = known->current_mode();
      known->set(id, info, ConstByteView(edid, length), fallback_number(id));
      if (slot >= 0) {   // its place is still there: it shows at the size the place has
        MonitorMode m = kept;
        m.preferred = false;
        known->set_current_mode(m);
        layout_.set_enabled(id, true);
      } else {
        known->set_current_mode(mode_for(*known));
        if (!add_to_layout(id)) ++waiting_;
      }
    } else {
      known->set_connected(false);
      if (slot >= 0) layout_.set_enabled(id, false);
      else if (waiting_) --waiting_;
    }
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
  const Source& src = sources_[source_index(id)];
  const uint32_t connector = connector_of(id);
  uint32_t n = 0;
  auto add = [&](uint32_t w, uint32_t h, bool preferred) {
    if (w < 640 || h < 480 || w > kMaxMonitorWidth || h > kMaxMonitorHeight) return;
    if (src.source && !src.source->mode_available(connector, w, h)) return;
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
  } else if (src.source) {
    MonitorMode device[32];
    const uint32_t count = src.source->device_modes(connector, device, 32);
    for (uint32_t i = 0; i < count; ++i) add(device[i].width, device[i].height, false);
  } else if (source_index(id) != 0 && src.gpu) {   // another GPU that tells nothing about monitors: the modes of its driver
    for (uint32_t i = 0; i < src.gpu->mode_count(); ++i) {
      DisplayMode mode;
      if (src.gpu->mode_info(i, &mode)) add(mode.width, mode.height, false);
    }
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

bool SystemMonitors::can_set_refresh(uint32_t id) const {
  const Source& src = sources_[source_index(id)];
  return src.source && src.source->can_set_refresh(connector_of(id));
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
  if (!m || slot < 0 || (!sources_[source_index(id)].source && source_index(id) == 0)) return false;
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
  IMonitorSource* from = sources_[source_index(id)].source;
  if (!m || !from || !from->can_set_refresh(connector_of(id))) return false;
  const MonitorMode now = m->current_mode();
  uint32_t rates[16];
  const uint32_t count = compatible_refresh_rates(id, now.width, now.height, rates, 16);
  bool ok = false;
  for (uint32_t i = 0; i < count; ++i) ok = ok || rates[i] == refresh_millihz;
  if (!ok || !from->set_refresh(connector_of(id), refresh_millihz)) return false;
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
