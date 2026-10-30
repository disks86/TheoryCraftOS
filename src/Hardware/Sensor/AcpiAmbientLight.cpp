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

#include "Hardware/Sensor/AcpiAmbientLight.hpp"

namespace hardware {

namespace {
acpi::Node* method_of(acpi::AmlInterpreter& vm, acpi::Node* device, const char* name) {
  for (acpi::Node* c = vm.first_child(device); c; c = vm.next_sibling(c)) {
    const char* n = vm.name_of(c);
    if (n && n[0] == name[0] && n[1] == name[1] && n[2] == name[2] && n[3] == name[3] && vm.is_method(c)) return c;
  }
  return nullptr;
}

void walk(acpi::AmlInterpreter& vm, acpi::Node* node, uint32_t depth, acpi::Node** out, uint32_t capacity, uint32_t* count) {
  if (depth > 10) return;
  for (acpi::Node* c = vm.first_child(node); c && *count < capacity; c = vm.next_sibling(c)) {
    if (vm.is_device(c) && vm.device_has_id(c, "ACPI0008") && method_of(vm, c, "_ALI")) out[(*count)++] = c;
    walk(vm, c, depth + 1, out, capacity, count);
  }
}
}  // namespace

uint32_t AcpiAmbientLight::find_devices(acpi::AmlInterpreter& vm, acpi::Node** out, uint32_t capacity) {
  uint32_t count = 0;
  walk(vm, vm.root(), 0, out, capacity, &count);
  return count;
}

bool AcpiAmbientLight::initialize() {
  uint64_t lux;
  acpi::Node* ali = method_of(vm_, device_, "_ALI");
  return ali && vm_.evaluate_integer(ali, &lux);
}

bool AcpiAmbientLight::read(SensorReading* out) {
  uint64_t lux;
  acpi::Node* ali = method_of(vm_, device_, "_ALI");
  if (!ali || !vm_.evaluate_integer(ali, &lux)) return false;
  SensorReading r;
  r.kind = SensorKind::AmbientLight;
  r.x = static_cast<float>(lux);
  r.sequence = ++sequence_;
  if (out) *out = r;
  return true;
}

}  // namespace hardware
