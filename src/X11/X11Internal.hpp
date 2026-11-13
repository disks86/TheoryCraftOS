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

// What the parts of the X server (X11Requests.cpp, X11Windows.cpp, X11Drawing.cpp, X11Input.cpp) share and nobody else needs.
#pragma once
#include "X11/X11Server.hpp"

namespace x11 {

struct Server::Client {
  explicit Client(allocators::IAllocator& allocator) : in(allocator), out(allocator) {}
  uint8_t index = 0;
  bool setup_done = false, big = false, closing = false, big_requests = false;
  uint16_t xi_version = 0;      // the XInput version it agreed to (major << 8 | minor), 0: none yet
  uint16_t sequence = 0;
  uint64_t pid = 0;
  ByteBuffer in;       // bytes of a request not complete yet
  ByteBuffer out;      // what the client is owed
};

// Where a drawing request draws: a picture (a pixmap's, or the top-level picture a window's inside is part of) and the part of it the drawable may touch.
struct Server::Canvas {
  explicit Canvas(allocators::IAllocator& allocator) : clip(allocator) {}
  Picture* picture = nullptr;
  int32_t ox = 0, oy = 0;          // the drawable's (0, 0) in the picture
  uint8_t depth = 24;
  Region clip;                     // the drawable's coordinates; inside the picture
  Window* top = nullptr;           // the top-level whose picture it is (null: a pixmap)
  Picture* mask = nullptr;         // the GC's clip mask, at (mask_x, mask_y)
  int32_t mask_x = 0, mask_y = 0;
  Rect dirty;                      // what was drawn (drawable coordinates)
  bool drew = false;
};

// The colour and raster operation a drawing uses (from the GC, or fixed: ImageText and backgrounds draw with plain copies).
struct Ink {
  uint8_t function = 3, fill_style = 0;
  uint32_t foreground = 0, background = 0, plane_mask = 0xFFFFFFFFu;
};

inline uint64_t pad4(uint64_t n) { return (n + 3) & ~uint64_t{3}; }
inline int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }
inline int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }
inline Rect intersect(const Rect& a, const Rect& b) {
  const int32_t x0 = max32(a.x, b.x), y0 = max32(a.y, b.y);
  const int64_t x1 = a.x + int64_t{a.w} < b.x + int64_t{b.w} ? a.x + int64_t{a.w} : b.x + int64_t{b.w};
  const int64_t y1 = a.y + int64_t{a.h} < b.y + int64_t{b.h} ? a.y + int64_t{a.h} : b.y + int64_t{b.h};
  if (x1 <= x0 || y1 <= y0) return {x0, y0, 0, 0};
  return {x0, y0, static_cast<int32_t>(x1 - x0), static_cast<int32_t>(y1 - y0)};
}
inline Rect unite(const Rect& a, const Rect& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  const int32_t x0 = min32(a.x, b.x), y0 = min32(a.y, b.y);
  const int32_t x1 = max32(a.x + a.w, b.x + b.w), y1 = max32(a.y + a.h, b.y + b.h);
  return {x0, y0, x1 - x0, y1 - y0};
}
inline bool contains(const Rect& r, int32_t x, int32_t y) { return x >= r.x && y >= r.y && x - r.x < r.w && y - r.y < r.h; }

// A pixel value as a picture of this depth keeps it (depth 24: opaque; the others: the bits the depth has).
inline uint32_t stored_pixel(uint8_t depth, uint32_t v) {
  switch (depth) {
    case 1: return v & 1u;
    case 8: return v & 0xFFu;
    case 24: return 0xFF000000u | (v & 0xFFFFFFu);
    default: return v;
  }
}
inline uint32_t pixel_value(uint8_t depth, uint32_t stored) { return depth == 24 ? stored & 0xFFFFFFu : stored; }
inline uint32_t depth_mask(uint8_t depth) { return depth >= 32 ? 0xFFFFFFFFu : (1u << depth) - 1; }

}  // namespace x11
