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

// Runs the draws of WebGL (and WebGPU) on the machine's 3D device: an webgl::IAccelerator over a graphics::IGraphicsDevice. The browser's canvas contexts keep
// their picture in a device color/depth image pair between draws and read it back only when the CPU needs it (see WebGl/Accelerator.hpp).
//
// A device that cannot take TGSI shaders (the VMware SVGA3D device wants Direct3D bytecode, the Raspberry Pi's V3D has fixed QPU programs) refuses every program,
// and the contexts then draw on the CPU: the choice is made per draw by whether the device accepted the shaders, never by a list of device names.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Graphics/Graphics3D/IGraphicsDevice.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"
#include "WebGl/Accelerator.hpp"

namespace graphics {

class WebGlAccelerator final : public webgl::IAccelerator {
 public:
  // `device` must be an accelerated one (SystemGraphics::accelerated()) and outlive this object, and nothing else may use it while a canvas draws.
  WebGlAccelerator(IGraphicsDevice& device, allocators::IAllocator& allocator);
  ~WebGlAccelerator();

  const char* name() const override { return device_.info().name; }
  webgl::AccelLimits limits() const override;
  uint32_t create_program(const char* vs_tgsi, const char* fs_tgsi, uint32_t attributes, uint32_t varyings, uint32_t textures) override;
  uint32_t create_program_stages(const char* vs_tgsi, const char* fs_tgsi, uint32_t attributes, uint32_t varyings, uint32_t textures, const webgl::AccelStages& stages) override;
  void destroy_program(uint32_t program) override;
  uint32_t create_texture(uint32_t w, uint32_t h) override;
  void destroy_texture(uint32_t texture) override;
  bool write_texture(uint32_t texture, ConstByteView rgba) override;
  uint32_t create_target(uint32_t w, uint32_t h) override;
  void destroy_target(uint32_t target) override;
  bool clear(uint32_t target, bool color, const float rgba[4], bool depth, float depth_value) override;
  bool draw(uint32_t target, uint32_t program, const webgl::AccelState& state, ConstByteView vertices, uint32_t vertex_count, ConstByteView constants) override;
  bool read_color(uint32_t target, ByteView rgba) override;

 private:
  // A pipeline of a program for one state: the pipeline state of an AccelState packed into a key.
  struct Variant {
    uint64_t key = 0;
    IPipeline* pipeline = nullptr;
  };
  struct Program {
    explicit Program(allocators::IAllocator& a) : variants(a), gs_text(a), tcs_text(a), tes_text(a) {}
    IShaderModule* vs = nullptr;
    IShaderModule* fs = nullptr;
    IShaderModule* gs = nullptr;    // the geometry stage, or nullptr
    IShaderModule* hs = nullptr;    // the tessellation stages (both or neither)
    IShaderModule* ds = nullptr;
    List<Variant> variants;
    uint32_t attributes = 0, varyings = 0, textures = 0;
    uint8_t gs_input = 2;           // graphics::GeometryPrimitive the geometry stage takes
    uint32_t patch_vertices = 0;    // with tessellation: the vertices of a patch
    String gs_text, tcs_text, tes_text;   // (the device may read a module's text again when it makes a pipeline: these copies live as long as the modules)
  };
  struct Texture {
    IImage* image = nullptr;
    uint32_t width = 0, height = 0;
  };
  struct Target {
    IImage* color = nullptr;
    IImage* depth = nullptr;
    uint32_t width = 0, height = 0;
  };

  void destroy_program_locked(Program& p);
  IPipeline* pipeline_for(Program& p, const webgl::AccelState& state);
  bool run_pass(Target& t, bool clear_color, const Vec4& color, bool clear_depth, float depth, Program* program, const webgl::AccelState* state, ConstByteView vertices,
                uint32_t vertex_count, ConstByteView constants);
  Program* program_at(uint32_t handle) { return handle && handle <= programs_.count() ? programs_[handle - 1].get() : nullptr; }
  Texture* texture_at(uint32_t handle) { return handle && handle <= textures_.count() ? textures_[handle - 1].get() : nullptr; }
  Target* target_at(uint32_t handle) { return handle && handle <= targets_.count() ? targets_[handle - 1].get() : nullptr; }

  IGraphicsDevice& device_;
  allocators::IAllocator& allocator_;
  kernel::Mutex lock_;
  List<UniquePointer<Program>> programs_;
  List<UniquePointer<Target>> targets_;
  List<UniquePointer<Texture>> textures_;
  ICommandBuffer* commands_ = nullptr;
  IBuffer* vertex_buffer_ = nullptr;
  uint64_t vertex_capacity_ = 0;
};

}  // namespace graphics
