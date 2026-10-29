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
#include "Hardware/GraphicsProcessingUnit/IGraphicsProcessingUnit.hpp"

namespace graphics {

// The CPU implementation of IGraphicsDevice: a small tile-less scanline
// rasterizer (triangle lists, perspective-correct varyings, depth buffer,
// back-face culling, scissor, alpha blending) running the shaders' native
// functions. Always available; present() copies to the display GPU.
class SoftwareDevice final : public IGraphicsDevice {
 public:
  SoftwareDevice(allocators::IAllocator& allocator, hardware::IGraphicsProcessingUnit* display)
      : allocator_(allocator), display_(display) {}

  DeviceInfo info() const override { return {"software rasterizer", DeviceKind::Software, false, kMaxPushConstantBytes, kFeatureBlendState | kFeatureTexelAccess | kFeatureSystemInputs | kFeatureInstancing | kFeatureLines | kFeatureMultipleTargets | kFeatureFloatFormats | kFeatureOcclusionQueries | kFeatureTextureKinds | kFeatureIndependentBlend}; }

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

 private:
  allocators::IAllocator& allocator_;
  hardware::IGraphicsProcessingUnit* display_;
};

}  // namespace graphics
