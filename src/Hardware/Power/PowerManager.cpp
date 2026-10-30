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

#include "Hardware/Power/PowerManager.hpp"

namespace hardware {

void PowerManager::apply(PowerProfile profile) {
  profile_ = profile;
  cpu_.set_profile(profile);
  if (set_idle_hint_) set_idle_hint_(CpuPower::idle_hint(caps_, profile));
}

void PowerManager::start() { apply(PowerProfile::Balanced); }

void PowerManager::set_profile(PowerProfile profile) {
  automatic_ = false;
  apply(profile);
}

void PowerManager::set_automatic() { automatic_ = true; }

bool PowerManager::update(const BatteryStatus& battery, uint64_t now_ms) {
  on_battery_ = battery.present && battery.state == BatteryState::Discharging;
  if (!automatic_) return false;
  PowerProfile wanted = PowerProfile::Balanced;
  if (on_battery_ && battery.percent >= 0 && battery.percent < kLowBatteryPercent) wanted = PowerProfile::PowerSaver;
  if (wanted == profile_) return false;
  if (changed_once_ && now_ms - changed_ms_ < kMinDwellMs) return false;
  changed_once_ = true;
  changed_ms_ = now_ms;
  apply(wanted);
  return true;
}

}  // namespace hardware
