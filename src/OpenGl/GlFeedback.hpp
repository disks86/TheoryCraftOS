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

// Selection and feedback (glRenderMode): the primitives after the vertex stage, instead of pixels.
#pragma once
#include <stdint.h>
#include "ObjectContainer/List.hpp"
#include "OpenGl/GlState.hpp"
#include "WebGl/Raster.hpp"

namespace opengl {

class GlApi;

class FeedbackSink final : public gpu::IPrimitiveSink {
 public:
  FeedbackSink(allocators::IAllocator& a, GlApi& api) : api_(api), values_(a), names_(a) {}
  void triangle(const gpu::RVertex& a, const gpu::RVertex& b, const gpu::RVertex& c) override;
  void line(const gpu::RVertex& a, const gpu::RVertex& b, bool first_of_strip) override;
  void point(const gpu::RVertex& a) override;

  // Selection: a hit in the current name record.
  void hit(float z);
  void write_hit_record();
  void reset(uint32_t mode, uint32_t capacity, uint64_t address, uint32_t feedback_type);
  uint32_t mode = 0x1C00;      // GL_RENDER
  uint32_t capacity = 0;
  uint64_t address = 0;
  uint32_t feedback_type = 0;
  uint32_t hits = 0;
  bool overflow = false;
  bool hit_pending = false;
  float hit_min = 1, hit_max = 0;
  void push(float v);
  List<float>& values() { return values_; }
  List<uint32_t>& names() { return names_; }
  // Where the fixed-function program keeps the colour and the texture coordinate among the varyings (-1: nowhere).
  int32_t color_at = -1, tex_at = -1;
  void pass_through(float token);

 private:
  void vertex_out(const gpu::RVertex& v);
  void window(const gpu::RVertex& v, float out[4]) const;
  void select_clipped(const gpu::RVertex* poly, uint32_t n);
  GlApi& api_;
  List<float> values_;      // what goes to the feedback or selection buffer, as floats (selection: the bits of unsigned values)
  List<uint32_t> names_;
};

}  // namespace opengl
