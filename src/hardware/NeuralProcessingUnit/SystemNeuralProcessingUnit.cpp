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

#include "hardware/NeuralProcessingUnit/SystemNeuralProcessingUnit.hpp"
#include <new>
#include "hardware/NeuralProcessingUnit/AMD/AmdXdnaNpu.hpp"
#include "hardware/NeuralProcessingUnit/Intel/IntelNpu.hpp"

namespace hardware {

namespace {
static_assert(sizeof(IntelNpu) == sizeof(SoftwareNeuralProcessingUnit) &&
                  sizeof(AmdXdnaNpu) == sizeof(SoftwareNeuralProcessingUnit),
              "NPU classes must fit SystemNeuralProcessingUnit's storage");
}

SystemNeuralProcessingUnit::SystemNeuralProcessingUnit(const IPciBus& pci, ICentralProcessingUnit& cpu,
                                                       scheduling::ICPUScheduler& scheduler) {
  PciDeviceInfo d;
  for (uint32_t i = 0; !active_ && pci.pci_device(i, &d); ++i) {
    if (d.vendor_id == IntelNpu::kPciVendor && IntelNpu::matches(d.device_id))
      active_ = new (storage_) IntelNpu(cpu, scheduler);
    else if (d.vendor_id == AmdXdnaNpu::kPciVendor && AmdXdnaNpu::matches(d.device_id))
      active_ = new (storage_) AmdXdnaNpu(cpu, scheduler);
  }
  if (!active_) active_ = new (storage_) SoftwareNeuralProcessingUnit(cpu, scheduler);
  active_->initialize();
}

}  // namespace hardware
