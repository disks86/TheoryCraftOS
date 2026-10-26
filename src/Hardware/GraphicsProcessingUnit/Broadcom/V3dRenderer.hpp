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
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dFrame.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dGpuMemory.hpp"

namespace hardware {

// One block of memory the GPU can read and write, as the CPU and the GPU see it: where the CPU reaches it, the address the GPU uses, and the cache maintenance that
// makes the two views agree (the GPU goes around the CPU's caches).
class IV3dJobMemory {
 public:
  virtual ArrayView<uint8_t> bytes() = 0;
  virtual uint32_t gpu_address() const = 0;
  // Writes the CPU's cached changes to memory before the GPU reads them.
  virtual void clean() {}
  // Drops cached copies before the CPU reads what the GPU wrote.
  virtual void invalidate() {}

 protected:
  constexpr IV3dJobMemory() = default;
  ~IV3dJobMemory() = default;
};

// A block of the GPU memory pool as job memory, with the pool doing the cache maintenance.
class V3dBlockJobMemory final : public IV3dJobMemory {
 public:
  V3dBlockJobMemory(IV3dGpuMemory& memory, const V3dGpuBlock& block) : memory_(memory), block_(block) {}
  ArrayView<uint8_t> bytes() override { return block_.cpu; }
  uint32_t gpu_address() const override { return block_.gpu_address; }
  void clean() override { memory_.clean(block_); }
  void invalidate() override { memory_.invalidate(block_); }

 private:
  IV3dGpuMemory& memory_;
  const V3dGpuBlock& block_;
};

// What goes into the binner's list between "start tile binning" and its end: state and draws. `indirect` is list memory the GPU reads (uniforms, shader records).
class IV3dBinningContent {
 public:
  virtual bool emit(V3dControlList& bcl, V3dControlList& indirect) = 0;

 protected:
  constexpr IV3dBinningContent() = default;
  ~IV3dBinningContent() = default;
};

// How much room the lists of a frame get (each a multiple of 4 KiB). The renderer's list and the per-tile list are 4 KiB each.
struct V3dRenderLayout {
  uint32_t binner_bytes = 4096;
  uint32_t indirect_bytes = 0;
};

// Runs one frame on the V3D: lays the binner's list, the renderer's list, the per-tile list, the tile state and the tile allocation memory out in a block of GPU memory,
// starts the binner and then the renderer, and waits for both. With no draws in the binner's list this is what a clear, a fill or a tile load and store comes to - the
// frame's color target (and depth) are cleared, loaded and stored by the GPU's tile buffer. (Drawing needs shader programs for the QPUs, which this OS does not have yet.)
class V3dRenderer {
 public:
  // The size of the block render() needs for this frame.
  static bool memory_needed(const V3dFrameDesc& frame, uint64_t* bytes) { return memory_needed(frame, V3dRenderLayout(), bytes); }
  static bool memory_needed(const V3dFrameDesc& frame, const V3dRenderLayout& layout, uint64_t* bytes);
  // `attempts` bounds each wait. `overflow` is extra tile memory for the binner when it runs out. The lists are 4 KiB each, in this order: binner, renderer, per-tile,
  // then the tile state and the tile allocation memory (both 4 KiB aligned). The color and depth surfaces of `frame` must already be addresses the GPU can reach.
  static V3dJobResult render(V3dGpu& gpu, const V3dFrameDesc& frame, IV3dJobMemory& memory, uint32_t attempts, ArrayView<const V3dMemoryBlock> overflow = ArrayView<const V3dMemoryBlock>()) {
    return render(gpu, frame, memory, attempts, overflow, V3dRenderLayout(), nullptr);
  }
  // With draws: `content` fills the binner's list and the indirect memory (`layout` sizes them; memory_needed(frame, layout) is the block's size). The order in
  // the block is binner list, renderer list, per-tile list, indirect memory, tile state, tile allocation memory.
  static V3dJobResult render(V3dGpu& gpu, const V3dFrameDesc& frame, IV3dJobMemory& memory, uint32_t attempts, ArrayView<const V3dMemoryBlock> overflow,
                             const V3dRenderLayout& layout, IV3dBinningContent* content);

 private:
  static constexpr uint32_t kListBytes = 4096;
  static bool valid_layout(const V3dRenderLayout& layout) { return layout.binner_bytes >= kListBytes && (layout.binner_bytes & 4095) == 0 && (layout.indirect_bytes & 4095) == 0; }
};

}  // namespace hardware
