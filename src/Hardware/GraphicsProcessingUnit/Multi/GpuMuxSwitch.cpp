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

// The order of the steps follows Linux's vga_switcheroo.c (vga_switchto_stage1: power the new client on and switch DDC; stage2: the new
// client's mode set, the handler's switchto, the old client powered off) with the rollback this OS adds for every step.
#include "Hardware/GraphicsProcessingUnit/Multi/GpuMuxSwitch.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuManager.hpp"
#include "Hardware/Monitor/SystemMonitors.hpp"

namespace hardware {

const char* panel_light_status_name(PanelLightStatus status) {
  switch (status) {
    case PanelLightStatus::Ok: return "lit";
    case PanelLightStatus::NoPanel: return "no built-in panel output";
    case PanelLightStatus::NotSupported: return "cannot light the panel";
    case PanelLightStatus::Busy: return "the display drives another output";
    case PanelLightStatus::NoEdid: return "the panel sent no EDID";
    case PanelLightStatus::NoMode: return "no mode of the panel fits the output";
    case PanelLightStatus::PowerFailed: return "the panel's power could not be switched";
    case PanelLightStatus::LinkTraining: return "the panel's link did not train";
    case PanelLightStatus::ModeSet: return "the mode set failed";
  }
  return "?";
}

const char* gpu_mux_switch_status_name(GpuMuxSwitchStatus status) {
  switch (status) {
    case GpuMuxSwitchStatus::Switched: return "switched";
    case GpuMuxSwitchStatus::AlreadyActive: return "already there";
    case GpuMuxSwitchStatus::Busy: return "another switch is in progress";
    case GpuMuxSwitchStatus::NoOutput: return "no driver for that GPU's panel output";
    case GpuMuxSwitchStatus::NoPanelOutput: return "that GPU has no built-in panel output";
    case GpuMuxSwitchStatus::CannotLight: return "that GPU's driver cannot light the panel";
    case GpuMuxSwitchStatus::WakeFailed: return "that GPU would not wake";
    case GpuMuxSwitchStatus::LightFailed: return "that GPU's driver could not light the panel";
    case GpuMuxSwitchStatus::MuxFailed: return "the display switch would not move";
    case GpuMuxSwitchStatus::StateUnknown: return "where the panel is is not known";
  }
  return "?";
}

const char* gpu_mux_target_name(GpuMuxTarget target) { return target == GpuMuxTarget::Discrete ? "discrete GPU" : "integrated GPU"; }

bool GpuManagerLeases::acquire(uint32_t index) { return manager_.acquire(index); }
void GpuManagerLeases::release(uint32_t index) { manager_.release(index); }

namespace {
GpuMuxSwitch* g_published = nullptr;   // (atomic accesses only)
}  // namespace

void GpuMuxSwitch::publish(GpuMuxSwitch* system_switch) { __atomic_store_n(&g_published, system_switch, __ATOMIC_RELEASE); }
GpuMuxSwitch* GpuMuxSwitch::published() { return __atomic_load_n(&g_published, __ATOMIC_ACQUIRE); }

GpuMuxSwitch::~GpuMuxSwitch() {
  GpuMuxSwitch* self = this;
  (void)__atomic_compare_exchange_n(&g_published, &self, nullptr, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
  kernel::ScopedMutex g(lock_);
  give_back(GpuMuxTarget::Integrated);
  give_back(GpuMuxTarget::Discrete);
}

void GpuMuxSwitch::attach_output(GpuMuxTarget side, IPanelOutput* output) {
  kernel::ScopedMutex g(lock_);
  output_[slot(side)] = output;
}

bool GpuMuxSwitch::active(GpuMuxTarget* out) const {
  if (!__atomic_load_n(&known_, __ATOMIC_ACQUIRE)) return false;
  if (out) *out = static_cast<GpuMuxTarget>(__atomic_load_n(&active_, __ATOMIC_ACQUIRE));
  return true;
}

void GpuMuxSwitch::set_known_active(GpuMuxTarget target) {
  kernel::ScopedMutex g(lock_);
  __atomic_store_n(&active_, static_cast<uint8_t>(target), __ATOMIC_RELEASE);
  __atomic_store_n(&known_, true, __ATOMIC_RELEASE);
}

bool GpuMuxSwitch::holds_lease(GpuMuxTarget side) const {
  kernel::ScopedMutex g(lock_);
  return held_[slot(side)];
}

void GpuMuxSwitch::give_back(GpuMuxTarget side) {
  const uint32_t s = slot(side);
  if (!held_[s]) return;
  held_[s] = false;
  if (leases_ && index_[s] >= 0) leases_->release(static_cast<uint32_t>(index_[s]));
}

bool GpuMuxSwitch::preconditions(GpuMuxTarget target, GpuMuxTarget from, GpuMuxSwitchReport& r) const {
  r.from = from;
  r.to = target;
  if (!known_) {
    r.status = GpuMuxSwitchStatus::StateUnknown;
    r.reason = "an earlier switch could not be undone: where the panel is is not known";
    return false;
  }
  const IPanelOutput* to = output_[slot(target)];
  if (from == target && to && to->panel_lit()) {
    r.status = GpuMuxSwitchStatus::AlreadyActive;
    r.reason = "the panel is on that GPU already";
    return false;
  }
  if (!to) {
    r.status = GpuMuxSwitchStatus::NoOutput;
    r.reason = target == GpuMuxTarget::Discrete ? "the discrete GPU has no display driver that can drive the panel"
                                                : "the integrated GPU has no display driver registered for the panel";
    return false;
  }
  if (from != target && !output_[slot(from)]) {
    r.status = GpuMuxSwitchStatus::NoOutput;
    r.reason = "the GPU the panel is on now has no driver registered to let it go";
    return false;
  }
  if (!to->has_panel_output()) {
    r.status = GpuMuxSwitchStatus::NoPanelOutput;
    r.reason = "the target GPU's tables list no built-in panel output";
    return false;
  }
  if (!to->can_light_panel()) {
    r.status = GpuMuxSwitchStatus::CannotLight;
    r.reason = to->panel_status();
    return false;
  }
  return true;
}

bool GpuMuxSwitch::can_switch_to(GpuMuxTarget target, const char** why) const {
  if (!lock_.try_lock()) {
    if (why) *why = "another switch is in progress";
    return false;
  }
  GpuMuxSwitchReport r;
  const bool ok = preconditions(target, static_cast<GpuMuxTarget>(active_), r);
  lock_.unlock();
  if (why) *why = ok ? "" : r.reason;
  return ok;
}

GpuMuxSwitchReport GpuMuxSwitch::switch_to(GpuMuxTarget target) {
  if (!lock_.try_lock()) {
    GpuMuxSwitchReport r;
    r.status = GpuMuxSwitchStatus::Busy;
    r.reason = "another switch is in progress";
    r.to = target;
    return r;
  }
  __atomic_store_n(&in_flight_, true, __ATOMIC_RELEASE);
  const GpuMuxSwitchReport r = run(target);
  __atomic_store_n(&in_flight_, false, __ATOMIC_RELEASE);
  lock_.unlock();
  return r;
}

GpuMuxSwitchReport GpuMuxSwitch::run(GpuMuxTarget target) {
  GpuMuxSwitchReport r;
  // The mux's own answer wins over what the switcher remembers (ATPX and the NVIDIA _DSM cannot tell; gmux can), and settles where the panel
  // is after a rollback that failed.
  GpuMuxTarget from = static_cast<GpuMuxTarget>(active_);
  GpuMuxTarget told;
  if (mux_.active(&told)) {
    from = told;
    __atomic_store_n(&active_, static_cast<uint8_t>(told), __ATOMIC_RELEASE);
    __atomic_store_n(&known_, true, __ATOMIC_RELEASE);
  }
  if (!preconditions(target, from, r)) return r;
  IPanelOutput& to = *output_[slot(target)];
  IPanelOutput* old = from == target ? nullptr : output_[slot(from)];
  const uint32_t t = slot(target);

  // 2. The target GPU awake for as long as the panel is on it.
  bool took_lease = false;
  if (!held_[t] && leases_ && index_[t] >= 0) {
    if (!leases_->acquire(static_cast<uint32_t>(index_[t]))) {
      r.status = GpuMuxSwitchStatus::WakeFailed;
      r.reason = "the target GPU would not wake";
      return r;
    }
    took_lease = true;
    held_[t] = true;
    r.woke = true;
  }
  auto undo = [&](bool mux_moved) {
    r.rolled_back = true;
    if (mux_moved && !mux_.switch_to(from)) r.rollback_clean = false;
    to.release_panel();
    if (r.ddc_switched && !mux_.switch_ddc(from)) r.rollback_clean = false;
    if (took_lease) give_back(target);
    if (!r.rollback_clean) __atomic_store_n(&known_, false, __ATOMIC_RELEASE);
  };

  // 3. DDC first, where the mux can move it alone, so the target reads the panel's EDID.
  if (from != target) r.ddc_switched = mux_.switch_ddc(target);

  // 4. The target lights the panel (it does not reach the panel's pixels until the mux moves, but its power, AUX and EDID do).
  r.light = to.light_panel();
  if (r.light != PanelLightStatus::Ok) {
    undo(false);
    r.status = GpuMuxSwitchStatus::LightFailed;
    r.reason = to.panel_status();
    return r;
  }

  // 5. The display mux. (When the panel was already on this side, only unlit, there is nothing to move.)
  if (from != target) {
    if (!mux_.switch_to(target)) {
      undo(true);
      r.status = GpuMuxSwitchStatus::MuxFailed;
      r.reason = "the display switch would not move to the target GPU";
      return r;
    }
    r.mux_switched = true;
  }

  // 6. The old side lets the panel go and may sleep.
  if (old) old->release_panel();
  if (from != target) give_back(from);
  __atomic_store_n(&active_, static_cast<uint8_t>(target), __ATOMIC_RELEASE);
  __atomic_fetch_add(&switches_, 1u, __ATOMIC_RELAXED);
  r.status = GpuMuxSwitchStatus::Switched;
  r.reason = "the panel is on the target GPU";
  return r;
}

// --- GpuMuxDesktop ---------------------------------------------------------------------------------------------------------------------

IGraphicsProcessingUnit* GpuMuxDesktop::driver(GpuMuxTarget side) const {
  IGraphicsProcessingUnit* d = driver_[side == GpuMuxTarget::Discrete ? 1 : 0];
  const int32_t index = switch_.gpu_index(side);
  if (!d && index >= 0) d = monitors_.gpu_driver(static_cast<uint32_t>(index));
  return d;
}

const char* GpuMuxDesktop::gpu_name(GpuMuxTarget side) const {
  const IGraphicsProcessingUnit* d = driver(side);
  return d && d->name()[0] ? d->name() : gpu_mux_target_name(side);
}

bool GpuMuxDesktop::can_switch_to(GpuMuxTarget side, const char** why) const {
  const int32_t index = switch_.gpu_index(side);
  if (index < 0 || !driver(side)) {
    if (why) *why = "the desktop has no display driver for that GPU";
    return false;
  }
  return switch_.can_switch_to(side, why);
}

IBuiltInScreenSwitch::Result GpuMuxDesktop::switch_to(GpuMuxTarget side) {
  Result result;
  GpuMuxTarget from;
  if (!switch_.active(&from)) {
    result.rolled_back = false;
    result.reason = "where the built-in screen is is not known";
    return result;
  }
  const int32_t from_index = switch_.gpu_index(from), to_index = switch_.gpu_index(side);
  IGraphicsProcessingUnit* to_driver = driver(side);
  if (to_index < 0 || from_index < 0 || !to_driver) {
    result.reason = "the desktop has no display driver for that GPU";
    return result;
  }
  const GpuMuxSwitchReport r = switch_.switch_to(side);
  if (!r.ok()) {
    result.rolled_back = r.rollback_clean;
    result.reason = r.reason;
    return result;
  }
  if (r.status == GpuMuxSwitchStatus::AlreadyActive) {
    result.ok = true;
    return result;
  }
  if (!monitors_.move_panel(static_cast<uint32_t>(from_index), static_cast<uint32_t>(to_index), to_driver)) {
    const GpuMuxSwitchReport back = switch_.switch_to(from);   // (the desktop could not follow: the panel goes back)
    result.rolled_back = back.ok();
    result.reason = "the built-in screen did not appear among that GPU's monitors";
    return result;
  }
  result.ok = true;
  result.reason = "switched";
  return result;
}

}  // namespace hardware
