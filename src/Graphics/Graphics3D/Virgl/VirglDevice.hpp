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
#include "Hardware/GraphicsProcessingUnit/Virtio/VirtioGpu.hpp"

namespace graphics {

// IGraphicsDevice on a virtio-gpu with virgl: QEMU runs the work on the host's
// OpenGL. Resources are host-side virgl resources (buffers keep a guest copy that
// is pushed to the host on unmap()); a submit translates the recorded commands to
// the virgl command stream (Gallium-style state objects, TGSI shaders). Images are
// only read back to the CPU on request (IImage::read_pixels).
class VirglDevice final : public IGraphicsDevice {
 public:
  explicit VirglDevice(hardware::VirtioGpu& gpu) : gpu_(gpu) {}
  ~VirglDevice();

  DeviceInfo info() const override {
    // (compute only when the host says it has it: a compute shader on a host without loses the whole context)
    return {"virgl (virtio-gpu)", DeviceKind::VirtualGpu, true, kMaxPushConstantBytes, kFeatureBlendState | kFeatureTexelAccess | kFeatureSystemInputs | kFeatureInstancing | kFeatureLines | kFeatureMultipleTargets | kFeatureFloatFormats | kFeatureOcclusionQueries | kFeatureStatisticsQueries | kFeatureTimestampQueries | kFeatureDepthResolve | kFeatureStreamOutput | kFeatureTextureKinds | kFeatureIndependentBlend | (gpu_.virgl_has_compute() ? kFeatureCompute : 0u) | (gpu_.virgl_has_geometry() ? kFeatureGeometryShaders : 0u) | (gpu_.virgl_has_tessellation() ? kFeatureTessellation : 0u) | kFeatureSrgbFormats | kFeatureUnorm16Formats | kFeatureIntegerFormats | kFeatureSnormFormats | (gpu_.virgl_max_samples() >= 2 ? kFeatureMultisample : 0u),
            (gpu_.virgl_max_samples() >= 2 ? 1u << 2 : 0u) | (gpu_.virgl_max_samples() >= 4 ? 1u << 4 : 0u)};
  }

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

  void interrupt_clock() override { clock_interrupted_ = true; }
  bool submit(ICommandBuffer* commands) override;
  bool present(IImage* image) override;

  // Used by the resource classes (not part of IGraphicsDevice).
  hardware::VirtioGpu& gpu() { return gpu_; }
  bool send_commands(const uint32_t* dwords, uint32_t count);  // SUBMIT_3D
  uint32_t new_handle() { return next_handle_++; }
  bool destroy_object(uint32_t handle);
  // The host-side sampler state object for `sampler`, made on first use (0 if the host refuses it).
  uint32_t sampler_state(const SamplerDesc& sampler);

 private:
  bool clock_interrupted_ = false;   // (until a disjoint query has said so)
  hardware::VirtioGpu& gpu_;
  uint32_t next_handle_ = 1;  // object handles share one namespace per context
  uint32_t sampler_states_[8] = {};   // by filter and address modes (3 bits), 0 until made
};

}  // namespace graphics
