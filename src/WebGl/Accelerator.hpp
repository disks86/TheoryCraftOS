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

// The 3D hardware behind WebGL and WebGPU. When the machine has a GPU whose 3D engine the OS can drive (graphics::SystemGraphics::accelerated()), the OS installs
// an IAccelerator here and the canvas contexts hand it the draws it can run (shaders that translate to TGSI, see TgsiTranslator.hpp, and state the device has);
// every other draw, and every context when there is no accelerator, runs on the software rasterizer in this directory. A context never depends on the choice
// for correctness: the accelerator may refuse any call, and the context then does that work itself.
//
// An accelerator owns render targets (a color image and a depth image). The picture stays on the device between draws - nothing is read back per draw - and
// the context reads it back with read_color() only when something on the CPU needs it (presenting the canvas, readPixels, a draw the device cannot do).
// The device's color image can only be read, not written, so a context uses a target only while the target is at least as new as its own copy of the image.
//
// Calls come from whichever thread runs the page's script; an implementation serialises them.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"

namespace webgl {

// How a texture is read: nearest or linear filtering (there are no mipmaps), and for each axis wrap around or take the edge texel.
struct AccelSampler {
  bool linear = false;
  bool repeat_s = true, repeat_t = true;
};
constexpr uint32_t kAccelMaxTextures = 16;

// What an accelerator's programs may have (IAccelerator::limits): CONST rows, vertex attributes, varyings and textures. The defaults are what every device has.
struct AccelLimits {
  uint32_t const_rows = 8, attributes = 4, varyings = 4, textures = 2;
  bool blend_state = false;   // AccelState's custom blending, colour mask and depth functions
  bool lines = false;         // AccelState::topology lines and points
  bool geometry = false;      // programs with a geometry stage (AccelStages::gs), and the adjacency topologies
  bool tessellation = false;  // programs with tessellation stages (AccelStages::tcs / tes), and patches
  bool clip_distances = false;  // AccelState::clip_mask: the CLIPDIST outputs of the last stage cut the primitives
  bool float_textures = false;  // create_texture_float (what a depth texture is sent as)
  uint32_t control_varyings = 0;  // tessellation: the registers per control point from the control to the evaluation stage (0: `varyings`)
};

// The stages of a program between the vertex and the fragment shader (create_program_stages), as TGSI text, and what the device must know of them. The numbers are the
// device's: graphics::GeometryPrimitive, GeometryTopology, TessDomain, TessPartitioning and TessOutput (IGraphicsDevice.hpp).
struct AccelStages {
  const char* gs = nullptr;    // a geometry shader, or nullptr
  uint8_t gs_input = 2, gs_output = 2;   // GeometryPrimitive (2 triangles), GeometryTopology (2 a triangle strip)
  uint32_t gs_max_vertices = 0, gs_invocations = 1;
  const char* tcs = nullptr;   // the control and evaluation shaders: both or neither
  const char* tes = nullptr;
  uint32_t patch_vertices = 0;   // the vertices of a patch the draws make (the control shader's input), and of the control shader's output patch
  uint32_t tcs_vertices = 0;
  uint8_t tess_domain = 1, tess_partitioning = 0, tess_output = 1;   // TessDomain (1 triangles), TessPartitioning (0 integer), TessOutput (1 counter-clockwise triangles)
};

struct AccelState {
  // Window coordinates as in GL: origin at the bottom left, rows counted from there (the device image is stored the same way, see GlAccel.cpp).
  float vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
  float depth_near = 0, depth_far = 1;
  bool scissor = false;
  int32_t sc_x = 0, sc_y = 0;
  uint32_t sc_w = 0, sc_h = 0;
  uint8_t cull = 0;          // 0 none, 1 front faces culled, 2 back faces culled
  bool front_ccw = true;     // counter-clockwise in GL window coordinates
  bool depth_test = false;   // passes if closer or equal
  bool depth_write = false;
  bool blend = false;        // source alpha over one minus source alpha (custom_blend false)
  // AccelLimits::blend_state: blending by these factors and equations (enabled by `blend`), the channels written (bit 0 red .. bit 3 alpha) and what the depth test
  // compares with. The numbers are the device's (graphics::BlendFactor, BlendOp, CompareOp in IGraphicsDevice.hpp); custom_blend false is source over destination,
  // every channel written and a depth test that passes when closer or equal.
  bool custom_blend = false;
  uint8_t src_rgb = 1, dst_rgb = 0, src_alpha = 1, dst_alpha = 0;   // graphics::BlendFactor
  uint8_t op_rgb = 0, op_alpha = 0;                                 // graphics::BlendOp
  uint8_t color_mask = 0xF;
  uint8_t depth_compare = 3;                                        // graphics::CompareOp (LessEqual)
  // AccelLimits::lines: what the vertices make, 0 a list of triangles, 1 of lines, 2 of points (one pixel wide); AccelLimits::tessellation: 3 a list of patches of
  // patch_vertices vertices each; AccelLimits::geometry: 4 a list of lines with adjacency (4 vertices each), 5 of triangles with adjacency (6 each). A program with a
  // geometry stage takes the primitives its gs_input names, one with tessellation stages only patches.
  uint8_t topology = 0;
  uint32_t patch_vertices = 0;
  // AccelLimits::clip_distances: the clip distances the program's last stage writes (CLIPDIST[0] .xyzw 0 .. 3, CLIPDIST[1] 4 .. 7) that cut its primitives, bit i distance i.
  uint8_t clip_mask = 0;
  // The textures the program samples, by sampler index (SAMP[i] of its TGSI): texture handles from create_texture() and how each is read. texture_count is what the
  // program was created with.
  uint32_t texture_count = 0;
  uint32_t textures[kAccelMaxTextures] = {};
  AccelSampler samplers[kAccelMaxTextures] = {};
};

class IAccelerator {
 public:
  virtual const char* name() const = 0;
  virtual AccelLimits limits() const { return AccelLimits(); }

