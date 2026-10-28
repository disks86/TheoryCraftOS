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

// A unit cube (corners at +-1) with the logo - or any picture - on every face, for the 3D test: the point is that the texture sampler is
// exercised, which flat per-face colors cannot show. Each face is mapped so that the picture is upright and not mirrored when the face is
// looked at from outside with the camera upright (the device's clip space has +y down the screen, see the quad in GraphicsTest).
#pragma once
#include <stdint.h>
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Utility/ArrayView.hpp"

namespace graphics {

constexpr uint32_t kCubeVertices = 24, kCubeIndices = 36;

// Fills 24 vertices (four a face) and 36 indices (two triangles a face). The views must be that big (they are checked).
inline void build_textured_cube(ArrayView<TexturedVertex> vertices, ArrayView<uint16_t> indices) {
  // For each face: where it points (the normal), the screen-right and screen-up directions of the picture on it.
  struct Face {
    float normal[3], right[3], up[3];
  };
  static const Face kFaces[6] = {
      {{0, 0, 1}, {1, 0, 0}, {0, -1, 0}},   {{0, 0, -1}, {-1, 0, 0}, {0, -1, 0}}, {{1, 0, 0}, {0, 0, -1}, {0, -1, 0}},
      {{-1, 0, 0}, {0, 0, 1}, {0, -1, 0}},  {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},    {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}},
  };
  static const float kCorners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};   // (right, up) of each corner
  static const uint16_t kOrder[6] = {0, 1, 2, 0, 2, 3};
  for (uint32_t f = 0; f < 6; ++f) {
    const Face& face = kFaces[f];
    for (uint32_t c = 0; c < 4; ++c) {
      TexturedVertex& out = vertices[f * 4 + c];
      for (uint32_t k = 0; k < 3; ++k) out.position[k] = face.normal[k] + kCorners[c][0] * face.right[k] + kCorners[c][1] * face.up[k];
      out.uv[0] = (kCorners[c][0] + 1) * 0.5f;
      out.uv[1] = (1 - kCorners[c][1]) * 0.5f;   // v runs down the picture
    }
    for (uint32_t k = 0; k < 6; ++k) indices[f * 6 + k] = static_cast<uint16_t>(f * 4 + kOrder[k]);
  }
}

}  // namespace graphics
