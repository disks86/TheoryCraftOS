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

// NVIDIA's NVDEC / NVENC engines: the video decode/encode block that comes with the GPU (PCI vendor 0x10DE). The sessions
// still run on the CPU, through the software codecs, until a native driver for the engine exists - this class
// is where it will go, and what it reports now is that the engine is there (info().hardware_engine) and not used
// (info().accelerated is false). H.264, H.265, VP9 and AV1 will be served here once the engine is driven.
class NvidiaVideoEngine final : public SoftwareVideoCodec {
 public:
  static constexpr uint16_t kPciVendor = 0x10DE;

  explicit NvidiaVideoEngine(allocators::IAllocator& allocator) : SoftwareVideoCodec(allocator, /*hardware_engine=*/true) {}

  const char* name() const override { return "NVIDIA video engine (NVDEC/NVENC) (software path)"; }
};

}  // namespace hardware
