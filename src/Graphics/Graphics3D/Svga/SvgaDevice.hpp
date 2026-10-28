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
#include "Graphics/Graphics3D/CommandRecorder.hpp"
#include "Hardware/GraphicsProcessingUnit/VMware/VmwareSvgaGpu.hpp"

namespace graphics {

// IGraphicsDevice on the 3D engine of a VMware SVGA II adapter (SVGA3D, the Direct3D 9 era protocol): the host renders on its own GPU - VMware
// Workstation, Fusion, ESXi, and VirtualBox's VMSVGA adapter, which speaks the same protocol. Resources are host surfaces; the CPU reaches them only by
// DMA through a staging area in video memory (a vertex or index buffer keeps a guest copy that is pushed to the host on unmap()). Shaders are Direct3D 9
// bytecode (ShaderModuleDesc::d3d9_tokens). Commands are sent through the adapter's FIFO.
//
// Written from the protocol as VMware documents it in the open-source svga3d headers, and never run against a host that has the engine: QEMU's
// vmware-svga has none. SystemGraphics therefore draws a test picture on it first and only keeps the device if the host got it right.
class SvgaDevice final : public IGraphicsDevice {
 public:
  SvgaDevice(hardware::VmwareSvgaGpu& gpu, allocators::IAllocator& allocator) : gpu_(gpu), allocator_(allocator) {}
  ~SvgaDevice();

  // Creates the 3D context. False if the host refuses or there is no staging memory.
  bool initialize();

  DeviceInfo info() const override { return {"SVGA3D (VMware)", DeviceKind::VirtualGpu, true}; }

  IBuffer* create_buffer(uint64_t size, uint32_t usage) override;
  void destroy_buffer(IBuffer* buffer) override;
  IImage* create_image(const ImageDesc& desc) override;
  void destroy_image(IImage* image) override;
  IShaderModule* create_shader_module(const ShaderModuleDesc& desc) override;
  void destroy_shader_module(IShaderModule* module) override;
  IPipeline* create_graphics_pipeline(const GraphicsPipelineDesc& desc) override;
  void destroy_pipeline(IPipeline* pipeline) override;
  ICommandBuffer* create_command_buffer() override;
  void destroy_command_buffer(ICommandBuffer* commands) override;

  bool submit(ICommandBuffer* commands) override;
  bool present(IImage* image) override;

  // --- Used by the resource classes.
  static constexpr uint32_t kContext = 1;
  uint32_t new_id() { return next_id_++; }
  // One 3D command: `body` words after the id/size header.
  bool command(uint32_t id, const uint32_t* body, uint32_t words);
  // Surface transfers through the staging area. `write` copies guest -> host. For a 1D buffer surface `width` is bytes and `height` is 1.
  bool transfer(uint32_t surface, bool write, uint32_t x, uint32_t width, uint32_t height, uint32_t pitch, void* guest_data);
  // The same for writing from data that is not to be touched (a texture's pixels).
  bool upload(uint32_t surface, uint32_t mip, uint32_t x, uint32_t width, uint32_t height, uint32_t pitch, const void* guest_data) {
    return move(surface, mip, true, x, width, height, pitch, guest_data, nullptr);
  }
  // ... and reading a level of an image back.
  bool download(uint32_t surface, uint32_t mip, uint32_t x, uint32_t width, uint32_t height, uint32_t pitch, void* out) { return move(surface, mip, false, x, width, height, pitch, nullptr, out); }
  uint32_t scratch_bytes() const { return scratch_bytes_; }
  hardware::VmwareSvgaGpu& gpu() { return gpu_; }
  allocators::IAllocator& allocator() { return allocator_; }

 private:
  bool move(uint32_t surface, uint32_t mip, bool write, uint32_t start, uint32_t width, uint32_t height, uint32_t pitch, const void* source, void* destination);
  hardware::VmwareSvgaGpu& gpu_;
  allocators::IAllocator& allocator_;
  uint8_t* scratch_ = nullptr;
  uint32_t scratch_offset_ = 0, scratch_bytes_ = 0;
  uint32_t next_id_ = 1;   // surface and shader ids (one namespace each on the host; one counter is enough)
  bool context_ = false;
};

}  // namespace graphics
