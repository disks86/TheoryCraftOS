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
#include "Hardware/GraphicsProcessingUnit/UEFI/UefiGopGpu.hpp"

namespace hardware {

// Intel display controller (PCI vendor 0x8086). Modes and the framebuffer still
// come from the firmware (UEFI GOP); native mode setting for these parts is
// future work, and this class is where it will go.
class IntelGpu final : public UefiGopGpu {
 public:
  static constexpr uint16_t kPciVendor = 0x8086;

  explicit IntelGpu(const GpuContext& context) : UefiGopGpu(context) {}

  const char* name() const override { return "Intel graphics (UEFI framebuffer)"; }
};

}  // namespace hardware
