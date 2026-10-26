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
#include "Utility/UniquePointer.hpp"
#include "WebGl/Accelerator.hpp"

namespace graphics {

class WebGlAccelerator final : public webgl::IAccelerator {
 public:
  // `device` must be an accelerated one (SystemGraphics::accelerated()) and outlive this object, and nothing else may use it while a canvas draws.
  WebGlAccelerator(IGraphicsDevice& device, allocators::IAllocator& allocator);
  ~WebGlAccelerator();

  const char* name() const override { return device_.info().name; }
  uint32_t create_program(const char* vs_tgsi, const char* fs_tgsi, uint32_t attributes, uint32_t varyings) override;
  void destroy_program(uint32_t program) override;
  uint32_t create_target(uint32_t w, uint32_t h) override;
  void destroy_target(uint32_t target) override;
  bool clear(uint32_t target, bool color, const float rgba[4], bool depth, float depth_value) override;
  bool draw(uint32_t target, uint32_t program, const webgl::AccelState& state, ConstByteView vertices, uint32_t vertex_count, ConstByteView constants) override;
  bool read_color(uint32_t target, ByteView rgba) override;

 private:
  static constexpr uint32_t kVariants = 64;   // cull (3) x front face (2) x depth test x depth write x blend, as bits

  struct Program {
    IShaderModule* vs = nullptr;
    IShaderModule* fs = nullptr;
    IPipeline* pipelines[kVariants] = {};
    uint32_t attributes = 0, varyings = 0;
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
  Target* target_at(uint32_t handle) { return handle && handle <= targets_.count() ? targets_[handle - 1].get() : nullptr; }

  IGraphicsDevice& device_;
  allocators::IAllocator& allocator_;
  kernel::Mutex lock_;
  List<UniquePointer<Program>> programs_;
  List<UniquePointer<Target>> targets_;
  ICommandBuffer* commands_ = nullptr;
  IBuffer* vertex_buffer_ = nullptr;
  uint64_t vertex_capacity_ = 0;
};

}  // namespace graphics
