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
constexpr uint32_t kAccelMaxTextures = 2;

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
  bool blend = false;        // source alpha over one minus source alpha
  // The textures the program samples, by sampler index (SAMP[i] of its TGSI): texture handles from create_texture() and how each is read. texture_count is what the
  // program was created with.
  uint32_t texture_count = 0;
  uint32_t textures[kAccelMaxTextures] = {};
  AccelSampler samplers[kAccelMaxTextures] = {};
};

class IAccelerator {
 public:
  virtual const char* name() const = 0;

  // Compiles a TGSI vertex/fragment pair: `attributes` float4 inputs (IN[0..]), `varyings` float4 outputs, and `textures` 2D samplers (SAMP[0..] of the fragment
  // shader, at most kAccelMaxTextures). 0: the device cannot run it.
  virtual uint32_t create_program(const char* vs_tgsi, const char* fs_tgsi, uint32_t attributes, uint32_t varyings, uint32_t textures) = 0;
  virtual void destroy_program(uint32_t program) = 0;

  // A texture of w x h texels (RGBA8, straight alpha, no mipmaps). 0: the device cannot have one (the default: programs with samplers are then refused).
  virtual uint32_t create_texture(uint32_t w, uint32_t h) {
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

  // A render target of w x h pixels (RGBA8 color, float depth). 0 on failure.
  virtual uint32_t create_target(uint32_t w, uint32_t h) = 0;
  virtual void destroy_target(uint32_t target) = 0;
  // Clears color (to rgba) and/or depth (to `depth`).
  virtual bool clear(uint32_t target, bool color, const float rgba[4], bool depth, float depth_value) = 0;
  // Draws `vertex_count` vertices (a triangle list) of `attributes` float4 each from `vertices`, with `constants` (a multiple of 16 bytes: the CONST rows).
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
  // The device texture holding `rgba` (w * h * 4 bytes) as texture `key`. 0: the device cannot.
  uint32_t get(IAccelerator& acc, uint64_t key, uint32_t w, uint32_t h, ConstByteView rgba);
  void forget(IAccelerator& acc, uint64_t key);
  void release(IAccelerator& acc);   // every texture goes

 private:
  struct Entry {
    uint64_t key, hash;
    uint32_t handle, w, h;
  };
  static constexpr uint64_t kMaxEntries = 64;   // more and the oldest goes
  List<Entry> entries_;
};

// The accelerator all canvas contexts use (nullptr: software only). It must outlive every context.
void set_accelerator(IAccelerator* accelerator);
IAccelerator* accelerator();

}  // namespace webgl
