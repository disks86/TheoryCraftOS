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

#include "Graphics/GraphicsPool.hpp"
#include <new>
#include "Utility/AllocatedView.hpp"

namespace graphics {

void RenderLease::release() {
  if (!pool_) return;
  pool_->gpus_.manager().release(index_);
  pool_ = nullptr;
}

GraphicsPool::GraphicsPool(hardware::GpuPool& gpus, IDisplayGraphics& display, allocators::IAllocator& allocator, bool allow_acceleration)
    : allocator_(allocator), gpus_(gpus), display_(display), display_index_(gpus.manager().display_index()) {
  for (uint32_t i = 0; i < gpus.count(); ++i) {
    Slot& s = slots_[i];
    if (static_cast<int32_t>(i) == display_index_) {
      s.render = &display.render_node();  // one device per GPU: the display GPU already has its own
      continue;
    }
    hardware::GpuNode* n = gpus.node(i);
    void* memory = allocator.allocate_aligned(sizeof(RenderNode), 16);
    if (!memory) continue;
    s.render = new (memory) RenderNode(n->gpu, n->virtio, n->vmware, allocator, allow_acceleration);
    s.owns_render = true;
  }
}

GraphicsPool::~GraphicsPool() {
  for (uint32_t i = 0; i < gpus_.count(); ++i) {
    Slot& s = slots_[i];
    if (s.renderer) {
      s.renderer->~Renderer2DSlot();
      allocator_.deallocate(s.renderer);
    }
    if (s.owns_render && s.render) {
      s.render->~RenderNode();
      allocator_.deallocate(s.render);
    }
  }
  release_view(allocator_, staging_);
}

RenderNode* GraphicsPool::render_node(uint32_t index) { return index < gpus_.count() ? slots_[index].render : nullptr; }

uint32_t GraphicsPool::accelerated_count() {
  uint32_t n = 0;
  for (uint32_t i = 0; i < gpus_.count(); ++i)
    if (slots_[i].render && slots_[i].render->accelerated()) ++n;
  return n;
}

RenderLease GraphicsPool::acquire_node(uint32_t index) {
  if (index >= gpus_.count() || !slots_[index].render) return RenderLease();
  if (!gpus_.manager().acquire(index)) return RenderLease();
  return RenderLease(this, index);
}

RenderLease GraphicsPool::acquire(const hardware::GpuSelectionInput& input) {
  int32_t pick = gpus_.manager().select(input);
  if (pick < 0) return RenderLease();
  if (!slots_[pick].render || !slots_[pick].render->accelerated()) {
    for (uint32_t i = 0; i < gpus_.count(); ++i) {
      if (slots_[i].render && slots_[i].render->accelerated()) {
        pick = static_cast<int32_t>(i);
        break;
      }
    }
  }
  return acquire_node(static_cast<uint32_t>(pick));
}

bool GraphicsPool::present_image(IImage& image, uint32_t target, uint32_t x, uint32_t y) {
  hardware::GpuNode* node = gpus_.node(target);
  if (!node || !node->gpu) return false;
  const Extent2D size = image.extent();
  if (size.width == 0 || size.height == 0) return false;
  const uint64_t pixels = uint64_t{size.width} * size.height;
  if (staging_.size() < pixels) {
    release_view(allocator_, staging_);
    staging_ = allocate_view<uint32_t>(allocator_, pixels);
    if (staging_.empty()) return false;
  }
  if (!image.read_pixels(staging_.subview(0, pixels))) return false;
  node->gpu->blit(x, y, size.width, size.height, staging_.data(), size.width);
  node->gpu->present();
  return true;
}

IRenderer2D* GraphicsPool::renderer_2d(uint32_t index) {
  if (index >= gpus_.count()) return nullptr;
  if (static_cast<int32_t>(index) == display_index_) return display_.renderer_2d();
  Slot& s = slots_[index];
  hardware::GpuNode* n = gpus_.node(index);
  if (!s.render || !n->gpu) return nullptr;
  if (!s.renderer) {
    void* memory = allocator_.allocate(sizeof(Renderer2DSlot));
    if (!memory) return nullptr;
    s.renderer = new (memory) Renderer2DSlot(allocator_);
  }
  return s.renderer->get(*s.render, n->gpu, true);
}

}  // namespace graphics
