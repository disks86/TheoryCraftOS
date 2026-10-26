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

// The memory side of Intel native mode setting (see IntelDisplay.hpp): a framebuffer in ordinary system memory, large enough for the panel's own size, made
// visible to the display engine through a stretch of unused global GTT entries; and the modes shown from it. The display engine does not look into the CPU's
// caches, so what was drawn has to be flushed out of them (flush()) before it shows.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelDisplay.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGtt.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace intel_display {

class Scanout {
 public:
  // The linear surface of a Gen9+ plane is aligned to 256 KiB.
  static constexpr uint64_t kSurfaceAlignmentPages = 64;
  // Entries left alone at the top of the global GTT.
  static constexpr uint64_t kGuardPages = 16;

  // `registers`: the register BAR (at least kRegisterWindow bytes); `ggtt_table`: the global GTT table; both must outlive this object.
  Scanout(intel_gt::IRegisterBank& registers, const MmioRegion& ggtt_table, IntelGpuArchitecture architecture, allocators::IAllocator& allocator)
      : display_(registers, architecture), registers_(registers), ggtt_table_(ggtt_table), allocator_(allocator) {}
  ~Scanout() { shutdown(); }
  Scanout(const Scanout&) = delete;
  Scanout& operator=(const Scanout&) = delete;

  // Finds the pipe the firmware shows the screen on, allocates the framebuffer for the panel's size and maps it. False (and nothing changed) if any step fails.
  bool initialize();
  bool ready() const { return ready_; }
  const Display& display() const { return display_; }

  // Shows `width` x `height` from the framebuffer and returns the memory to draw in (the rows start `pitch_bytes` apart). The picture starts black. False if the
  // display engine does not take the mode (the previous one stays).
  bool set_mode(uint32_t width, uint32_t height, ByteView* memory, uint32_t* pitch_bytes);
  // Makes what the CPU drew in the rectangle visible to the display engine.
  static void flush(ByteView memory, uint32_t pitch_bytes, uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  // Gives the screen back to the firmware's framebuffer and unmaps ours.
  void shutdown();

 private:
  Display display_;
  intel_gt::IRegisterBank& registers_;
  MmioRegion ggtt_table_;
  allocators::IAllocator& allocator_;
  bool ready_ = false;
  ByteView memory_;                      // the whole framebuffer
  uint64_t first_page_ = 0, pages_ = 0;  // of the global GTT
  ArrayView<uint64_t> saved_entries_;    // what the pages held before
};

}  // namespace intel_display
}  // namespace hardware
