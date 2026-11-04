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


#pragma once
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuExecutor.hpp"
#include "Hardware/NeuralProcessingUnit/Software/SoftwareNeuralProcessingUnit.hpp"

namespace hardware {

// The Intel NPU as the system's INeuralProcessingUnit once its firmware runs. The matrix and activation primitives of the interface are still computed on the CPU:
// the NPU executes compiled network graphs, not single matrix products, and there is no compiler here to produce them. What this class adds is the device itself:
// executor() runs command streams on it, and busy_us() counts the time the NPU spent on them besides the CPU's own work, so the computer manager's meter shows both.
class IntelNpuAccelerator final : public SoftwareNeuralProcessingUnit {
 public:
  IntelNpuAccelerator(ICentralProcessingUnit& cpu, scheduling::ICPUScheduler& scheduler, IntelNpuExecutor& executor)
      : SoftwareNeuralProcessingUnit(cpu, scheduler), executor_(executor) {}

  const char* name() const override { return "Intel NPU (firmware running, tensor math on the CPU)"; }
  uint64_t busy_us() const override { return SoftwareNeuralProcessingUnit::busy_us() + executor_.busy_us(); }
  IntelNpuExecutor& executor() { return executor_; }

 private:
  IntelNpuExecutor& executor_;
};

}  // namespace hardware
