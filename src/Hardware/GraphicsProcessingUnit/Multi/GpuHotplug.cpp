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

#include "Hardware/GraphicsProcessingUnit/Multi/GpuHotplug.hpp"

namespace hardware {

namespace {

// ACPI 6, 5.6.6: the generic notification values; ACPI video extensions (appendix B): the adapter's output values.
constexpr uint32_t kBusCheck = 0x00, kDeviceCheck = 0x01, kDeviceWake = 0x02;
constexpr uint32_t kVideoCycleOrStatus = 0x80, kVideoOutputChange = 0x81;

constexpr uint32_t bit(uint32_t index) { return 1u << index; }

}  // namespace

bool GpuHotplug::is_output_notification(GpuAcpiRole role, uint32_t value) {
  switch (role) {
    case GpuAcpiRole::Device: return value == kDeviceWake || value == kVideoCycleOrStatus || value == kVideoOutputChange;
    case GpuAcpiRole::Bridge: return value == kBusCheck || value == kDeviceCheck || value == kDeviceWake;
    case GpuAcpiRole::Output: return value == kVideoCycleOrStatus || value == kVideoOutputChange;
    case GpuAcpiRole::OtherAdapter: return value == kVideoOutputChange;
  }
  return false;
}

bool GpuHotplug::watch(uint32_t index) {
  if (index >= pool_.count() || index >= GpuTopology::kMaxAdapters) return false;
  kernel::ScopedMutex g(lock_);
  watched_ |= bit(index);
  return true;
}

bool GpuHotplug::watched(uint32_t index) const {
  kernel::ScopedMutex g(lock_);
  return index < GpuTopology::kMaxAdapters && (watched_ & bit(index)) != 0;
}

bool GpuHotplug::watch_node(uint32_t index, acpi::Node* node, GpuAcpiRole role) {
  if (!node || index >= pool_.count() || index >= GpuTopology::kMaxAdapters) return false;
  kernel::ScopedMutex g(lock_);
  for (uint32_t i = 0; i < watch_count_; ++i) {
    const Watch& w = watches_[i];
    if (w.node == node && w.gpu == index && w.role == role) return true;   // (already)
  }
  if (watch_count_ >= kMaxWatches) return false;
  watches_[watch_count_++] = Watch{node, index, role};
  watched_ |= bit(index);
  return true;
}

GpuAcpiNodes GpuHotplug::watch_acpi(uint32_t index, acpi::AmlInterpreter& aml, PciAddress gpu, const Optional<PciAddress>& bridge) {
  if (index >= pool_.count() || index >= GpuTopology::kMaxAdapters) return GpuAcpiNodes{};
  (void)watch(index);
  const GpuAcpiNodes nodes = GpuAcpiNamespace::find_gpu(aml, gpu, bridge);
  // The Devices below the GPU's are its outputs (_DOD / _ADR 0x0100 .. as the video extensions number them); the firmware may notify those.
  acpi::Node* outputs[kMaxOutputsPerGpu] = {};
  uint32_t output_count = 0;
  if (nodes.device) {
    kernel::ScopedRecursiveMutex guard(aml.mutex());
    for (acpi::Node* child = aml.first_child(nodes.device); child && output_count < kMaxOutputsPerGpu; child = aml.next_sibling(child))
      if (aml.is_device(child)) outputs[output_count++] = child;
  }
  (void)watch_node(index, nodes.device, GpuAcpiRole::Device);
  (void)watch_node(index, nodes.bridge, GpuAcpiRole::Bridge);
  for (uint32_t i = 0; i < output_count; ++i) (void)watch_node(index, outputs[i], GpuAcpiRole::Output);
  kernel::ScopedMutex g(lock_);
  nodes_[index] = nodes;
  return nodes;
}

GpuAcpiNodes GpuHotplug::acpi_nodes(uint32_t index) const {
  kernel::ScopedMutex g(lock_);
  return index < GpuTopology::kMaxAdapters ? nodes_[index] : GpuAcpiNodes{};
}

void GpuHotplug::on_notify(acpi::Node* device, uint32_t value) {
  if (!device) return;
  uint32_t marked = 0;
  {
    kernel::ScopedMutex g(lock_);
    for (uint32_t i = 0; i < watch_count_; ++i) {
      const Watch& w = watches_[i];
      if (w.node == device && is_output_notification(w.role, value)) marked |= bit(w.gpu);
    }
  }
  if (!marked) return;
  __atomic_fetch_or(&pending_, marked, __ATOMIC_ACQ_REL);
  count(&notifications_);
}

bool GpuHotplug::request_check(uint32_t index) {
  if (!watched(index)) return false;
  __atomic_fetch_or(&pending_, bit(index), __ATOMIC_ACQ_REL);
  return true;
}

void GpuHotplug::set_poll_policy(const GpuOutputPollPolicy& policy) {
  kernel::ScopedMutex g(lock_);
  policy_ = policy;
  if (policy_.period_ms < GpuOutputPollPolicy::kMinPeriodMs) policy_.period_ms = GpuOutputPollPolicy::kMinPeriodMs;
  if (policy_.min_battery_percent > 100) policy_.min_battery_percent = 100;
  for (bool& timed : poll_timed_) timed = false;   // (the period starts again from the next service())
}

GpuOutputPollPolicy GpuHotplug::poll_policy() const {
  kernel::ScopedMutex g(lock_);
  return policy_;
}

bool GpuHotplug::add_listener(IGpuOutputListener* listener) {
  if (!listener) return false;
  kernel::ScopedMutex g(listeners_lock_);
  IGpuOutputListener** free_slot = nullptr;
  for (IGpuOutputListener*& slot : listeners_) {
    if (slot == listener) return true;
    if (!slot && !free_slot) free_slot = &slot;
  }
  if (!free_slot) return false;
  *free_slot = listener;
  return true;
}

void GpuHotplug::remove_listener(IGpuOutputListener* listener) {
  {
    kernel::ScopedMutex g(listeners_lock_);
    for (IGpuOutputListener*& slot : listeners_)
      if (slot == listener) slot = nullptr;
  }
  // A dispatch that copied the list before the removal still runs under the dispatch lock: wait it out.
  kernel::ScopedMutex d(dispatch_lock_);
}

void GpuHotplug::dispatch(uint32_t index) {
  kernel::ScopedMutex d(dispatch_lock_);
  IGpuOutputListener* copy[kMaxListeners] = {};
  {
    kernel::ScopedMutex g(listeners_lock_);
    for (uint32_t i = 0; i < kMaxListeners; ++i) copy[i] = listeners_[i];
  }
  for (IGpuOutputListener* listener : copy)
    if (listener) listener->outputs_changed(index);
}

uint32_t GpuHotplug::service(uint64_t now_ms) {
  kernel::ScopedMutex pass(service_lock_);
  const uint32_t gpus = pool_.count() < GpuTopology::kMaxAdapters ? pool_.count() : GpuTopology::kMaxAdapters;
  const uint32_t notified = __atomic_exchange_n(&pending_, 0u, __ATOMIC_ACQ_REL);

  // Which GPUs are due for a poll (the readable ones only: polling a GPU nothing can look at would wake it for nothing).
  uint32_t due = 0;
  GpuOutputPollPolicy policy;
  {
    kernel::ScopedMutex g(lock_);
    policy = policy_;
    if (now_ms < last_now_ms_)
      for (bool& timed : poll_timed_) timed = false;   // (a clock that went back: start again)
    last_now_ms_ = now_ms;
    for (uint32_t i = 0; i < gpus; ++i) {
      if (!(watched_ & bit(i))) continue;
      if (notified & bit(i)) {   // (a check now: the next poll is a period away)
        next_poll_ms_[i] = now_ms + policy.period_ms;
        poll_timed_[i] = true;
        continue;
      }
      if (!policy.enabled) continue;
      if (!poll_timed_[i]) {
        next_poll_ms_[i] = now_ms + policy.period_ms;
        poll_timed_[i] = true;
        continue;
      }
      if (now_ms < next_poll_ms_[i]) continue;
      next_poll_ms_[i] = now_ms + policy.period_ms;
      due |= bit(i);
    }
  }

  const GpuManager::PowerStatus power = pool_.manager().power_status();
  const bool low_battery = power.source == PowerSource::Battery && (power.battery_percent < policy.min_battery_percent || power.battery_saver);
  uint32_t reported = 0;
  for (uint32_t i = 0; i < gpus; ++i) {
    const bool was_notified = (notified & bit(i)) != 0;
    if (!was_notified && !(due & bit(i))) continue;
    if (!was_notified) {
      if (!pool_.outputs_readable(i)) continue;
      if (low_battery && pool_.manager().asleep(i)) {   // (an awake GPU costs nothing to look at)
        count(&skipped_);
        continue;
      }
    }
    count(&checks_);
    const GpuOutputCheck check = pool_.refresh_outputs(i);
    if (check == GpuOutputCheck::NoWake) count(&wake_failures_);
    // A notification nothing here can verify is passed on: the listeners look themselves (the monitor poll, the codec's pins).
    if (check == GpuOutputCheck::Changed || (was_notified && check == GpuOutputCheck::CannotTell)) {
      count(&changes_);
      ++reported;
      dispatch(i);
    }
  }
  return reported;
}

}  // namespace hardware
