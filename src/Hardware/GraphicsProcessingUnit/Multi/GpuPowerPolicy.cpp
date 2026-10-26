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

#include "Hardware/GraphicsProcessingUnit/Multi/GpuPowerPolicy.hpp"

namespace hardware {

bool gpu_workload_is_heavy(GpuWorkload workload) { return workload == GpuWorkload::Game || workload == GpuWorkload::Compute; }

namespace {
// The first adapter of the preferred kind; else the other real kind; else whatever exists (the boot display).
int32_t pick(const GpuTopology& t, GpuKind preferred, GpuKind other) {
  int32_t i = t.find_first(preferred);
  if (i < 0) i = t.find_first(other);
  if (i < 0) i = t.find_first(GpuKind::Unknown);
  if (i < 0) i = t.find_first(GpuKind::Virtual);
  return i < 0 ? t.boot_index() : i;
}
}  // namespace

int32_t gpu_select(const GpuTopology& topology, const GpuSelectionInput& input) {
  if (topology.count() == 0) return -1;
  if (topology.count() == 1) return 0;

  bool want_discrete;
  switch (input.preference) {
    case GpuPreference::LowPower: want_discrete = false; break;
    case GpuPreference::HighPerformance: want_discrete = true; break;
    default: {
      want_discrete = gpu_workload_is_heavy(input.workload);
      if (want_discrete && input.power == PowerSource::Battery && (input.battery_saver || input.battery_percent < kDiscreteMinBatteryPercent))
        want_discrete = false;
    }
  }
  return want_discrete ? pick(topology, GpuKind::Discrete, GpuKind::Integrated) : pick(topology, GpuKind::Integrated, GpuKind::Discrete);
}

}  // namespace hardware
