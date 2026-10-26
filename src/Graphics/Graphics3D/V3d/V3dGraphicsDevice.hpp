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
#include "Graphics/Graphics3D/CommandRecorder.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dGpuMemory.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dShaders.hpp"
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"

namespace graphics {

// The 3D device of the Raspberry Pi 4 and Pi 400: IGraphicsDevice on the V3D 4.2 GPU. Buffers and images live in GPU memory (IV3dGpuMemory); a command buffer is
// recorded as usual and, at submit, each render pass becomes one frame on the GPU: the binner sorts the pass's draws into 64x64 tiles and the renderer runs the
// QPU programs per tile and stores the tile buffer to the image. present() copies a finished image to the display.
//
// What it can run: the built-in coloured-vertex pipeline (BuiltinShaders), whose shader modules name QPU programs (there is no shader compiler for the QPUs), with a
// vertex layout of a float3 at location 0 and a float4 at location 1 and the MVP matrix as the first 64 bytes of the push constants. Any other pipeline is refused by
// create_graphics_pipeline() (nullptr), so a caller can fall back to the software device. Within a pass the viewport must lie inside the image; a scissor narrows
// the clip window. Only triangle lists are drawn, and only B8G8R8A8 and R8G8B8A8 colour images with an optional D32Float depth image of the same size are supported.
// Nothing here has run on a real V3D (see README).
class V3dGraphicsDevice final : public IGraphicsDevice {
 public:
  V3dGraphicsDevice(allocators::IAllocator& allocator, hardware::V3dGpu& gpu, hardware::IV3dGpuMemory& memory, hardware::IGraphicsProcessingUnit* display)
      : allocator_(allocator), gpu_(gpu), memory_(memory), display_(display) {}
  ~V3dGraphicsDevice();

  // Puts the QPU programs and the attribute defaults in GPU memory. False if the GPU is not initialised or memory ran out; nothing else works then.
  bool initialize();
  bool ready() const { return code_.valid(); }

  DeviceInfo info() const override { return {"Raspberry Pi V3D 4.2", DeviceKind::Gpu, true}; }

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

  // How many times a job's wait polls the GPU before the job is declared hung (each poll is a few register reads).
  void set_job_attempts(uint32_t attempts) { attempts_ = attempts; }

 private:
  struct DrawRecord;
  class PassContent;
  bool run_pass(const RenderPassBegin& pass, const List<DrawRecord>& draws);

  allocators::IAllocator& allocator_;
  hardware::V3dGpu& gpu_;
  hardware::IV3dGpuMemory& memory_;
  hardware::IGraphicsProcessingUnit* display_;
  hardware::V3dGpuBlock code_, defaults_;
  hardware::v3d_shader::ShaderCode shader_code_;
  uint32_t attempts_ = 5000000;
};

}  // namespace graphics
