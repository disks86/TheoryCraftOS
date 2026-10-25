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

// The rasterizer shared by WebGL and WebGPU: clips primitives in clip space, maps them to the window, and calls a sink for every pixel they cover (with the
// varyings interpolated, perspective-correct unless flat). Window coordinates have their origin at the bottom left with y up (GL's); WebGPU callers flip y
// in the vertex positions they pass (or in the viewport) as they need.
#pragma once
#include <stdint.h>

namespace gpu {

constexpr uint32_t kMaxVarying = 64;   // scalars per vertex

struct RVertex {
  float pos[4] = {0, 0, 0, 1};      // clip space
  float var[kMaxVarying] = {};
  float point_size = 1;
};

struct RasterState {
  int32_t vp_x = 0, vp_y = 0, vp_w = 1, vp_h = 1;   // viewport
  float depth_near = 0, depth_far = 1;
  int32_t sc_x = 0, sc_y = 0, sc_w = 0x7FFFFFF, sc_h = 0x7FFFFFF;   // scissor, in window coordinates (always applied, set it large to disable)
  int32_t target_w = 1, target_h = 1;
  uint8_t cull = 0;                  // 0 none, 1 front, 2 back, 3 both
  bool front_ccw = true;
  bool offset = false;
  float offset_factor = 0, offset_units = 0;
  uint32_t nvar = 0;
  bool flat[kMaxVarying] = {};
  bool flat_first = false;           // the provoking vertex is the first (WebGPU) rather than the last (GL)
  float max_point_size = 64;
  float line_width = 1;
  bool depth_clip = true;            // clip against near/far (WebGPU's unclipped_depth turns it off)
  bool clip_z_zero_to_one = false;   // WebGPU clip space has 0 <= z <= w
};

struct TriSetup;

struct Fragment {
  int32_t x = 0, y = 0;
  float z = 0;
  bool front = true;
  const float* varyings = nullptr;
  float coord[4] = {0, 0, 0, 1};     // gl_FragCoord
  float point_coord[2] = {0, 0};
  bool is_point = false;
  const TriSetup* setup = nullptr;
  uint32_t coverage_samples = 1;
  // The varyings as they would be at the centre of another pixel (for derivatives).
  void varyings_at(int32_t px, int32_t py, float* out, uint32_t n) const;
};

class IFragmentSink {
 public:
  virtual void fragment(const Fragment& f) = 0;

 protected:
  ~IFragmentSink() = default;
};

void draw_triangle(const RasterState& rs, const RVertex& a, const RVertex& b, const RVertex& c, IFragmentSink& sink);
void draw_line(const RasterState& rs, const RVertex& a, const RVertex& b, IFragmentSink& sink);
void draw_point(const RasterState& rs, const RVertex& a, IFragmentSink& sink);

// The pixels touched since the last call (a bounding box), for presenting only what changed. Reset by clear_touched().
struct TouchBox {
  int32_t x0 = 0x7FFFFFFF, y0 = 0x7FFFFFFF, x1 = -1, y1 = -1;
  void add(int32_t x, int32_t y) {
    if (x < x0) x0 = x;
    if (y < y0) y0 = y;
    if (x > x1) x1 = x;
    if (y > y1) y1 = y;
  }
  bool empty() const { return x1 < x0; }
};

}  // namespace gpu
