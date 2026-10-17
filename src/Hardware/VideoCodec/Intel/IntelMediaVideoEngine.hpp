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
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"

namespace hardware {

// Intel's media engine in its graphics (Quick Sync Video: the VDBOX/MFX fixed-function blocks): the video decode/encode block that comes with the GPU (PCI vendor 0x8086). The sessions
// still run on the CPU, through the software codecs, until a native driver for the engine exists - this class
// is where it will go, and what it reports now is that the engine is there (info().hardware_engine) and not used
// (info().accelerated is false). H.264, H.265, VP9 and AV1 will be served here once the engine is driven.
class IntelMediaVideoEngine final : public SoftwareVideoCodec {
 public:
  static constexpr uint16_t kPciVendor = 0x8086;

  explicit IntelMediaVideoEngine(allocators::IAllocator& allocator) : SoftwareVideoCodec(allocator, /*hardware_engine=*/true) {}

  const char* name() const override { return "Intel media engine (Quick Sync) (software path)"; }
};

}  // namespace hardware
