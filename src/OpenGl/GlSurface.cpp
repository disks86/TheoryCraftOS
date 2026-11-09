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

#include "OpenGl/GlSurface.hpp"

namespace opengl {

GlSurface::GlSurface(allocators::IAllocator& a, const SurfaceConfig& config) : a_(a), config_(config), color_{webgl::Image(a), webgl::Image(a)}, depth_(a), accum_(a) {
  valid_ = allocate(config.width ? config.width : 1, config.height ? config.height : 1);
}

bool GlSurface::allocate(uint32_t width, uint32_t height) {
  const webgl::FormatInfo* rgba = webgl::find_format(0x8058);
  const webgl::FormatInfo* ds = webgl::find_format(0x88F0);
  if (!color_[0].allocate(rgba, width, height, 1)) return false;
  if (config_.double_buffered && !color_[1].allocate(rgba, width, height, 1)) return false;
  if ((config_.depth_bits || config_.stencil_bits) && !depth_.allocate(ds, width, height, 1)) return false;
  if (config_.depth_bits || config_.stencil_bits) {
    const uint32_t one[4] = {__builtin_bit_cast(uint32_t, 1.0f), 0, 0, 0};
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) webgl::write_texel(depth_, x, y, 0, one);
  }
  accum_.clear();
  if (config_.accum_bits) {
    const uint64_t floats = uint64_t{width} * height * 4;
    if (!accum_.reserve(floats)) return false;
    for (uint64_t i = 0; i < floats; ++i) accum_.push_back(0);
  }
  config_.width = width;
  config_.height = height;
  ++generation_;
  return true;
}

bool GlSurface::resize(uint32_t width, uint32_t height) {
  if (width == 0) width = 1;
  if (height == 0) height = 1;
  if (width == config_.width && height == config_.height) return true;
  return allocate(width, height);
}

void GlSurface::swap() {
  if (config_.double_buffered) {
    front_ = 1 - front_;
    ++generation_;
  }
}

}  // namespace opengl
