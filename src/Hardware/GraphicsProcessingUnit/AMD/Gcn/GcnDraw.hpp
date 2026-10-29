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
// Not done: clipping (a triangle with a vertex at w <= 0 is dropped), depth and stencil, blending, flat interpolation, 2x2 quads (nothing here takes derivatives), and the pixel
// shader's input is the vertex shader's parameter of the same number.
#pragma once

#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnCompiler.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnEmu.hpp"
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

struct VertexAttribute {
  uint32_t components = 4;      // 1 to 4 floats
  uint32_t offset = 0;          // in the vertex
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
};

enum class DrawStatus : uint8_t { Ok, BadInput, OutOfMemory, ShaderFault };

struct DrawStats {
  uint32_t vertex_waves = 0, pixel_waves = 0, triangles = 0, pixels = 0;
};

DrawStatus draw(allocators::IAllocator& allocator, const DrawCall& call, Framebuffer& target, DrawStats* stats = nullptr);

}  // namespace gcn
