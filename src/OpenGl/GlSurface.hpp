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

// The drawable of a window or pbuffer: colour, depth and stencil images (and accumulation) that any context made current on it draws to. Lives in the kernel, one per window.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "WebGl/GlContext.hpp"

namespace opengl {

struct SurfaceConfig {
  uint32_t width = 1, height = 1;
  bool double_buffered = true;
  uint32_t depth_bits = 24, stencil_bits = 8, alpha_bits = 8, accum_bits = 64;
};

class GlSurface {
 public:
  GlSurface(allocators::IAllocator& a, const SurfaceConfig& config);
  bool valid() const { return valid_; }
  uint32_t width() const { return config_.width; }
  uint32_t height() const { return config_.height; }
  bool double_buffered() const { return config_.double_buffered; }
  const SurfaceConfig& config() const { return config_; }
  webgl::Image& front() { return color_[front_]; }
  webgl::Image& back() { return config_.double_buffered ? color_[1 - front_] : color_[front_]; }
  webgl::Image& depth_stencil() { return depth_; }
  ArrayView<float> accum() { return accum_.view(); }
  // Changes each time images move (swap, resize): contexts look at it before they run.
  uint64_t generation() const { return generation_; }
  // After a resize the contents are undefined. False if the memory is not there (the old size stays).
  bool resize(uint32_t width, uint32_t height);
  void swap();
  // A context finished drawing to a buffer that shows (a single-buffered window, glDrawBuffer(GL_FRONT)): the window should show it now.
  void mark_flushed() { flushed_ = true; }
  bool take_flushed() { const bool f = flushed_; flushed_ = false; return f; }

 private:
  bool allocate(uint32_t width, uint32_t height);
  allocators::IAllocator& a_;
  SurfaceConfig config_;
  webgl::Image color_[2];
  webgl::Image depth_;
  List<float> accum_;
  uint32_t front_ = 0;
  uint64_t generation_ = 1;
  bool valid_ = false;
  bool flushed_ = false;
};

}  // namespace opengl
