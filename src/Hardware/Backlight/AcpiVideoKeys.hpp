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

// The brightness keys of a machine whose firmware leaves them to the system: the display device is notified (ACPI 6.4 B.6.4) with 0x86 for "brighter" and 0x87 for "darker" (0x85 cycles the
// outputs, 0x88 is "brightness zero", 0x89 "off"; those are not ours). The step is taken by `step`, which the desktop supplies so that the level is shown on the screen as with the Fn keys.
#pragma once
#include "Hardware/Acpi/AcpiEvents.hpp"
#include "Hardware/Backlight/AcpiBacklight.hpp"

namespace hardware {

class AcpiVideoKeys final : public IAcpiNotifyListener {
 public:
  static constexpr uint32_t kBrighter = 0x86, kDarker = 0x87;
  using Step = void (*)(void* context, bool brighter);
  AcpiVideoKeys(AcpiBacklight& backlight, Step step, void* context) : backlight_(backlight), step_(step), context_(context) {}
  void on_notify(acpi::Node* device, uint32_t value) override {
    if (!step_ || !backlight_.concerns(device)) return;
    if (value == kBrighter) step_(context_, true);
    else if (value == kDarker) step_(context_, false);
  }

 private:
  AcpiBacklight& backlight_;
  Step step_;
  void* context_;
};

}  // namespace hardware
