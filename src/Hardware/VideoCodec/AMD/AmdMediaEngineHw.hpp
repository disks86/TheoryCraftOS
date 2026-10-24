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
  // Radeon APUs only (probe_aperture): the size of the part of system memory the firmware set aside as the GPU's video memory. It is where the table was
  // found, as the table sits in the last 64 KiB of that memory; 0 for a discrete card, whose probe does not learn it.
  uint64_t carve_out_bytes = 0;
  // Radeon cards only: the size of the card's own video memory, when known - from the table's position (probe_video_memory), or, when the table was
  // found at the end of the memory BAR, from the BAR (the BAR is then all of the memory: Resizable BAR is on).
  uint64_t video_memory_bytes = 0;
  uint32_t vcn_count() const { return probed ? discovery.vcn_instances : 0; }
};

// The look at the video engines of an AMD GPU (RDNA 1 and newer). Where Intel's media engines are found by reading fuse and power registers whose
// offsets are fixed per generation, AMD's chips list their blocks in an IP discovery table that the firmware leaves at the end of video memory - the
// same table the amdgpu driver reads - so the probe only reads memory: nothing is written to the device, and no register whose offset would have to be guessed is touched.
// Submitting work to the engine (the ring, the firmware, the codec commands) is not done yet; see AmdVcnVideoEngine.
// Reads the card's video memory at a byte offset, wherever it is - not only the part the CPU can see through the memory BAR.
class IAmdVramReader {
 public:
  virtual bool read32(uint64_t offset, uint32_t* value) = 0;   // offset is a multiple of 4; false if it cannot be read

 protected:
  constexpr IAmdVramReader() = default;
  ~IAmdVramReader() = default;
};

// The way the amdgpu driver reads video memory through the register BAR (BAR 5) of a Radeon card, past the end of the CPU-visible aperture: the first
// registers of the BAR are an index/data pair. MM_INDEX takes the low 31 bits of the offset and a flag (bit 31) that says "video memory", MM_INDEX_HI the rest
// of the offset, and MM_DATA then shows the word. Writes only the two index registers; park() puts them back to zero.
class AmdMmIndexVramReader final : public IAmdVramReader {
 public:
  static constexpr uint64_t kMmIndex = 0x00, kMmData = 0x04, kMmIndexHi = 0x18;
  static constexpr uint64_t kRegisterWindow = 0x1000;   // the registers lie in the first 28 bytes; a page is what the system maps
  static constexpr uint32_t kVramFlag = 0x80000000;

  explicit AmdMmIndexVramReader(const MmioRegion& registers) : registers_(registers) {}
  bool ready() const { return registers_.length() >= kMmIndexHi + 4; }
  bool read32(uint64_t offset, uint32_t* value) override;
  void park();

 private:
  MmioRegion registers_;
};

class AmdMediaEngineHw {
 public:
  // The table sits in the last 64 KiB of video memory.
  static constexpr uint64_t kDiscoveryWindow = 64 * 1024;

  // Do the chips of this architecture carry a discovery table? (Polaris and Vega predate it.)
  static bool supported(AmdGpuArchitecture architecture);

  // Reads the table from `window` (the last kDiscoveryWindow bytes of video memory as seen through the GPU's memory BAR) and parses it. Reads only
  // as many bytes as the table declares for itself. An empty window, a short one, or one that holds no table gives probed == false.
  static AmdMediaEngineStatus probe(allocators::IAllocator& allocator, const MmioRegion& window, AmdGpuArchitecture architecture);

  // The same for a Radeon APU (Ryzen 6000 and newer: RDNA 2, 3 and 3.5 graphics). An APU has no memory of its own: its "video memory" is a carve-out of system
  // RAM, a whole number of MiB the BIOS picks (UMA frame buffer size), and the table is in the last 64 KiB of the carve-out - not of the memory BAR, which
  // may be larger or smaller. `aperture` is the memory BAR from its start (or as much of it as is mapped); the scan looks at every MiB boundary of it, highest
  // first, for the table's signature, and takes the first one that parses and passes its checksum. The size of the carve-out is the boundary it found.
  // Reads only; a few thousand reads at most.
  static AmdMediaEngineStatus probe_aperture(allocators::IAllocator& allocator, const MmioRegion& aperture, AmdGpuArchitecture architecture);
  // The same for a Radeon card whose CPU-visible aperture is smaller than its memory (Resizable BAR off): the table is at the end of the card's memory, so the
  // scan reads the candidate positions through `reader`, smallest memory size first (one MiB steps up to `max_bytes`), and takes the first position that holds a
  // table that parses and passes its checksum. Smallest first because a card's memory can alias above its end; the real end is the lowest hit. The size of
  // the memory is the position found. A reader that fails, or memory with no table, give probed == false.
  static constexpr uint64_t kMaxVideoMemory = 64ull << 30;
  static AmdMediaEngineStatus probe_video_memory(allocators::IAllocator& allocator, IAmdVramReader& reader, AmdGpuArchitecture architecture,
                                                 uint64_t max_bytes = kMaxVideoMemory);
  // The most of an APU's memory BAR the system maps for the scan.
  static constexpr uint64_t kApertureScanLimit = 2ull << 30;
};

}  // namespace hardware
