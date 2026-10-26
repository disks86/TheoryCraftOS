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

// The memory side of AMD native mode setting on Raven-family APUs (see AmdDcn1.hpp): the OS's own framebuffer in the carve-out of system memory the BIOS sets aside as
// video memory, and the display engine pointed at it. The firmware's mode stays as it is (no resolution choice of its own: a mode change goes through the firmware,
// and the framebuffer is taken over again after it). Everything the firmware set up - what is fetched, how, and how it is shown - is kept; only the address changes.
//
// The framebuffer goes at the end of video memory, clear of the firmware's part and of the memory the video engine uses (amd_plan_display_region(); the video
// engine's plan is told how much to leave). The display engine does not look into the CPU's caches, so what was drawn has to be flushed (flush()).
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDcn1.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Hardware/VideoCodec/AMD/AmdRavenMemory.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace amd_dcn1 {

// The firmware's framebuffer as the UEFI graphics output describes the current mode.
struct FirmwareFramebuffer {
  uint64_t physical = 0;     // the CPU's address of its first byte (the OS runs identity mapped)
  uint64_t bytes = 0;        // the whole allocation
  uint32_t width = 0, height = 0;
  uint32_t pitch_pixels = 0;
};

class Scanout {
 public:
  Scanout(IVcnRegisters& registers, IRegisterMapper& mapper, const AmdRavenMemoryMap& memory_map, uint64_t reserved_bytes)
      : plane_(registers), mapper_(mapper), map_(memory_map), reserved_(reserved_bytes) {}
  ~Scanout() { disengage(); }
  Scanout(const Scanout&) = delete;
  Scanout& operator=(const Scanout&) = delete;

  // Whole MiB of video memory to set aside for the picture, for the largest mode of `max_framebuffer_bytes`.
  static uint64_t region_bytes(uint64_t max_framebuffer_bytes);

  // Copies the firmware's picture into the OS's framebuffer and points the display engine at it. On success `*memory` is the framebuffer to draw in (the rows are the
  // firmware's pitch apart); on failure nothing has changed. False if the memory map or the display engine does not look as expected.
  bool engage(const FirmwareFramebuffer& firmware, ByteView* memory);
  bool engaged() const { return engaged_; }
  // Gives the screen back to the firmware's framebuffer, with what was drawn since copied back into it.
  void disengage();
  // Makes what the CPU drew in the rectangle visible to the display engine.
  static void flush(ByteView memory, uint32_t pitch_bytes, uint32_t x, uint32_t y, uint32_t width, uint32_t height);

  const Plane& plane() const { return plane_; }

 private:
  Plane plane_;
  IRegisterMapper& mapper_;
  AmdRavenMemoryMap map_;
  uint64_t reserved_;
  bool engaged_ = false;
  FirmwareFramebuffer firmware_;
  ByteView memory_;   // the OS's framebuffer (for the current mode)
};

}  // namespace amd_dcn1
}  // namespace hardware
