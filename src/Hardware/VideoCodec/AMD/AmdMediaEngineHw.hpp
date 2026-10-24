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
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpuGenerations.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/VideoCodec/AMD/AmdIpDiscovery.hpp"

namespace hardware {

// What the first look at an AMD GPU's video engines found.
struct AmdMediaEngineStatus {
  bool probed = false;            // a valid IP discovery table was read (false: unsupported generation, no window, or no table where it should be)
  AmdIpDiscoveryInfo discovery;
  uint32_t vcn_count() const { return probed ? discovery.vcn_instances : 0; }
};

// The look at the video engines of an AMD GPU (RDNA 1 and newer). Where Intel's media engines are found by reading fuse and power registers whose
// offsets are fixed per generation, AMD's chips list their blocks in an IP discovery table that the firmware leaves at the end of video memory - the
// same table the amdgpu driver reads - so the probe only reads memory: nothing is written to the device, and no register whose offset would have to be guessed is touched.
// Submitting work to the engine (the ring, the firmware, the codec commands) is not done yet; see AmdVcnVideoEngine.
class AmdMediaEngineHw {
 public:
  // The table sits in the last 64 KiB of video memory.
  static constexpr uint64_t kDiscoveryWindow = 64 * 1024;

  // Do the chips of this architecture carry a discovery table? (Polaris and Vega predate it.)
  static bool supported(AmdGpuArchitecture architecture);

  // Reads the table from `window` (the last kDiscoveryWindow bytes of video memory as seen through the GPU's memory BAR) and parses it. Reads only
  // as many bytes as the table declares for itself. An empty window, a short one, or one that holds no table gives probed == false.
  static AmdMediaEngineStatus probe(allocators::IAllocator& allocator, const MmioRegion& window, AmdGpuArchitecture architecture);
};

}  // namespace hardware
