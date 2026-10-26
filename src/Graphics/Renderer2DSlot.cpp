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

#include "Graphics/Renderer2DSlot.hpp"
#include <new>
#include "Utility/AllocatedView.hpp"

namespace graphics {

IRenderer2D* Renderer2DSlot::get(RenderNode& node, hardware::IGraphicsProcessingUnit* target, bool allow_device) {
  if (software_) return software_;
  if (device_) return device_;
  if (!target) return nullptr;
  const Extent2D size{target->width(), target->height()};
  if (allow_device && node.accelerated()) {
    void* memory = allocator_.allocate(sizeof(DeviceRenderer2D));
    if (memory) {
      DeviceRenderer2D* r = new (memory) DeviceRenderer2D(*node.gpu_device(), allocator_, size);
      if (r->valid()) return device_ = r;
      r->~DeviceRenderer2D();
      allocator_.deallocate(memory);
    }
  }
  backbuffer_.width = size.width;
  backbuffer_.height = size.height;
  backbuffer_.pitch = size.width;
  backbuffer_.pixels = allocate_view<uint32_t>(allocator_, uint64_t{size.width} * size.height);
  void* memory = allocator_.allocate(sizeof(SoftwareRenderer2D));
  if (!backbuffer_.valid() || !memory) {
    release_view(allocator_, backbuffer_.pixels);
    allocator_.deallocate(memory);
    return nullptr;
  }
  for (uint32_t& p : backbuffer_.pixels) p = 0;
  return software_ = new (memory) SoftwareRenderer2D(backbuffer_, target);
}

void Renderer2DSlot::reset() {
  if (software_) {
    software_->~SoftwareRenderer2D();
    allocator_.deallocate(software_);
    software_ = nullptr;
    release_view(allocator_, backbuffer_.pixels);
  }
  if (device_) {
    device_->~DeviceRenderer2D();
    allocator_.deallocate(device_);
    device_ = nullptr;
  }
}

}  // namespace graphics
