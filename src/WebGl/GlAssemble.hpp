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

// How the vertices of a GL draw make primitives, for the CPU's stages (GlDraw.cpp) and the 3D hardware (GlAccel.cpp) alike.
#pragma once
#include <stdint.h>

namespace webgl {

// Calls `primitive(positions, n)` for every primitive of a draw in `mode`, with the positions in the draw's vertex list of its vertices (in the order the next stage takes them:
// adjacency primitives as the geometry stage's input arrays have them). A restart index (`vertex(i)` below 0) begins a new strip or list.
template <class Vertex, class Primitive>
inline void assemble_primitives(uint32_t mode, int32_t count, uint32_t patch_vertices, Vertex& vertex, Primitive& primitive) {
  int32_t pos[32];
  int32_t start = 0;
  for (int32_t i = 0; i <= count; ++i) {
    if (i < count && vertex(i) >= 0) continue;
    const int32_t n = i - start;      // a run between restarts: [start, i)
    const auto emit = [&](uint32_t k) { primitive(pos, k); };
    switch (mode) {
      case 0: for (int32_t k = 0; k < n; ++k) { pos[0] = start + k; emit(1); } break;
      case 1: for (int32_t k = 0; k + 1 < n; k += 2) { pos[0] = start + k; pos[1] = start + k + 1; emit(2); } break;
      case 2: case 3:
        for (int32_t k = 0; k + 1 < n; ++k) { pos[0] = start + k; pos[1] = start + k + 1; emit(2); }
        if (mode == 2 && n > 2) { pos[0] = start + n - 1; pos[1] = start; emit(2); }
        break;
      case 4: for (int32_t k = 0; k + 2 < n; k += 3) { pos[0] = start + k; pos[1] = start + k + 1; pos[2] = start + k + 2; emit(3); } break;
      case 5:
        for (int32_t k = 0; k + 2 < n; ++k) {
          pos[0] = start + k + (k & 1);
          pos[1] = start + k + 1 - (k & 1);
          pos[2] = start + k + 2;
          emit(3);
        }
        break;
      case 6: for (int32_t k = 1; k + 1 < n; ++k) { pos[0] = start; pos[1] = start + k; pos[2] = start + k + 1; emit(3); } break;
      case 0xA: for (int32_t k = 0; k + 3 < n; k += 4) { for (int32_t j = 0; j < 4; ++j) pos[j] = start + k + j; emit(4); } break;
      case 0xB: for (int32_t k = 0; k + 3 < n; ++k) { for (int32_t j = 0; j < 4; ++j) pos[j] = start + k + j; emit(4); } break;
      case 0xC: for (int32_t k = 0; k + 5 < n; k += 6) { for (int32_t j = 0; j < 6; ++j) pos[j] = start + k + j; emit(6); } break;
      case 0xD: {
        // A triangle strip with adjacency (GL 4.6, table 10.1, 1-based there): which vertices make triangle t of the strip, as (first, its adjacent, second, ..., third, ...).
        const int32_t tris = n >= 6 ? (n - 4) / 2 : 0;
        for (int32_t t = 0; t < tris; ++t) {
          int32_t v[6];      // 1-based as in the table: 1st, adj 1-2, 2nd, adj 2-3, 3rd, adj 3-1
          const int32_t i2 = 2 * t;
          if (tris == 1) { const int32_t a[6] = {1, 2, 3, 6, 5, 4}; for (int j = 0; j < 6; ++j) v[j] = a[j]; }
          else if (t == 0) { const int32_t a[6] = {1, 2, 3, 7, 5, 4}; for (int j = 0; j < 6; ++j) v[j] = a[j]; }
          else if (t == tris - 1) {
            if (t & 1) { const int32_t a[6] = {i2 + 3, i2 - 1, i2 + 1, i2 + 4, i2 + 5, i2 + 6}; for (int j = 0; j < 6; ++j) v[j] = a[j]; }
            else { const int32_t a[6] = {i2 + 1, i2 - 1, i2 + 3, i2 + 6, i2 + 5, i2 + 4}; for (int j = 0; j < 6; ++j) v[j] = a[j]; }
          } else if (t & 1) {
            const int32_t a[6] = {i2 + 3, i2 - 1, i2 + 1, i2 + 4, i2 + 5, i2 + 7};
            for (int j = 0; j < 6; ++j) v[j] = a[j];
          } else {
            const int32_t a[6] = {i2 + 1, i2 - 1, i2 + 3, i2 + 7, i2 + 5, i2 + 4};
            for (int j = 0; j < 6; ++j) v[j] = a[j];
          }
          bool inside = true;
          for (int j = 0; j < 6; ++j) { pos[j] = start + v[j] - 1; inside = inside && v[j] >= 1 && v[j] <= n; }
          if (inside) emit(6);
        }
        break;
      }
      case 0xE:
        if (patch_vertices >= 1 && patch_vertices <= 32)
          for (int32_t k = 0; k + static_cast<int32_t>(patch_vertices) <= n; k += static_cast<int32_t>(patch_vertices)) {
            for (uint32_t j = 0; j < patch_vertices; ++j) pos[j] = start + k + static_cast<int32_t>(j);
            emit(patch_vertices);
          }
        break;
      default: break;
    }
    start = i + 1;
  }
}

}  // namespace webgl
