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

// An IGraphicsDevice on the Radeon RX Vega M's graphics engine, built on AmdVegaDraw (a draw staged into the card's draw area, run from an indirect buffer on the graphics ring,
// and read back). The built-in shaders (and any TGSI the GCN compiler takes) are compiled when the module is made; a pipeline carries the translated blend, depth, stencil and cull state.
//
// How it works, and what it costs: the images live in system memory (a colour image as 0xAARRGGBB, a depth image as floats and an optional stencil plane), because AmdVegaDraw stages a
// draw's data into the card's area and copies the planes back; each draw goes up with the target's, the depth plane's and the textures' current contents as its starting point and the
// planes that come back are merged into the image (only inside the scissor). A render pass of one draw, like the 3D test's cube, costs one round trip; a pass of hundreds of draws over a
// big target (a desktop) costs hundreds, so this device is for 3D work on a target of modest size, not for the 2D renderer of a display.
//
// What it does not take (a pipeline or submit that needs it is refused, so a caller falls back to the software device): float formats, mip chains and texture kinds other than 2D, more
// than one colour target, lines and points, instancing, occlusion queries, independent blending, a viewport other than the whole target, and a draw with a vertex at or behind the
// eye (the emulator the command processor model runs does not clip). present() is refused: the card shows nothing through this device.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Graphics/Graphics3D/CommandRecorder.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdVegaDraw.hpp"

namespace graphics {

class VegaGraphicsDevice final : public IGraphicsDevice {
 public:
  VegaGraphicsDevice(allocators::IAllocator& allocator, hardware::AmdVegaM& card);
  ~VegaGraphicsDevice();
  VegaGraphicsDevice(const VegaGraphicsDevice&) = delete;
  VegaGraphicsDevice& operator=(const VegaGraphicsDevice&) = delete;

  // The card is up and has room for draws.
  bool usable() const { return usable_; }
  // Draws the card has run, and the last one's failure ("" when it worked).
  uint64_t hardware_draws() const { return draws_; }
  const char* last_error() const { return error_; }

  DeviceInfo info() const override { return {"Radeon RX Vega M (graphics engine)", DeviceKind::Gpu, true, 128, kFeatureBlendState}; }

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
  bool present(IImage*) override { return false; }

 private:
  struct Scratch;
  struct DrawArgs;
  bool run_draw(const DrawArgs& args);
  bool fail(const char* why) {
    error_ = why;
    return false;
  }
  allocators::IAllocator& allocator_;
  hardware::AmdVegaM& card_;
  uint64_t area_offset_ = 0, area_bytes_ = 0;
  bool usable_ = false;
  uint64_t draws_ = 0;
  const char* error_ = "";
  Scratch* scratch_ = nullptr;   // (buffers kept between draws; defined in the .cpp)
};

}  // namespace graphics