  // Compiles a TGSI vertex/fragment pair: `attributes` float4 inputs (IN[0..]), `varyings` float4 outputs, and `textures` 2D samplers (SAMP[0..] of the fragment
  // shader, at most limits().textures). 0: the device cannot run it.
  virtual uint32_t create_program(const char* vs_tgsi, const char* fs_tgsi, uint32_t attributes, uint32_t varyings, uint32_t textures) = 0;
  // The same with geometry or tessellation stages (`varyings`: the fragment shader's inputs). The default takes only programs without them.
  virtual uint32_t create_program_stages(const char* vs_tgsi, const char* fs_tgsi, uint32_t attributes, uint32_t varyings, uint32_t textures, const AccelStages& stages) {
    if (stages.gs || stages.tcs || stages.tes) return 0;
    return create_program(vs_tgsi, fs_tgsi, attributes, varyings, textures);
  }
  virtual void destroy_program(uint32_t program) = 0;

  // A texture of w x h texels (RGBA8, straight alpha, no mipmaps). 0: the device cannot have one (the default: programs with samplers are then refused).
  virtual uint32_t create_texture(uint32_t w, uint32_t h) {
    (void)w;
    (void)h;
    return 0;
  }
  // A texture of w x h texels of four 32-bit floats (AccelLimits::float_textures), read exactly as written with nearest filtering. 0: the device cannot have one.
  virtual uint32_t create_texture_float(uint32_t w, uint32_t h) {
    (void)w;
    (void)h;
    return 0;
  }
  virtual void destroy_texture(uint32_t texture) { (void)texture; }
  // Replaces the contents: w * h * 4 bytes, R, G, B, A of each texel, the first row (t = 0) first.
  virtual bool write_texture(uint32_t texture, ConstByteView rgba) {
    (void)texture;
    (void)rgba;
    return false;
  }
  // The same for a float texture: w * h * 16 bytes, R, G, B, A of each texel as little-endian floats.
  virtual bool write_texture_float(uint32_t texture, ConstByteView rgba32f) {
    (void)texture;
    (void)rgba32f;
    return false;
  }

  // A render target of w x h pixels (RGBA8 color, float depth). 0 on failure.
  virtual uint32_t create_target(uint32_t w, uint32_t h) = 0;
  virtual void destroy_target(uint32_t target) = 0;
  // Clears color (to rgba) and/or depth (to `depth`).
  virtual bool clear(uint32_t target, bool color, const float rgba[4], bool depth, float depth_value) = 0;
  // Draws `vertex_count` vertices (a list of AccelState::topology: triangles by default) of `attributes` float4 each from `vertices`, with `constants` (a multiple of 16 bytes: the CONST rows).
  virtual bool draw(uint32_t target, uint32_t program, const AccelState& state, ConstByteView vertices, uint32_t vertex_count, ConstByteView constants) = 0;
  // Copies the color image to `rgba` (width * height * 4 bytes: R, G, B, A of each pixel), the rows in the order the GL window has them - the bottom row first.
  virtual bool read_color(uint32_t target, ByteView rgba) = 0;

 protected:
  ~IAccelerator() = default;
};

// The device copies of the textures a context (or a WebGPU device) sampled. A texture is named by a key of the caller's; its texels go to the device when it is first
// used and again when they changed (a texture changes in too many places for each to say so, so a hash of the texels is read at each use).
class AccelTextureCache {
 public:
  explicit AccelTextureCache(allocators::IAllocator& a) : entries_(a) {}
  // The device texture holding `rgba` (w * h * 4 bytes; with `floats`, w * h * 16 bytes of floats: a float texture) as texture `key`. 0: the device cannot.
  uint32_t get(IAccelerator& acc, uint64_t key, uint32_t w, uint32_t h, ConstByteView rgba, bool floats = false);
  void forget(IAccelerator& acc, uint64_t key);
  void release(IAccelerator& acc);   // every texture goes

 private:
  struct Entry {
    uint64_t key, hash;
    uint32_t handle, w, h;
    bool floats;
  };
  static constexpr uint64_t kMaxEntries = 64;   // more and the oldest goes
  List<Entry> entries_;
};

// The accelerator all canvas contexts use (nullptr: software only). It must outlive every context.
void set_accelerator(IAccelerator* accelerator);
IAccelerator* accelerator();

}  // namespace webgl
