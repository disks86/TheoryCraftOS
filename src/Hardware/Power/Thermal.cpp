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

#include "Hardware/Power/Thermal.hpp"
#include "Hardware/CentralProcessingUnit/CpuTemperature.hpp"

namespace hardware {

bool CpuThermalSource::read(int32_t* millicelsius) {
  const int32_t t = cpu_temperature_millicelsius(pci_);
  if (t < 0) return false;
  *millicelsius = t;
  return true;
}

// ---- ACPI -----------------------------------------------------------------------------------------------------------------------------------------------------------------------

namespace {
acpi::Node* child(acpi::AmlInterpreter& vm, acpi::Node* parent, const char* name) {
  for (acpi::Node* c = vm.first_child(parent); c; c = vm.next_sibling(c)) {
    const char* n = vm.name_of(c);
    if (n && n[0] == name[0] && n[1] == name[1] && n[2] == name[2] && n[3] == name[3]) return c;
  }
  return nullptr;
}

void walk(acpi::AmlInterpreter& vm, acpi::Node* node, uint32_t depth, acpi::Node** out, uint32_t capacity, uint32_t* count) {
  if (depth > 10) return;
  for (acpi::Node* c = vm.first_child(node); c && *count < capacity; c = vm.next_sibling(c)) {
    if (vm.is_thermal_zone(c) && child(vm, c, "_TMP")) out[(*count)++] = c;
    walk(vm, c, depth + 1, out, capacity, count);
  }
}
}  // namespace

uint32_t AcpiThermalZone::find_zones(acpi::AmlInterpreter& vm, acpi::Node** out, uint32_t capacity) {
  uint32_t count = 0;
  walk(vm, vm.root(), 0, out, capacity, &count);
  return count;
}

bool AcpiThermalZone::temperature_of(const char* method, int32_t* millicelsius) {
  kernel::ScopedRecursiveMutex guard(vm_.mutex());
  acpi::Node* n = child(vm_, zone_, method);
  uint64_t tenths;
  if (!n || !vm_.evaluate_integer(n, &tenths) || tenths < 2732 || tenths > 5000) return false;     // (0 K to 227 C: what is not a temperature is not one)
  *millicelsius = static_cast<int32_t>((tenths - 2731) * 100);
  return true;
}

bool AcpiThermalZone::read(int32_t* millicelsius) { return temperature_of("_TMP", millicelsius); }

ThermalLimits AcpiThermalZone::limits() {
  ThermalLimits l;
  if (!temperature_of("_PSV", &l.passive_mc)) l.passive_mc = -1;
  if (!temperature_of("_CRT", &l.critical_mc)) l.critical_mc = -1;
  return l;
}

// ---- the manager -----------------------------------------------------------------------------------------------------------------------------------------------------------------

bool ThermalManager::add(IThermalSource* source) {
  if (!source || count_ == kMaxSources) return false;
  sources_[count_++] = source;
  return true;
}

bool ThermalManager::poll(uint64_t now_ms) {
  if (polled_ && now_ms - last_ms_ < kIntervalMs) return false;
  polled_ = true;
  last_ms_ = now_ms;
  hottest_ = -1;
  hottest_name_ = "";
  bool over = false, warm = false;
  for (uint32_t i = 0; i < count_; ++i) {
    int32_t t;
    if (!sources_[i]->read(&t)) continue;
    const ThermalLimits l = sources_[i]->limits();
    if (t > hottest_) { hottest_ = t; hottest_name_ = sources_[i]->name(); }
    if (l.critical_mc >= 0 && t >= l.critical_mc) critical_ = true;
    if (l.passive_mc >= 0) {
      if (t > l.passive_mc) over = true;
      if (t > l.passive_mc - static_cast<int32_t>(kMarginMc)) warm = true;       // not cool enough to give speed back
    }
  }
  uint32_t next = ceiling_;
  if (over) next = ceiling_ > kMinCeiling + kStepPercent ? ceiling_ - kStepPercent : kMinCeiling;
  else if (!warm) next = ceiling_ + kStepPercent > 100 ? 100 : ceiling_ + kStepPercent;
  if (next == ceiling_) return false;
  ceiling_ = next;
  if (actuator_) actuator_->set_ceiling_percent(ceiling_);
  return true;
}

}  // namespace hardware
