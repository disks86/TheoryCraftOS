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
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/NeuralProcessingUnit/Software/SoftwareNeuralProcessingUnit.hpp"
#include "Utility/FixedString.hpp"

namespace hardware {

// Picks the NPU class for this machine: a vendor class when a known hardware
// NPU is on the PCI bus, otherwise the software NPU. There is always one.
class SystemNeuralProcessingUnit {
 public:
  SystemNeuralProcessingUnit(const IPciBus& pci, ICentralProcessingUnit& cpu,
                             scheduling::ICPUScheduler& scheduler);

  INeuralProcessingUnit& npu() { return *active_; }
  // One line for the boot log: which NPU hardware was found (vendor and platform) and that the computation still runs on the CPU, or that there is none.
  const char* description() const { return description_.c_str(); }
  // True when a hardware NPU is on the bus (its work still runs on the CPU until a native driver exists).
  bool hardware_present() const { return hardware_present_; }

 private:
  // Vendor classes add no data, so the software NPU's size covers them all.
  alignas(16) unsigned char storage_[sizeof(SoftwareNeuralProcessingUnit)];
  INeuralProcessingUnit* active_ = nullptr;
  FixedString<96> description_;
  bool hardware_present_ = false;
};

}  // namespace hardware
