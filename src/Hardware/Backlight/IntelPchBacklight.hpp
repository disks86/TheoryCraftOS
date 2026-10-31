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

// The backlight of a laptop panel driven by the PWM of the PCH (Sunrise Point and later; i915's lpt/spt backlight): BLC_PWM_PCH_CTL1 (0xC8250) has the enable bit (31), BLC_PWM_PCH_CTL2
// (0xC8254) the period in its top half and the duty in its low half. Nothing is configured here: the firmware has started the panel with the PWM running at some period, and brightness
// is the duty out of that period. A PWM that is not enabled, or has no period, is not ours to start (the frequency is a property of the panel, which only the video BIOS tables give),
// and the backlight is then reported as not available.
#pragma once
#include "Hardware/Backlight/IBacklight.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRegisterBank.hpp"

namespace hardware {

class IntelPchBacklight final : public IBacklight {
 public:
  static constexpr uint64_t kControl1 = 0xC8250, kControl2 = 0xC8254;
  static constexpr uint32_t kEnable = 1u << 31;
  explicit IntelPchBacklight(intel_gt::IRegisterBank& registers) : registers_(registers) {}

  const char* name() const override { return "Intel PCH backlight"; }
  bool initialize() override;
  void shutdown() override {}
  uint32_t steps() const override { return period_ < 255 ? period_ + 1 : 256; }
  bool percent(uint8_t* out) override;
  bool set_percent(uint8_t percent) override;

 private:
  intel_gt::IRegisterBank& registers_;
  uint32_t period_ = 0;
};

}  // namespace hardware
