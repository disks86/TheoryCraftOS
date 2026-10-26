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

// Which GPU of a hybrid machine to use, the way the balance between performance and power is struck: work that a CPU's own graphics does
// well (the desktop, video playback, pages) goes to the integrated GPU, so the discrete one can stay powered down; heavy work (games, compute,
// WebGL/WebGPU that asks for it) goes to the discrete GPU - but on battery only while there is charge to spare and battery saver is off. A program
// (or the user, per application) can override this with an explicit preference.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/Multi/GpuTopology.hpp"

namespace hardware {

enum class PowerSource : uint8_t { Unknown, Ac, Battery };

// What the caller asks for. Auto lets the policy decide from the workload and the power state.
enum class GpuPreference : uint8_t { Auto, LowPower, HighPerformance };

enum class GpuWorkload : uint8_t {
  Desktop,  // window composition, text, 2D
  Video,    // playback (the media engines of either GPU do the decoding)
  Web,      // pages, canvas, light WebGL
  Game,     // sustained 3D
  Compute,  // GPGPU / heavy WebGPU
};

struct GpuSelectionInput {
  GpuWorkload workload = GpuWorkload::Desktop;
  GpuPreference preference = GpuPreference::Auto;
  PowerSource power = PowerSource::Unknown;
  uint8_t battery_percent = 100;  // 0-100
  bool battery_saver = false;
};

// On battery, heavy work is sent to the discrete GPU only at or above this charge.
constexpr uint8_t kDiscreteMinBatteryPercent = 30;

// True for the workloads that are worth the discrete GPU's power draw.
bool gpu_workload_is_heavy(GpuWorkload workload);

// The index (in `topology`) of the adapter to use, or -1 if the machine has no display controller. Virtual adapters are used only when nothing
// else exists; a machine with a single GPU always gets that one.
int32_t gpu_select(const GpuTopology& topology, const GpuSelectionInput& input);

}  // namespace hardware
