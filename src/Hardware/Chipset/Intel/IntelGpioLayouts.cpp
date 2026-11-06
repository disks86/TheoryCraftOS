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

#include "Hardware/Chipset/Intel/IntelGpioLayouts.hpp"

namespace hardware {

namespace {
#include "Hardware/Chipset/Intel/IntelGpioLayouts.inc"

bool same(const char* a, const char* b) {
  for (uint32_t i = 0; i < 16; ++i) {
    if (a[i] != b[i]) return false;
    if (a[i] == 0) return true;
  }
  return false;
}
}  // namespace

const IntelGpioLayout* intel_gpio_layout(const char* hid, const char* uid) {
  if (!hid) return nullptr;
  for (const IntelGpioLayout& layout : kIntelGpioLayouts) {
    if (!same(layout.hid, hid)) continue;
    if (layout.uid && (!uid || !same(layout.uid, uid))) continue;
    return &layout;
  }
  return nullptr;
}

uint32_t intel_gpio_layout_count() { return sizeof(kIntelGpioLayouts) / sizeof(kIntelGpioLayouts[0]); }

const IntelGpioLayout* intel_gpio_layout_at(uint32_t index) { return index < intel_gpio_layout_count() ? &kIntelGpioLayouts[index] : nullptr; }

}  // namespace hardware
