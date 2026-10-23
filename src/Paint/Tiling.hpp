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

// How a background layer's tiles lie along one axis (background-position, background-repeat: repeat / space / round / no-repeat). The builder uses it for
// gradient layers, the painter for pictures.
#pragma once
#include <stdint.h>

namespace paint {

struct TileAxis {
  int32_t size = 0;       // the tile's size (changed by `round`)
  int32_t first = 0;      // where the first tile starts
  int32_t pitch = 0;      // from one tile to the next
  int32_t count = 0;      // how many tiles (0: none show)
};

constexpr int32_t kMaxTilesPerAxis = 4096;
enum TileRepeat : uint8_t { kRepeat = 0, kSpace = 1, kRound = 2, kNoRepeat = 3 };

// The positioning area is [origin, origin + origin_len); only [clip_start, clip_end) shows. `size` is the tile size; position is `percent_x100` percent of the
// room left (origin_len - size) plus `px100` pixels.
inline TileAxis lay_out_tiles(int32_t origin, int32_t origin_len, int32_t clip_start, int32_t clip_end, int32_t size, int32_t percent_x100, int32_t px100, uint8_t repeat) {
  TileAxis out;
  if (size <= 0 || origin_len <= 0 || clip_end <= clip_start) return out;
  if (repeat == kRound) {
    int32_t n = (origin_len + size / 2) / size;
    if (n < 1) n = 1;
    size = (origin_len + n / 2) / n > 0 ? (origin_len + n / 2) / n : 1;   // (the tile count is whole: the sizes stretch, the last tile may be cut a little)
  }
  out.size = size;
  out.pitch = size;
  if (repeat == kSpace) {
    const int32_t n = origin_len / size;
    if (n >= 2) {
      out.pitch = size + (origin_len - n * size) / (n - 1);
      out.first = origin;
      out.count = n < kMaxTilesPerAxis ? n : kMaxTilesPerAxis;
      return out;
    }
    repeat = kNoRepeat;   // (not even two tiles fit: one, placed by the position)
  }
  const int32_t start = origin + static_cast<int32_t>(static_cast<int64_t>(origin_len - size) * percent_x100 / 10000) + px100 / 100;
  if (repeat == kNoRepeat) {
    out.first = start;
    out.count = 1;
    return out;
  }
  // repeat (or round): the tiles that reach into the clip
  int32_t first = start;
  if (start > clip_start) first = start - ((start - clip_start + size - 1) / size) * size;
  else first = start + ((clip_start - start) / size) * size;
  out.first = first;
  int64_t count = (static_cast<int64_t>(clip_end) - first + size - 1) / size;
  if (count < 0) count = 0;
  out.count = count < kMaxTilesPerAxis ? static_cast<int32_t>(count) : kMaxTilesPerAxis;
  return out;
}

}  // namespace paint
