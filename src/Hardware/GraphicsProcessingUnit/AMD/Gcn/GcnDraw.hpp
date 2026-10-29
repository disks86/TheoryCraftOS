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

// A way to run the compiler's output: a draw call executed on the wave emulator. The vertex shader runs 64 vertices to a wave (the indices in v0), the triangles are set up and
// rasterized here (the part the card's fixed function hardware does: viewport transform, edge functions with the top-left rule, perspective-correct barycentrics i and j for
// the pixel shader's v_interp instructions), and the pixel shader runs 64 covered pixels to a wave. The shaders see the OS's own ABI (GcnCompiler.hpp): the constants, a table
// of buffer / image / sampler descriptors and the vertex buffer live in a small flat GPU address space of the harness.
//
// The back end is modelled too: blending of the colour into the target (the factors and functions of CB_BLEND0_CONTROL, a constant colour, a write mask), a depth buffer (32 bit
// float, the test functions of DB_DEPTH_CONTROL) and an 8 bit stencil with the test, the three operations and the masks of DB_STENCIL_CONTROL / DB_STENCILREFMASK. Pixels are tested
// before the pixel shader runs (the stencil test, then the depth test, each with its fail operation) and the depth and the stencil pass operation are written for the pixels that
// exported a colour, so a pixel the shader discards changes neither. Triangles are drawn in order, so blending and the tests see the earlier triangles' pixels. Textures come
// either from an ITextureSource by unit (the shader's T# is then a unit number) or from real T# / S# descriptors read through `texel_memory` (GcnTexture.hpp).
//
// Not done: clipping against the frustum (a triangle with a vertex at w <= 0 is dropped; a pixel whose depth is outside 0 to 1 is not drawn), back face stencil, depth bounds, dual
// source blending, logic ops, flat interpolation, 2x2 quads (nothing here takes derivatives), and the pixel shader's input k reads the vertex shader's parameter `ps_param[k]`
// (SPI_PS_INPUT_CNTL_k.OFFSET; the parameter of the same number by default).
#pragma once

#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnCompiler.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnEmu.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnTexture.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"

namespace gcn {

// The texture a pixel shader samples (image_sample: the T# names the unit).
class ITextureSource {
 public:
  virtual void sample(uint32_t unit, float u, float v, float out[4]) = 0;

