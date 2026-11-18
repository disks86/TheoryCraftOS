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

// The battery charge limit kept in the system settings (the system database, filesystem::layout::kSystemDatabase, i.e. the "System"
// application's folder in the AppStore): scope "power", name "battery_charge_limit", the percentage the EC stops charging at.
#pragma once
#include "Database/Local/SystemSettings.hpp"
#include "Hardware/EmbeddedController/ChromeEc/ChromeEcStack.hpp"

namespace hardware {

class SettingsChargeLimitStore final : public IChargeLimitStore {
 public:
  static constexpr const char* kScope = "power";
  static constexpr const char* kName = "battery_charge_limit";

  explicit SettingsChargeLimitStore(database::local::SystemSettings& settings) : settings_(settings) {}

  Optional<uint8_t> load() override {
    const int64_t value = settings_.get_int(kScope, kName, -1);
    if (value < 0 || value > 100) return {};
    return static_cast<uint8_t>(value);
  }
  bool store(uint8_t max_percent) override { return settings_.set_int(kScope, kName, max_percent); }

 private:
  database::local::SystemSettings& settings_;
};

}  // namespace hardware
