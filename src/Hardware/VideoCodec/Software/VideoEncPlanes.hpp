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

// The pictures the HEVC and AV1 picture encoders (H265PictureEncoder, Av1PictureEncoder) take and write: 4:2:0, 8 or 10 bits a sample, every sample a 16-bit value with the
// sample in its low bits (0 .. 255 or 0 .. 1023), three compact planes - Y rows of `width` samples, U and V rows of (width + 1) / 2 - and (height + 1) / 2 chroma rows.
#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

struct EncPlanes16 {
  ArrayView<const uint16_t> y, u, v;
  uint32_t width = 0, height = 0;
  uint32_t chroma_width() const { return (width + 1) / 2; }
  uint32_t chroma_height() const { return (height + 1) / 2; }
  // The planes hold a whole picture of this size.
  bool fits() const {
    const uint64_t luma = uint64_t{width} * height, chroma = uint64_t{chroma_width()} * chroma_height();
    return width && height && y.size() >= luma && u.size() >= chroma && v.size() >= chroma;
  }
};
struct EncWritablePlanes16 {
  ArrayView<uint16_t> y, u, v;
  uint32_t width = 0, height = 0;
  uint32_t chroma_width() const { return (width + 1) / 2; }
  uint32_t chroma_height() const { return (height + 1) / 2; }
  bool fits() const {
    const uint64_t luma = uint64_t{width} * height, chroma = uint64_t{chroma_width()} * chroma_height();
    return width && height && y.size() >= luma && u.size() >= chroma && v.size() >= chroma;
  }
  EncPlanes16 view() const { return {y, u, v, width, height}; }
};

// What the encoders tell of a picture (D3D12_VIDEO_ENCODER_OUTPUT_METADATA_STATISTICS): counted over the coding units (HEVC: the smallest coding blocks, 8x8; AV1: 8x8
// luma blocks), vectors in quarter samples.
struct EncStatistics {
  uint64_t qp_sum = 0, units = 0;   // the QP (HEVC QpY, AV1 qindex) summed over the units
  uint64_t intra = 0, inter = 0, skipped = 0;
  int64_t vector_x_sum = 0, vector_y_sum = 0;   // of the inter units' first vectors
};

}  // namespace hardware
