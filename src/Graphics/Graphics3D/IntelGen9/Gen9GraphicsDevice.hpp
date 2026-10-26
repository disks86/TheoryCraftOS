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
#include "Hardware/GraphicsProcessingUnit/Intel/IntelRenderBatch.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelVcsDriver.hpp"

namespace graphics {

// The 3D device of the Intel Gen9.5 integrated GPUs (Kaby Lake, Coffee Lake, Comet Lake graphics; the HD Graphics 630 of the Kaby Lake-G in a Dell XPS 15 9575):
// IGraphicsDevice on the render engine through the execlist driver (VcsDriver in its render mode) and the batch builder (intel_gt::gen9_3d::RenderBatch). Buffers and
// images live in GPU memory mapped into the engine's address space. A command buffer is recorded as usual; at submit each render pass becomes a series of jobs: a solid
// rectangle for the clear, then the draws' triangles.
//
// What it can run: the built-in coloured-vertex pipeline (BuiltinShaders), whose shader modules name pixel kernels (there is no shader compiler for the EUs), with a
// vertex layout of a float3 at location 0 and a float4 at location 1 and the MVP matrix as the first 64 bytes of the push constants. Any other pipeline is refused by
// create_graphics_pipeline() (nullptr), so a caller can fall back to the software device. The engine has no vertex shader in these jobs: the CPU transforms the vertices,
// clips the triangles (near plane and a guard band), applies the viewport, culls, and hands the engine window-space triangles; the engine interpolates the colour and
// blends. Differences from the software device: colour is interpolated linearly in screen space (not perspective-correct), there is no depth buffer (depth images and
// pipelines that test or write depth are refused), and only B8G8R8A8 and R8G8B8A8 images up to 4096 x 4096 exist.
// Nothing here has run on a real GPU (see docs/DellXps9575.md).
class Gen9GraphicsDevice final : public IGraphicsDevice {
 public:
  // `engine` is the render-mode driver, already initialised; `memory` the GPU memory it was built with; `display` shows presented images (may be null).
  Gen9GraphicsDevice(allocators::IAllocator& allocator, hardware::intel_gt::VcsDriver& engine, hardware::intel_gt::IGpuMemory& memory, hardware::IGraphicsProcessingUnit* display)
      : allocator_(allocator), engine_(engine), memory_(memory), display_(display) {}
  ~Gen9GraphicsDevice();

  // Takes the engine's lease and allocates the job buffers. False if the driver is not ready, someone else holds the lease or memory ran out; nothing works then.
  bool initialize();
  bool ready() const { return !image_.empty(); }

  DeviceInfo info() const override { return {"Intel Gen9 render engine", DeviceKind::Gpu, true}; }

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

  // The graphics address range buffers and images are mapped into (exposed for tests).
  static constexpr uint32_t kFirstAddress = hardware::intel_gt::VcsDriver::kFirstBufferGpuAddress;

 private:
  struct DrawRecord;
  class AddressSpace {
   public:
    AddressSpace() { ranges_[0] = {hardware::intel_gt::VcsDriver::kFirstBufferGpuAddress, 0xFFFFF000u - hardware::intel_gt::VcsDriver::kFirstBufferGpuAddress}; count_ = 1; }
    // A page-aligned range of `bytes` (a multiple of 4096) and its address, or false.
    bool take(uint32_t bytes, uint32_t* address);
    void give(uint32_t address, uint32_t bytes);

   private:
    struct Range { uint32_t start, length; };
    static constexpr uint32_t kMaxRanges = 64;
    Range ranges_[kMaxRanges];
    uint32_t count_ = 0;
  };

  bool allocate_region(uint64_t bytes, hardware::intel_gt::GpuBuffer* buffer, uint32_t* address);
  void free_region(const hardware::intel_gt::GpuBuffer& buffer, uint32_t address);
  bool run_pass(const RenderPassBegin& pass, const List<DrawRecord>& draws);
  bool run_job(const hardware::intel_gt::gen9_3d::DrawState& state, uint32_t vertices);
  bool run_draw(const DrawRecord& draw, const hardware::intel_gt::gen9_3d::DrawState& base_state, const Extent2D& target);

  allocators::IAllocator& allocator_;
  hardware::intel_gt::VcsDriver& engine_;
  hardware::intel_gt::IGpuMemory& memory_;
  hardware::IGraphicsProcessingUnit* display_;
  AddressSpace addresses_;
  ArrayView<uint32_t> image_;                              // the batch image of a job
  ArrayView<hardware::intel_gt::gen9_3d::Vertex> staging_;  // the window-space triangles of the job being filled
  uint32_t staged_ = 0;
  bool leased_ = false;
};

}  // namespace graphics
