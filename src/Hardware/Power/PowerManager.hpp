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

// The power policy of the machine: which profile the processor runs under. Automatic follows the battery - on mains power (or with no battery) Balanced; on battery Balanced while it
// has more than a third left, Power saver below, with a pause between changes so a battery that reports wildly does not make it flap. A profile chosen by the user holds until
// automatic is chosen again. Applying a profile sets the processor's speed range and preference (CpuPower) and the C-state that idle cores ask for.
#pragma once
#include "Hardware/Battery/AcpiBattery.hpp"
#include "Hardware/Power/CpuPower.hpp"

namespace hardware {

class PowerManager {
 public:
  static constexpr int32_t kLowBatteryPercent = 33;
  static constexpr uint64_t kMinDwellMs = 10000;
  using IdleHintSetter = void (*)(uint32_t hint);
  PowerManager(CpuPower& cpu, const CpuPowerCaps& caps, IdleHintSetter set_idle_hint) : cpu_(cpu), caps_(caps), set_idle_hint_(set_idle_hint) {}

  // Applies the starting profile (Balanced).
  void start();
  void set_profile(PowerProfile profile);          // manual
  void set_automatic();
  bool automatic() const { return automatic_; }
  PowerProfile profile() const { return profile_; }
  // The battery, now (call about once a second). Returns true if the profile changed.
  bool update(const BatteryStatus& battery, uint64_t now_ms);
  // Whether the machine is running on its battery as of the last update.
  bool on_battery() const { return on_battery_; }

 private:
  void apply(PowerProfile profile);
  CpuPower& cpu_;
  CpuPowerCaps caps_;
  IdleHintSetter set_idle_hint_;
  PowerProfile profile_ = PowerProfile::Balanced;
  bool automatic_ = true, on_battery_ = false, changed_once_ = false;
  uint64_t changed_ms_ = 0;
};

}  // namespace hardware