 protected:
  ITextureSource() = default;
  ~ITextureSource() = default;
};

// The colour target: float RGBA, row 0 at the top.
class Framebuffer {
 public:
  Framebuffer(allocators::IAllocator& allocator, uint32_t width, uint32_t height);
  bool valid() const { return valid_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  void clear(float r, float g, float b, float a);
  const float* pixel(uint32_t x, uint32_t y) const { return x < width_ && y < height_ ? &rgba_[(static_cast<uint64_t>(y) * width_ + x) * 4] : nullptr; }
  float* pixel(uint32_t x, uint32_t y) { return x < width_ && y < height_ ? &rgba_[(static_cast<uint64_t>(y) * width_ + x) * 4] : nullptr; }

 private:
  List<float> rgba_;
  uint32_t width_, height_;
  bool valid_ = true;
};

// 32 bit float depth and 8 bit stencil planes, row 0 at the top.
class DepthBuffer {
 public:
  DepthBuffer(allocators::IAllocator& allocator, uint32_t width, uint32_t height);
  bool valid() const { return valid_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  void clear(float depth, uint8_t stencil);
  float& depth(uint32_t x, uint32_t y) { return z_[static_cast<uint64_t>(y) * width_ + x]; }
  float depth(uint32_t x, uint32_t y) const { return z_[static_cast<uint64_t>(y) * width_ + x]; }
  uint8_t& stencil(uint32_t x, uint32_t y) { return s_[static_cast<uint64_t>(y) * width_ + x]; }
  uint8_t stencil(uint32_t x, uint32_t y) const { return s_[static_cast<uint64_t>(y) * width_ + x]; }

 private:
  List<float> z_;
  List<uint8_t> s_;
  uint32_t width_, height_;
  bool valid_ = true;
};

// The numbers are the hardware's (the BlendOp, CombFunc, CompareFrag and StencilOp enumerations of the register headers), so the state in registers maps straight onto this.
struct BlendState {
  bool enable = false;
  uint8_t color_src = 1, color_dst = 0, color_op = 0;           // factors: 0 zero, 1 one, 2 src colour, 3 1 - src colour, 4 src alpha, 5 1 - src alpha, 6 dst alpha, 7 1 - dst alpha,
                                                                 // 8 dst colour, 9 1 - dst colour, 10 src alpha saturate, 13 constant colour, 14 1 - constant colour,
                                                                 // 19 constant alpha, 20 1 - constant alpha; operations: 0 src + dst, 1 src - dst, 2 min, 3 max, 4 dst - src
  bool separate_alpha = false;                                  // else the alpha uses the colour's factors and operation
  uint8_t alpha_src = 1, alpha_dst = 0, alpha_op = 0;
  float constant[4] = {0, 0, 0, 0};                             // CB_BLEND_RED .. ALPHA
  uint8_t write_mask = 0xF;                                     // the colour channels written (CB_TARGET_MASK for target 0): bit 0 red .. bit 3 alpha
  static bool supported_factor(uint32_t f) { return f <= 10 || f == 13 || f == 14 || f == 19 || f == 20; }
};

struct DepthStencilState {
  bool depth_test = false, depth_write = false;
  uint8_t depth_func = 7;                                       // 0 never, 1 less, 2 equal, 3 less or equal, 4 greater, 5 not equal, 6 greater or equal, 7 always
  bool stencil_test = false;
  uint8_t stencil_func = 7;
  uint8_t stencil_ref = 0, stencil_mask = 0xFF, stencil_write_mask = 0xFF, stencil_op_value = 1;
  // 0 keep, 1 zero, 2 ones, 3 replace with the reference, 4 replace with the operation value, 5 add (clamp), 6 subtract (clamp), 7 invert, 8 add (wrap), 9 subtract (wrap)
  uint8_t fail_op = 0, zpass_op = 0, zfail_op = 0;
};

struct VertexAttribute {
  uint32_t components = 4;      // 1 to 4 floats
  uint32_t offset = 0;          // in the vertex
};

struct Viewport {
  bool set = false;                    // false: the whole target, as PA_CL_VPORT_* with the scale half the size
  float x_scale = 0, x_offset = 0, y_scale = 0, y_offset = 0;       // screen = ndc * scale + offset
  float z_scale = 0.5f, z_offset = 0.5f;                            // the depth, from the OpenGL clip range -1 to 1 onto 0 to 1 unless set otherwise
};

struct Scissor {
  bool set = false;
  uint32_t left = 0, top = 0, right = 0, bottom = 0;                // pixels [left, right) x [top, bottom)
};

struct DrawCall {
  ArrayView<const uint32_t> vertex_code, pixel_code;
  ShaderInfo vertex_info, pixel_info;
  ArrayView<const float> vertex_constants, pixel_constants;          // rows of four floats
  ArrayView<const uint32_t> vertex_data;                            // the vertex buffer
  uint32_t vertex_stride = 0;                                       // bytes
  VertexAttribute attributes[abi::kMaxAttributes];                  // attribute i is the vertex shader's IN[i]
  ArrayView<const uint32_t> indices;                                // a triangle list
  ITextureSource* textures = nullptr;
  // Texture descriptors: the T# and S# of unit i go to the descriptor table (when bit i of `descriptors_set` is set) and are sampled through `texel_memory`.
  ITexelMemory* texel_memory = nullptr;
  uint32_t descriptors_set = 0;
  uint32_t image_words[abi::kMaxSamplers][8] = {};
  uint32_t sampler_words[abi::kMaxSamplers][4] = {};
  BlendState blend;
  DepthStencilState depth_stencil;
  DepthBuffer* depth = nullptr;                                     // required when the depth or stencil test is on
  Viewport viewport;
  Scissor scissor;
  uint8_t ps_param[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
};

enum class DrawStatus : uint8_t { Ok, BadInput, OutOfMemory, ShaderFault };

struct DrawStats {
  uint32_t vertex_waves = 0, pixel_waves = 0, triangles = 0, pixels = 0;
};

DrawStatus draw(allocators::IAllocator& allocator, const DrawCall& call, Framebuffer& target, DrawStats* stats = nullptr);

}  // namespace gcn
