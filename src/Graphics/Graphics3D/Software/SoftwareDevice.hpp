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

  DeviceInfo info() const override { return {"software rasterizer", DeviceKind::Software, false, kMaxPushConstantBytes, kFeatureBlendState | kFeatureVertexFormats | kFeatureAddressModes | kFeatureTexelAccess | kFeatureSystemInputs | kFeatureInstancing | kFeatureLines | kFeatureMultipleTargets | kFeatureFloatFormats | kFeatureOcclusionQueries | kFeatureStatisticsQueries | kFeatureTimestampQueries | kFeatureDepthResolve | kFeatureResolveModes | kFeatureStreamOutput | kFeatureTextureKinds | kFeatureIndependentBlend | kFeatureCompute | kFeatureMultisample | kFeatureGeometryShaders | kFeatureSrgbFormats | kFeatureUnorm16Formats | kFeatureIntegerFormats | kFeatureSnormFormats | kFeatureTessellation | kFeatureStorageInDraws | kFeatureFragmentStores | kFeatureVertexStores | kFeatureGeometryStores | kFeatureTessellationStores, (1u << 2) | (1u << 4) | (1u << 8),
            DeviceLimits{kMaxVertexAttributes, kMaxVaryings, kMaxTextureUnits, kMaxColorTargets, kMaxVertexBuffers}, true, kFeature2StorageImages | kFeature2StorageImagesInDraws | kFeature2LayeredTargets | kFeature2SingleChannelFormats | kFeature2ConservativeRaster | kFeature2DepthTextures | kFeature2DepthAndStencilPlanes | kFeature2TexelBuffers | kFeature2StorageImagesAllStages | kFeature2MipmappedLayers | kFeature2RasterizerState | kFeature2DualSourceBlend | kFeature2LogicOps | kFeature2SamplerState | kFeature2Viewports | kFeature2LayerFromAnyStage | kFeature2TileResidency}; }   // (the CPU renderer takes all the capacity)

  // Every format numbered from 64: the packed 32-bit ones as the 8-bit formats (levels, filtering, blending, storage), the wide ones with levels too - every image has them,
  // kFeature2MipmappedLayers - (the integer ones not filtered or blended; R9G9B9E5 only sampled, the 96-bit ones not storage images - Direct3D has neither), the block-compressed
  // ones decoded by the memory manager.
  uint32_t format_caps(Format f) const override {
    // (storage images of R8 and R8G8 alone: Direct3D has no unordered access to alpha-only or 5:6:5 / 5:5:5:1 / 4:4:4:4 textures)
    if (is_packed32_format(f)) return kFormatSampled | kFormatFiltered | kFormatRenderTarget | kFormatBlendable | kFormatMipmaps | (f == Format::R8Unorm || f == Format::R8G8Unorm ? kFormatStorage : 0u);
    if (f == Format::R9G9B9E5Float) return kFormatSampled | kFormatFiltered | kFormatMipmaps;
    if (is_extra_wide_format(f)) {
      const bool rgb32 = f == Format::R32G32B32Uint || f == Format::R32G32B32Sint || f == Format::R32G32B32Float;
      return kFormatSampled | kFormatRenderTarget | kFormatMipmaps | (rgb32 ? 0u : kFormatStorage) | (is_integer_format(f) ? 0u : kFormatFiltered | kFormatBlendable);
    }
    return f >= Format::Bc4Unorm && f <= Format::Bc7Srgb ? kFormatSampled | kFormatFiltered | kFormatMipmaps : 0u;
  }
  uint32_t max_image_dimension() const override { return 16384; }

  IBuffer* create_buffer(uint64_t size, uint32_t usage) override;
  void destroy_buffer(IBuffer* buffer) override;
  IImage* create_image(const ImageDesc& desc) override;
  void destroy_image(IImage* image) override;
  IShaderModule* create_shader_module(const ShaderModuleDesc& desc) override;
  void destroy_shader_module(IShaderModule* module) override;
  IPipeline* create_graphics_pipeline(const GraphicsPipelineDesc& desc) override;
  IPipeline* create_compute_pipeline(const IShaderModule* shader) override;
  void destroy_pipeline(IPipeline* pipeline) override;
  ICommandBuffer* create_command_buffer() override;
  void destroy_command_buffer(ICommandBuffer* commands) override;

  bool submit(ICommandBuffer* commands) override;
  bool present(IImage* image) override;

  // The clock timestamp queries read (nanoseconds); without one the device counts 1000 more for every timestamp.
  void set_clock(uint64_t (*now)(void*), void* context) {
    clock_ = now;
    clock_context_ = context;
    interrupt_clock();   // (a new clock: the old timestamps do not compare with its)
  }
  void interrupt_clock() override { clock_interrupted_ = true; }

 private:
  allocators::IAllocator& allocator_;
  hardware::IGraphicsProcessingUnit* display_;
  uint64_t (*clock_)(void*) = nullptr;
  void* clock_context_ = nullptr;
  uint64_t counted_ns_ = 0;
  bool clock_interrupted_ = false;
};

}  // namespace graphics
