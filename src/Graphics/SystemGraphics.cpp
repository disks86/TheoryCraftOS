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

#include "Graphics/SystemGraphics.hpp"
#include <new>
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Utility/AllocatedView.hpp"

namespace graphics {

SystemGraphics::SystemGraphics(hardware::SystemGpu& gpu, allocators::IAllocator& allocator, bool allow_acceleration)
    : allocator_(allocator), display_(gpu.gpu()), software_(allocator, gpu.gpu()) {
  active_ = &software_;
  hardware::VirtioGpu* virtio = gpu.virtio_gpu();
  if (allow_acceleration && virtio && virtio->has_3d()) {
    VirglDevice* device = new (virgl_storage_) VirglDevice(*virtio);
    if (self_test(*device)) {
      gpu_device_ = device;
      active_ = device;
    } else {
      device->~VirglDevice();
    }
  }
}

IRenderer2D* SystemGraphics::renderer_2d() {
  if (software_2d_) return software_2d_;
  if (device_2d_) return device_2d_;
  if (!display_) return nullptr;
  hardware::IGraphicsProcessingUnit* target = override_ ? override_ : display_;
  const Extent2D size{target->width(), target->height()};
  if (accelerated() && !override_) {
    void* memory = allocator_.allocate(sizeof(DeviceRenderer2D));
    if (memory) {
      DeviceRenderer2D* r = new (memory) DeviceRenderer2D(*gpu_device_, allocator_, size);
      if (r->valid()) return device_2d_ = r;
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
  return software_2d_ = new (memory) SoftwareRenderer2D(backbuffer_, target);
}

void SystemGraphics::reset_renderer() {
  if (software_2d_) {
    software_2d_->~SoftwareRenderer2D();
    allocator_.deallocate(software_2d_);
    software_2d_ = nullptr;
    release_view(allocator_, backbuffer_.pixels);
  }
  if (device_2d_) {
    device_2d_->~DeviceRenderer2D();
    allocator_.deallocate(device_2d_);
    device_2d_ = nullptr;
  }
}

SystemGraphics::~SystemGraphics() {
  if (software_2d_) {
    software_2d_->~SoftwareRenderer2D();
    allocator_.deallocate(software_2d_);
    release_view(allocator_, backbuffer_.pixels);
  }
  if (device_2d_) {
    device_2d_->~DeviceRenderer2D();
    allocator_.deallocate(device_2d_);
  }
  if (gpu_device_) static_cast<VirglDevice*>(gpu_device_)->~VirglDevice();
}

// Builds every kind of object once; a driver stack that cannot is not used.
bool SystemGraphics::self_test(IGraphicsDevice& device) {
  IShaderModule* vs = device.create_shader_module(colored_vertex_shader());
  IShaderModule* fs = device.create_shader_module(colored_fragment_shader());
  IImage* image = device.create_image({{16, 16}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  IBuffer* buffer = device.create_buffer(64, kBufferVertex);
  GraphicsPipelineDesc desc;
  describe_colored_pipeline(&desc);
  desc.vertex_shader = vs;
  desc.fragment_shader = fs;
  IPipeline* pipeline = vs && fs ? device.create_graphics_pipeline(desc) : nullptr;
  const bool ok = vs && fs && image && buffer && pipeline;
  if (pipeline) device.destroy_pipeline(pipeline);
  if (buffer) device.destroy_buffer(buffer);
  if (image) device.destroy_image(image);
  if (fs) device.destroy_shader_module(fs);
  if (vs) device.destroy_shader_module(vs);
  return ok;
}

}  // namespace graphics
