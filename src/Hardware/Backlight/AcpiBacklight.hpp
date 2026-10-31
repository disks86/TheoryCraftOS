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

// The backlight of an integrated panel as the firmware describes it (ACPI 6.4 section B.6, the video extensions): a display device of the namespace with the methods
//   _BCL   the levels: a package of integers, the level for mains power, the level for battery power, then the levels the panel has
//   _BCM   sets one of them
//   _BQC   the current one (optional)
// The methods do whatever the machine needs - write the embedded controller, a register of the chip, the graphics chip's OpRegion. They run in the interpreter, so nothing here knows how.
// Levels are usually percentages but need not be: they are mapped across the range from the smallest to the largest. A firmware that gives no _BQC is trusted to have the level it was last
// given (the level that was set is remembered).
#pragma once
#include "Hardware/Acpi/AmlInterpreter.hpp"
#include "Hardware/Backlight/IBacklight.hpp"

namespace hardware {

class AcpiBacklight final : public IBacklight {
 public:
  static constexpr uint32_t kMaxLevels = 64;
  AcpiBacklight(acpi::AmlInterpreter& vm, acpi::Node* device) : vm_(vm), device_(device) {}

  // The display devices of the namespace that have both _BCL and _BCM (at most `capacity`).
  static uint32_t find_devices(acpi::AmlInterpreter& vm, acpi::Node** out, uint32_t capacity);
  acpi::Node* device() const { return device_; }
  // Is `node` the device or the one it is a child of? (A notification for a video bus is news for the displays below.)
  bool concerns(const acpi::Node* node) const;

  const char* name() const override { return "ACPI backlight"; }
  bool initialize() override;
  void shutdown() override {}
  uint32_t steps() const override { return count_; }
  bool percent(uint8_t* out) override;
  bool set_percent(uint8_t percent) override;

 private:
  uint32_t level_for(uint8_t percent) const;
  acpi::Node* method(const char* name) const;
  acpi::AmlInterpreter& vm_;
  acpi::Node* device_;
  uint64_t levels_[kMaxLevels] = {};
  uint32_t count_ = 0;
  uint64_t current_ = 0;
  bool known_ = false;
};

}  // namespace hardware
