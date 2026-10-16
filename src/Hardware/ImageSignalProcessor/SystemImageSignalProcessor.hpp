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
#include "Hardware/ImageSignalProcessor/IImageSignalProcessor.hpp"
#include "Hardware/ImageSignalProcessor/Software/SoftwareImageSignalProcessor.hpp"

namespace hardware {

// Picks the ISP class for this machine: a vendor class when a known hardware
// ISP is on the PCI bus, otherwise the software ISP. There is always one.
class SystemImageSignalProcessor {
 public:
  explicit SystemImageSignalProcessor(const IPciBus& pci);

  IImageSignalProcessor& isp() { return *active_; }

 private:
  // Vendor classes add no data, so the software ISP's size covers them all.
  alignas(16) unsigned char storage_[sizeof(SoftwareImageSignalProcessor)];
  IImageSignalProcessor* active_ = nullptr;
};

}  // namespace hardware
