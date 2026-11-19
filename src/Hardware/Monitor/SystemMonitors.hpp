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
//
// Several GPUs: the display device comes first (its monitors keep the ids and the order they have on a machine with one GPU), then every other GPU
// that has a display driver (add_gpu(), before initialize()): its monitors from its IMonitorSource, or its whole screen as one monitor when it has
// none. They go to the right of the display device's in the default row, and are arranged, scaled and given modes like any other; a mode change
// goes to that GPU's driver (IMonitorSource::set_display_mode() or IGraphicsProcessingUnit::set_mode()). Each GPU has a surface of its own (its
// monitors' pixels in a row, starting at 0): physical_placements() gives each monitor's place on its own GPU's surface, and targets() says which
// monitors are not on the display device, so that MonitorDisplay copies their part of the desktop to their GPU's screen. A monitor's id says which
// GPU it is on: the display device's are its connector numbers, the k-th other GPU's are k * kIdsPerGpu + connector.
//
// A muxed laptop's built-in panel moves between GPUs (Multi/GpuMuxSwitch): move_panel() then takes the panel's monitor off the GPU that let it go and
// puts it on the one that lit it, in the same place of the layout at the same scale (and primary if it was), so the next apply() shows it on the new
// GPU (its targets() entry, or none when it came back to the display device). A GPU added with a zero-sized screen (a discrete GPU waiting with
// nothing lit for the panel) shows none of its monitors until then.
#pragma once
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "Hardware/Monitor/Monitor.hpp"
#include "Hardware/Monitor/MonitorDisplay.hpp"
#include "Hardware/Monitor/MonitorLayout.hpp"
#include "ObjectContainer/List.hpp"

namespace hardware {

class SystemMonitors {
 public:
  // The largest desktop surface that is built, in pixels (a bigger arrangement is cut back to each monitor's smallest mode).
  static constexpr uint64_t kMaxDesktopPixels = 16u << 20;
  static constexpr uint32_t kMaxMonitorWidth = 3840, kMaxMonitorHeight = 2400;

  // How many GPUs the desktop can span (the display device and the others), and the id range each has.
  static constexpr uint32_t kMaxGpus = 8;
  static constexpr uint32_t kIdsPerGpu = IMonitorSource::kMaxConnectors;

  // `gpu` may be null (no monitors then). It must outlive this object.
  SystemMonitors(IGraphicsProcessingUnit* gpu, allocators::IAllocator& allocator) : gpu_(gpu), monitors_(allocator), layout_(allocator) {
    sources_[0].gpu = gpu;
  }

  // The display device's index in the machine's GPU list (GpuPool), recorded in its monitors (default 0).
  void set_display_gpu_index(uint32_t gpu_index) { sources_[0].gpu_index = gpu_index; }
  // Another GPU with a display driver whose monitors the desktop spans too (`gpu_index`: its index in GpuPool). Normally called before initialize();
  // one added after it has its monitors listed but none of them in the layout (move_panel() and poll() put them there). False if `gpu` is null or
  // the display device, or kMaxGpus are there. `gpu` must outlive this object.
  [[nodiscard]] bool add_gpu(IGraphicsProcessingUnit* gpu, uint32_t gpu_index);
  // The display device and the GPUs added.
  uint32_t gpu_count() const { return source_count_; }
  // The driver of the GPU with that index in GpuPool (the display device or one added), or null.
  IGraphicsProcessingUnit* gpu_driver(uint32_t gpu_index) const;

  // --- A muxed laptop's built-in panel moved from the GPU `from_gpu_index` to `to_gpu_index` (both GpuPool indices): its monitor goes from the old
  // GPU's monitors (marked not connected there, and not taken back by poll() while it is away) to the new GPU's (whose connectors are read again),
  // keeping its place in the layout, its scale and whether it is primary; the new GPU shows it at the size its screen has (a GPU that shows one
  // display) or the mode it had. `to_driver` adds the new GPU first when it is not on the desktop yet. Call apply() after. False (nothing changed)
  // when no monitor on the old GPU is the panel (an Internal connector, else the only one in use) or the new GPU lists none (an Internal connector
  // that is connected; on a GPU without Internal connectors the only connected one not in use, or its screen).
  [[nodiscard]] bool move_panel(uint32_t from_gpu_index, uint32_t to_gpu_index, IGraphicsProcessingUnit* to_driver = nullptr);
  // The id of the monitor move_panel() last put on a GPU (kNoMonitor before).
  static constexpr uint32_t kNoMonitor = 0xFFFFFFFFu;
  uint32_t panel_id() const { return panel_id_; }

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
  // The GPU that drives a monitor (its index in GpuPool), its driver and the driver's name; the display device's for an unknown id.
  uint32_t gpu_of(uint32_t id) const { return sources_[source_index(id)].gpu_index; }
  IGraphicsProcessingUnit* device_of(uint32_t id) const { return sources_[source_index(id)].gpu; }
  const char* device_name(uint32_t id) const;
  // True when some monitor in use is on another GPU than the display device.
  bool spans_gpus() const;
  // The monitors in use that another GPU drives (for MonitorDisplay::update(); no guards are filled in). Returns how many.
  uint32_t targets(MonitorTarget* out, uint32_t capacity) const;
  // The size of the surface of the GPU that drives monitor `id`, as last applied.
  uint32_t surface_width_of(uint32_t id) const { return source_index(id) == 0 ? surface_width_ : sources_[source_index(id)].surface_width; }
  uint32_t surface_height_of(uint32_t id) const { return source_index(id) == 0 ? surface_height_ : sources_[source_index(id)].surface_height; }

