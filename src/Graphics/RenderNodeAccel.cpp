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

// What a RenderNode does to find and use a 3D engine on its GPU. Apart from RenderNode.cpp so that the rest of the node (and what is built on it) can be
// built without the virgl and SVGA3D devices and the drivers under them.
#include "Graphics/RenderNode.hpp"
#include <new>
#include "Graphics/Graphics3D/BuiltinShaders.hpp"

namespace graphics {

void RenderNode::build_accelerated(hardware::VirtioGpu* virtio, hardware::VmwareSvgaGpu* svga, allocators::IAllocator& allocator, bool allow_acceleration) {
  if (!allow_acceleration) {
    reason_ = "acceleration was switched off";
  } else if (virtio) {
    if (!virtio->has_3d()) {
      reason_ = "virtio-gpu without virgl (start QEMU with virtio-gpu-gl and a gl display)";
    } else {
      VirglDevice* device = new (virgl_storage_) VirglDevice(*virtio);
      if (self_test(*device)) {
        gpu_device_ = device;
        active_ = device;
      } else {
        device->~VirglDevice();
        reason_ = "the virgl device failed its self-test";
      }
    }
  } else if (svga) {
    if (!svga->has_3d()) {
      reason_ = "VMware SVGA adapter without a 3D engine (enable 3D acceleration in the VM settings)";
    } else {
      SvgaDevice* device = new (svga_storage_) SvgaDevice(*svga, allocator);
      if (device->initialize() && self_test(*device) && drawing_test(*device)) {
        gpu_device_ = device;
        active_ = device;
        svga_ = true;
      } else {
        device->~SvgaDevice();
        reason_ = "SVGA3D did not draw the test picture correctly";
      }
    }
  } else {
    reason_text_.assign("the display adapter has no 3D driver: ");
    reason_text_.append(gpu_ ? gpu_->name() : "none");
    reason_ = reason_text_.c_str();
  }
  if (gpu_device_) {
    raw_device_ = gpu_device_;
    locked_ = new (locked_storage_) LockedDevice(*raw_device_);
    gpu_device_ = locked_;
    active_ = locked_;
  }
}

void RenderNode::destroy_accelerated() {
  if (locked_) locked_->~LockedDevice();
  if (raw_device_) {
    if (svga_) static_cast<SvgaDevice*>(raw_device_)->~SvgaDevice();
    else static_cast<VirglDevice*>(raw_device_)->~VirglDevice();
  }
}

// Builds every kind of object once; a driver stack that cannot is not used.
bool RenderNode::self_test(IGraphicsDevice& device) {
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

bool RenderNode::drawing_test(IGraphicsDevice& device) {
  constexpr uint32_t kSize = 16;
  IShaderModule* vs = device.create_shader_module(colored_vertex_shader());
  IShaderModule* fs = device.create_shader_module(colored_fragment_shader());
  IImage* image = device.create_image({{kSize, kSize}, Format::B8G8R8A8Unorm, kImageColorAttachment});
  IBuffer* buffer = device.create_buffer(sizeof(ColoredVertex) * 3, kBufferVertex);
  ICommandBuffer* commands = device.create_command_buffer();
  GraphicsPipelineDesc desc;
  describe_colored_pipeline(&desc);
  desc.vertex_shader = vs;
  desc.fragment_shader = fs;
  desc.cull_mode = CullMode::None;
  IPipeline* pipeline = vs && fs ? device.create_graphics_pipeline(desc) : nullptr;
  bool ok = false;
  if (pipeline && image && buffer && commands) {
    // One big red triangle that covers the whole target; the matrix is the identity (its columns are the constants).
    ByteView memory = buffer->map();
    ColoredVertex* v = reinterpret_cast<ColoredVertex*>(memory.data());
    const float corners[3][2] = {{-1, -1}, {3, -1}, {-1, 3}};
    for (int i = 0; i < 3; ++i) {
      v[i].position[0] = corners[i][0];
      v[i].position[1] = corners[i][1];
      v[i].position[2] = 0.5f;
      v[i].color[0] = 1; v[i].color[1] = 0; v[i].color[2] = 0; v[i].color[3] = 1;
    }
    buffer->unmap();
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    RenderPassBegin pass;
    pass.color = image;
    pass.clear_color_value = {0, 0, 1, 1};   // blue where nothing is drawn
    commands->begin();
    commands->begin_render_pass(pass);
    commands->bind_pipeline(pipeline);
    commands->bind_vertex_buffer(buffer);
    commands->push_constants(identity, sizeof identity);
    commands->draw(3);
    commands->end_render_pass();
    uint32_t pixels[kSize * kSize] = {};
    ok = commands->end() && device.submit(commands) && image->read_pixels(ArrayView<uint32_t>(pixels, kSize * kSize)) &&
         (pixels[(kSize / 2) * kSize + kSize / 2] & 0x00FFFFFF) == 0x00FF0000 && (pixels[0] & 0x00FFFFFF) == 0x00FF0000;
  }
  if (commands) device.destroy_command_buffer(commands);
  if (pipeline) device.destroy_pipeline(pipeline);
  if (buffer) device.destroy_buffer(buffer);
  if (image) device.destroy_image(image);
  if (fs) device.destroy_shader_module(fs);
  if (vs) device.destroy_shader_module(vs);
  return ok;
}

}  // namespace graphics
