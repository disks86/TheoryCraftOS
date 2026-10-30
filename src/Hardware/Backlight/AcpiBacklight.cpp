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

#include "Hardware/Backlight/AcpiBacklight.hpp"

namespace hardware {

namespace {
bool name_is(const acpi::AmlInterpreter& vm, const acpi::Node* node, const char* name) {
  const char* n = vm.name_of(node);
  return n && n[0] == name[0] && n[1] == name[1] && n[2] == name[2] && n[3] == name[3];
}

acpi::Node* child_named(const acpi::AmlInterpreter& vm, const acpi::Node* parent, const char* name) {
  for (acpi::Node* c = vm.first_child(parent); c; c = vm.next_sibling(c))
    if (name_is(vm, c, name)) return c;
  return nullptr;
}

void walk(acpi::AmlInterpreter& vm, acpi::Node* node, uint32_t depth, acpi::Node** out, uint32_t capacity, uint32_t* count) {
  if (depth > 10 || *count >= capacity) return;
  for (acpi::Node* c = vm.first_child(node); c && *count < capacity; c = vm.next_sibling(c)) {
    if (vm.is_device(c)) {
      acpi::Node* bcl = child_named(vm, c, "_BCL");
      acpi::Node* bcm = child_named(vm, c, "_BCM");
      if (bcl && bcm && vm.is_method(bcm)) out[(*count)++] = c;
    }
    walk(vm, c, depth + 1, out, capacity, count);
  }
}
}  // namespace

uint32_t AcpiBacklight::find_devices(acpi::AmlInterpreter& vm, acpi::Node** out, uint32_t capacity) {
  uint32_t count = 0;
  walk(vm, vm.root(), 0, out, capacity, &count);
  return count;
}

bool AcpiBacklight::concerns(const acpi::Node* node) const { return node && (node == device_ || node == vm_.parent(device_)); }

acpi::Node* AcpiBacklight::method(const char* name) const { return device_ ? child_named(vm_, device_, name) : nullptr; }

bool AcpiBacklight::initialize() {
  count_ = 0;
  known_ = false;
  acpi::Node* bcl = method("_BCL");
  acpi::Value list;
  if (!bcl || !vm_.evaluate(bcl, nullptr, 0, &list) || list.kind != acpi::ValueKind::Package || list.length < 3) return false;
  // the first two are the levels for mains and battery power, the rest the panel's, in no particular order
  for (uint32_t i = 2; i < list.length; ++i) {
    if (list.elements[i].kind != acpi::ValueKind::Integer) continue;
    const uint64_t level = list.elements[i].integer;
    uint32_t at = 0;
    while (at < count_ && levels_[at] < level) ++at;
    if (at < count_ && levels_[at] == level) continue;                        // (listed twice)
    if (count_ == kMaxLevels) break;
    for (uint32_t k = count_; k > at; --k) levels_[k] = levels_[k - 1];
    levels_[at] = level;
    ++count_;
  }
  if (count_ == 0 || !method("_BCM")) return false;
  uint8_t now;
  percent(&now);
  return true;
}

uint32_t AcpiBacklight::level_for(uint8_t percent) const {
  if (percent > 100) percent = 100;
  const uint64_t low = levels_[0], high = levels_[count_ - 1];
  const uint64_t wanted = low + ((high - low) * percent + 50) / 100;
  uint32_t best = 0;
  for (uint32_t i = 1; i < count_; ++i) {                                     // the nearest level the panel has
    const uint64_t a = levels_[i] > wanted ? levels_[i] - wanted : wanted - levels_[i], b = levels_[best] > wanted ? levels_[best] - wanted : wanted - levels_[best];
    if (a < b) best = i;
  }
  return best;
}

bool AcpiBacklight::percent(uint8_t* out) {
  if (count_ == 0) return false;
  uint64_t level = current_;
  acpi::Node* bqc = method("_BQC");
  if (bqc) {
    uint64_t value;
    if (vm_.evaluate_integer(bqc, &value)) {
      level = value;
      current_ = value;
      known_ = true;
    }
  }
  if (!known_) return false;
  const uint64_t low = levels_[0], high = levels_[count_ - 1];
  if (high == low) { *out = 100; return true; }
  if (level <= low) { *out = 0; return true; }
  if (level >= high) { *out = 100; return true; }
  *out = static_cast<uint8_t>(((level - low) * 100 + (high - low) / 2) / (high - low));
  return true;
}

bool AcpiBacklight::set_percent(uint8_t percent) {
  if (count_ == 0) return false;
  acpi::Node* bcm = method("_BCM");
  if (!bcm) return false;
  const uint64_t level = levels_[level_for(percent)];
  acpi::Value argument;
  argument.kind = acpi::ValueKind::Integer;
  argument.integer = level;
  if (!vm_.evaluate(bcm, &argument, 1, nullptr)) return false;
  current_ = level;
  known_ = true;
  return true;
}

}  // namespace hardware
