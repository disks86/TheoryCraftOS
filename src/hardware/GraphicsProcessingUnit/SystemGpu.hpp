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
#include "hardware/GraphicsProcessingUnit/GpuContext.hpp"
#include "hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"
#include "hardware/GraphicsProcessingUnit/QEMU/QemuStdVgaGpu.hpp"
#include "hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"

namespace hardware {

constexpr unsigned long GpuMaxOf(unsigned long a, unsigned long b) { return a > b ? a : b; }

// Finds the display controller on the PCI bus and brings up the best GPU class
// for it: a vendor/model class where one exists, else the UEFI framebuffer.
// Storage is inline because there is no heap yet.
class SystemGpu {
 public:
  // Tries candidates in order and keeps the first whose initialize() succeeds.
  SystemGpu(efi::SystemTable* st, const IPciBus& pci);

  // The initialized GPU, or nullptr if no usable display was found.
  IGraphicsProcessingUnit* gpu() { return active_; }

 private:
  static constexpr unsigned long kSize =
      GpuMaxOf(sizeof(UefiGopGpu), sizeof(QemuStdVgaGpu));

  alignas(16) unsigned char storage_[kSize];
  IGraphicsProcessingUnit* active_ = nullptr;
};

}  // namespace hardware
