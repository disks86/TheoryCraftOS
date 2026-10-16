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

#include "Hardware/ImageSignalProcessor/SystemImageSignalProcessor.hpp"
#include <new>
#include "Hardware/ImageSignalProcessor/Intel/IntelIpuIsp.hpp"

namespace hardware {

namespace {
static_assert(sizeof(IntelIpu6Isp) == sizeof(SoftwareImageSignalProcessor) &&
                  sizeof(IntelIpu7Isp) == sizeof(SoftwareImageSignalProcessor),
              "ISP classes must fit SystemImageSignalProcessor's storage");

// Camera ISPs enumerate as "multimedia: other" (class 0x04, subclass 0x80).
constexpr uint8_t kClassMultimedia = 0x04;
constexpr uint8_t kSubclassOther = 0x80;
}  // namespace

SystemImageSignalProcessor::SystemImageSignalProcessor(const IPciBus& pci) {
  PciDeviceInfo d;
  for (uint32_t i = 0; !active_ && pci.pci_device(i, &d); ++i) {
    if (d.class_code != kClassMultimedia || d.subclass != kSubclassOther) continue;
    if (d.vendor_id != kIntelPciVendor) continue;
    if (IntelIpu6Isp::matches(d.device_id)) active_ = new (storage_) IntelIpu6Isp();
    else if (IntelIpu7Isp::matches(d.device_id)) active_ = new (storage_) IntelIpu7Isp();
  }
  if (!active_) active_ = new (storage_) SoftwareImageSignalProcessor();
  active_->initialize();
}

}  // namespace hardware
