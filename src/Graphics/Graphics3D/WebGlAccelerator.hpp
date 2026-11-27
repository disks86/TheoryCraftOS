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
  uint32_t create_texture_float(uint32_t w, uint32_t h) override;
  bool write_texture_float(uint32_t texture, ConstByteView rgba32f) override;
  void destroy_texture(uint32_t texture) override;
  bool write_texture(uint32_t texture, ConstByteView rgba) override;
  uint32_t create_target(uint32_t w, uint32_t h) override;
  void destroy_target(uint32_t target) override;
  bool clear(uint32_t target, bool color, const float rgba[4], bool depth, float depth_value) override;
  bool draw(uint32_t target, uint32_t program, const webgl::AccelState& state, ConstByteView vertices, uint32_t vertex_count, ConstByteView constants) override;
  bool read_color(uint32_t target, ByteView rgba) override;
  bool write_color(uint32_t target, ConstByteView rgba) override;
  bool write_depth(uint32_t target, ConstByteView depth) override;
  bool read_depth(uint32_t target, ByteView depth) override;
  uint32_t create_image(uint32_t w, uint32_t h, webgl::AccelFormat format, uint32_t levels) override;
  bool write_image(uint32_t image, uint32_t level, ConstByteView pixels) override;
  uint32_t create_layered(uint32_t w, uint32_t h, uint32_t layers, webgl::AccelKind kind) override;
  bool write_layer(uint32_t image, uint32_t layer, ConstByteView rgba) override;
  bool read_image(uint32_t image, ByteView pixels) override;
  bool takes_blocks(webgl::AccelBlocks blocks) const override;
  uint32_t create_texture_levels(const webgl::AccelTextureDesc& desc) override;
  bool write_level(uint32_t texture, uint32_t level, uint32_t layer, ConstByteView data) override;
  bool clear_images(const webgl::AccelAttachments& a, bool color, const float rgba[4], bool depth, float depth_value) override;
  bool draw_images(const webgl::AccelAttachments& a, uint32_t program, const webgl::AccelState& state, ConstByteView vertices, uint32_t vertex_count, ConstByteView constants) override;

 private:
  // A pipeline of a program for one state: the pipeline state of an AccelState packed into a key.
  struct Variant {
    uint64_t key = 0, key2 = 0;
    float bias_slope = 0, bias_units = 0;
    bool per_target = false;
    webgl::AccelState::TargetBlend targets[webgl::kAccelMaxColors] = {};
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
  // A texture (create_texture / create_texture_float, sampled only) or a device image (create_image, which draws may also render into).
  struct Texture {
    IImage* image = nullptr;
    uint32_t width = 0, height = 0;
    webgl::AccelFormat format = webgl::AccelFormat::Rgba8;
    uint32_t levels = 1;
    bool renderable = false;   // a colour or depth attachment as well
    uint32_t layers = 0;       // create_layered: its layers (0: a 2D texture)
    // create_texture_levels: a sampled texture of `kind` with levels (write_level), of `blocks` when not None
    bool sampled_levels = false;
    webgl::AccelKind kind = webgl::AccelKind::Plain;
    webgl::AccelBlocks blocks = webgl::AccelBlocks::None;
  };
  uint32_t make_texture(uint32_t w, uint32_t h, webgl::AccelFormat format, uint32_t levels, bool renderable);
  uint32_t add_texture(UniquePointer<Texture>&& t);
  struct Target {
    IImage* color = nullptr;
    IImage* depth = nullptr;
    uint32_t width = 0, height = 0;
  };
  // The render pass of draw_images / clear_images: the images of `a`, which must exist, be renderable and have one size (a depth-only pass gets a colour image of
  // its own: the device's passes have one, see blank_color). False if not.
  bool images_pass(const webgl::AccelAttachments& a, RenderPassBegin* pass, uint32_t* width, uint32_t* height);
  IImage* blank_color(uint32_t w, uint32_t h);
  bool depth_planes(IImage& image, bool stencil, ConstByteView in, ByteView* out);   // (see the .cpp)
  struct Blank {
    IImage* image = nullptr;
    uint32_t width = 0, height = 0;
  };
  List<Blank> blanks_;   // (the colour images of depth-only passes, one per size)

  void destroy_program_locked(Program& p);
  IPipeline* pipeline_for(Program& p, const webgl::AccelState& state, uint32_t color_count = 1);
  bool run_pass(Target& t, bool clear_color, const Vec4& color, bool clear_depth, float depth, Program* program, const webgl::AccelState* state, ConstByteView vertices,
                uint32_t vertex_count, ConstByteView constants);
  // One pass: `pass` (its clears as they are) and, with a program, one draw of `color_count` colour outputs into a width x height target.
  bool run_pass_on(const RenderPassBegin& pass, uint32_t width, uint32_t height, uint32_t color_count, Program* program, const webgl::AccelState* state, ConstByteView vertices,
                   uint32_t vertex_count, ConstByteView constants);
  bool draw_checks(Program& p, const webgl::AccelState& state, ConstByteView vertices, uint32_t vertex_count, ConstByteView constants);
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