  // Makes the display device show the layout: the surface is built to hold every enabled monitor's pixels in a row, and each monitor shows its
  // part of it. The desktop (the layout's bounds) keeps its own size. False if the device cannot (it keeps showing what it showed, and only the
  // primary monitor is in use).
  bool apply();
  // Where each enabled monitor's pixels are on the surface: a row in the order of the monitors' places (left to right, then top to bottom), tops
  // aligned; one row per GPU, each on that GPU's own surface. `connector` is the monitor's id. Fills `out` and returns how many.
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
  bool can_set_refresh(uint32_t id) const;
  // The scales (percent) that leave the desktop of the monitor at least 640x480 (at its resolution, or at `width` x `height` when given): 100, 125, 150,
  // 175, 200, 250, 300, 400.
  uint32_t compatible_scales(uint32_t id, uint32_t* out, uint32_t capacity, uint32_t width = 0, uint32_t height = 0) const;
  uint32_t scale_of(uint32_t id) const;
  // HiDPI: the scale the desktop draws at, in percent: the largest scale of the enabled monitors (100 when all are at 100 %). The desktop's pictures
  // are the layout at this scale (MonitorDisplay maps them onto each monitor), and programs that can draw at it are drawn sharp.
  uint32_t render_percent() const;
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
  // Looks for monitors that came or went (on every GPU). True if anything changed; the layout is updated (and shown) to match.
  bool poll();
  // The size of the desktop - the layout's bounds, in the units the programs draw in - as last applied (0 before apply()).
  uint32_t desktop_width() const { return desktop_width_; }
  uint32_t desktop_height() const { return desktop_height_; }
  // Monitors that were plugged in after start-up and found no room (the desktop would be too big).
  uint32_t waiting_for_restart() const { return waiting_; }
  // Counts up whenever the set of monitors in use changes.
  uint32_t generation() const { return generation_; }

 private:
  // A GPU whose monitors are on the desktop: [0] is the display device (gpu_, source_ and multi_ are its), the others were added.
  struct Source {
    IGraphicsProcessingUnit* gpu = nullptr;
    IMonitorSource* source = nullptr;
    uint32_t gpu_index = 0;
    bool multi = false;
    uint32_t surface_width = 0, surface_height = 0;
    uint32_t released = kNoMonitor;   // the built-in panel's monitor while the panel is on another GPU (poll() leaves it alone)
  };

  uint32_t source_index(uint32_t id) const {
    const uint32_t s = id / kIdsPerGpu;
    return s < source_count_ ? s : 0;
  }
  static uint32_t connector_of(uint32_t id) { return id % kIdsPerGpu; }
  void add_connector(uint32_t source, uint32_t index, const ConnectorInfo& info);
  uint32_t fallback_number(uint32_t id) const;
  void enumerate(uint32_t source);
  bool poll_source(uint32_t source);
  Monitor* find_mutable(uint32_t id);
  bool add_to_layout(uint32_t id);
  uint32_t best_refresh(const Monitor& monitor, uint32_t width, uint32_t height) const;
  MonitorMode mode_for(const Monitor& monitor) const;
  bool show() { return show_source(0); }
  bool show_source(uint32_t source);
  bool show_secondaries();
  bool sync_device_mode(uint32_t source = 0);
  int32_t source_of_gpu(uint32_t gpu_index) const;
  bool source_has_monitors(uint32_t source) const;
  // Reads every connector of a GPU again into its monitors (a connector not seen before is added); the layout is not touched.
  void rescan(uint32_t source);

  IGraphicsProcessingUnit* gpu_;
  IMonitorSource* source_ = nullptr;
  Source sources_[kMaxGpus];
  uint32_t source_count_ = 1;
  List<Monitor> monitors_;
  MonitorLayout layout_;
  bool multi_ = false;
  bool applied_ = false;
  bool initialized_ = false;
  uint32_t panel_id_ = kNoMonitor;
  uint32_t desktop_width_ = 0, desktop_height_ = 0, surface_width_ = 0, surface_height_ = 0;
  uint32_t waiting_ = 0, generation_ = 0;
};

}  // namespace hardware
