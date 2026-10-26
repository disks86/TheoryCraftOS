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

// Rendering and presenting across every GPU of the machine (GpuPool says what they are). Each GPU gets a RenderNode (its 3D back end, the CPU renderer
// where it has no 3D engine we can drive) and a 2D renderer; the display GPU's are the ones SystemGraphics already has.
//
//  - Render on GPU A: acquire a RenderLease (by policy - see hardware::gpu_select() - or for a chosen GPU). The lease wakes the GPU if it sleeps and
//    lets it sleep again when it is let go.
//  - Present on GPU B: present_image() copies a finished image to GPU B's screen (read back from A, written through B's driver). A and B can be
//    the same GPU or different ones, in either direction, which is how a frame rendered on the discrete GPU reaches a screen that the integrated GPU
//    drives (and the other way round).
//  - Draw 2D on a GPU of its own: renderer_2d(i) draws to the screen of GPU i.
//
// Presenting needs a driver on the target GPU; a GPU without one can render (on the CPU) but not show anything. Not thread safe: one thread presents.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Graphics/RenderNode.hpp"
#include "Graphics/Renderer2DSlot.hpp"
#include "Graphics/IDisplayGraphics.hpp"
#include "Hardware/GraphicsProcessingUnit/Multi/GpuPool.hpp"

namespace graphics {

class GraphicsPool;

// Holds a GPU awake for the length of some rendering. Movable, not copyable.
class RenderLease {
 public:
  RenderLease() = default;
  ~RenderLease() { release(); }
  RenderLease(RenderLease&& other) : pool_(other.pool_), index_(other.index_) { other.pool_ = nullptr; }
  RenderLease& operator=(RenderLease&& other) {
    if (this != &other) {
      release();
      pool_ = other.pool_;
      index_ = other.index_;
      other.pool_ = nullptr;
    }
    return *this;
  }
  RenderLease(const RenderLease&) = delete;
  RenderLease& operator=(const RenderLease&) = delete;

  explicit operator bool() const { return pool_ != nullptr; }
  // The GPU (its index in the GpuPool). Only valid while the lease is held.
  uint32_t node() const { return index_; }
  RenderNode& render_node() const;
  IGraphicsDevice& device() const { return render_node().device(); }
  bool accelerated() const { return render_node().accelerated(); }
  void release();

 private:
  friend class GraphicsPool;
  RenderLease(GraphicsPool* pool, uint32_t index) : pool_(pool), index_(index) {}
  GraphicsPool* pool_ = nullptr;
  uint32_t index_ = 0;
};

class GraphicsPool {
 public:
  // `gpus` and `display` must outlive the pool; `display` is the SystemGraphics (or the like) of the GPU that carries the boot display (its node is reused).
  GraphicsPool(hardware::GpuPool& gpus, IDisplayGraphics& display, allocators::IAllocator& allocator, bool allow_acceleration = true);
  ~GraphicsPool();
  GraphicsPool(const GraphicsPool&) = delete;
  GraphicsPool& operator=(const GraphicsPool&) = delete;

  uint32_t count() const { return gpus_.count(); }
  RenderNode* render_node(uint32_t index);
  // How many GPUs render with their own 3D engine.
  uint32_t accelerated_count();
  hardware::GpuPool& gpus() { return gpus_; }

  // A lease on the GPU that policy picks for this work. If that GPU has no 3D engine of ours but another does, the lease is on the other one (the work
  // is then not left to the CPU for nothing). Empty if the machine has no GPU or the GPU would not wake.
  RenderLease acquire(const hardware::GpuSelectionInput& input);
  // A lease on one GPU; empty for a bad index or a GPU that would not wake.
  RenderLease acquire_node(uint32_t index);

  // Shows a finished image (0xAARRGGBB when read back) on the screen of GPU `target` with its top left at (x, y), clipped to that screen. False if the
  // image is empty, `target` has no display driver, or memory ran out. `source` is the image's device.
  bool present_image(IImage& image, uint32_t target, uint32_t x = 0, uint32_t y = 0);

  // The 2D renderer that draws to the screen of GPU `index`; nullptr if it has no display driver. See Renderer2DSlot.
  IRenderer2D* renderer_2d(uint32_t index);

 private:
  friend class RenderLease;
  struct Slot {
    RenderNode* render = nullptr;
    bool owns_render = false;
    Renderer2DSlot* renderer = nullptr;
  };

  allocators::IAllocator& allocator_;
  hardware::GpuPool& gpus_;
  IDisplayGraphics& display_;
  int32_t display_index_;
  Slot slots_[hardware::GpuTopology::kMaxAdapters];
  ArrayView<uint32_t> staging_;
};

inline RenderNode& RenderLease::render_node() const { return *pool_->slots_[index_].render; }

}  // namespace graphics
