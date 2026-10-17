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

#include "Hardware/Printer/Halftone.hpp"

namespace hardware {

namespace {

const uint8_t kBayer8[64] = {0,  32, 8,  40, 2,  34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4,  36, 14, 46, 6,  38, 60, 28, 52, 20, 62, 30, 54, 22,
                             3,  35, 11, 43, 1,  33, 9,  41, 51, 19, 59, 27, 49, 17, 57, 25, 15, 47, 7,  39, 13, 45, 5,  37, 63, 31, 55, 23, 61, 29, 53, 21};

// Grey level (255 = white) of a pixel, composited over white.
int gray_of(uint32_t p) {
  const int a = p >> 24, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
  const int y = (77 * r + 150 * g + 29 * b) >> 8;
  return (y * a + 255 * (255 - a) + 127) / 255;
}

}  // namespace

uint32_t packbits(const uint8_t* d, uint32_t n, uint8_t* out) {
  uint32_t o = 0, i = 0;
  while (i < n) {
    uint32_t run = 1;
    while (i + run < n && run < 128 && d[i + run] == d[i]) ++run;
    if (run >= 3) {
      out[o++] = static_cast<uint8_t>(257 - run);
      out[o++] = d[i];
      i += run;
      continue;
    }
    // A literal stretch up to the next run of three or more (or 128 bytes).
    uint32_t start = i, len = 0;
    while (i < n && len < 128) {
      if (i + 2 < n && d[i] == d[i + 1] && d[i] == d[i + 2]) break;
      ++i;
      ++len;
    }
    out[o++] = static_cast<uint8_t>(len - 1);
    for (uint32_t k = 0; k < len; ++k) out[o++] = d[start + k];
  }
  return o;
}

bool render_surface(const graphics::Surface& src, PageBitmap* page, Fit fit, Halftone method, allocators::IAllocator& allocator) {
  if (src.width == 0 || src.height == 0 || src.pixels.empty() || page->width() == 0) return false;
  const uint32_t pw = page->width(), ph = page->height();
  uint32_t dw = pw, dh = ph, dx = 0, dy = 0;
  if (fit == Fit::Contain) {
    // Largest size with the source's proportions that fits.
    if (uint64_t{src.width} * ph > uint64_t{src.height} * pw) {
      dh = static_cast<uint32_t>(uint64_t{src.height} * pw / src.width);
      if (dh == 0) dh = 1;
    } else {
      dw = static_cast<uint32_t>(uint64_t{src.width} * ph / src.height);
      if (dw == 0) dw = 1;
    }
    dx = (pw - dw) / 2;
    dy = (ph - dh) / 2;
  }
  ArrayView<int16_t> err = allocate_view<int16_t>(allocator, uint64_t{2} * (pw + 2));
  if (err.empty()) return false;
  err.fill(0);
  int16_t* cur = err.data() + 1;
  int16_t* next = err.data() + pw + 3;
  // Fixed-point 16.16 source coordinates per destination pixel.
  const uint64_t xstep = (uint64_t{src.width} << 16) / dw, ystep = (uint64_t{src.height} << 16) / dh;
  for (uint32_t y = 0; y < ph; ++y) {
    for (uint32_t x = 0; x < pw; ++x) {
      int g = 255;
      if (x >= dx && x < dx + dw && y >= dy && y < dy + dh) {
        const uint64_t fx = (x - dx) * xstep, fy = (y - dy) * ystep;
        if (xstep > 0x10000 || ystep > 0x10000) {
          // Reducing: average the source pixels this dot covers.
          uint32_t x0 = static_cast<uint32_t>(fx >> 16), x1 = static_cast<uint32_t>((fx + xstep + 0xFFFF) >> 16);
          uint32_t y0 = static_cast<uint32_t>(fy >> 16), y1 = static_cast<uint32_t>((fy + ystep + 0xFFFF) >> 16);
          if (x1 > src.width) x1 = src.width;
          if (y1 > src.height) y1 = src.height;
          if (x1 <= x0) x1 = x0 + 1;
          if (y1 <= y0) y1 = y0 + 1;
          uint64_t sum = 0;
          for (uint32_t sy = y0; sy < y1; ++sy) {
            const uint32_t* row = src.row(sy);
            for (uint32_t sx = x0; sx < x1; ++sx) sum += static_cast<uint64_t>(gray_of(row[sx]));
          }
          g = static_cast<int>(sum / (uint64_t{x1 - x0} * (y1 - y0)));
        } else {
          // Enlarging: bilinear between the four nearest source pixels (pixel centres at +0.5).
          int64_t cx = static_cast<int64_t>(fx) - 0x8000 + static_cast<int64_t>(xstep >> 1), cy = static_cast<int64_t>(fy) - 0x8000 + static_cast<int64_t>(ystep >> 1);
          if (cx < 0) cx = 0;
          if (cy < 0) cy = 0;
          uint32_t x0 = static_cast<uint32_t>(cx >> 16), y0 = static_cast<uint32_t>(cy >> 16);
          const uint32_t wx = static_cast<uint32_t>(cx & 0xFFFF) >> 8, wy = static_cast<uint32_t>(cy & 0xFFFF) >> 8;
          uint32_t x1 = x0 + 1 < src.width ? x0 + 1 : x0, y1 = y0 + 1 < src.height ? y0 + 1 : y0;
          if (x0 >= src.width) x0 = x1 = src.width - 1;
          if (y0 >= src.height) y0 = y1 = src.height - 1;
          const uint32_t top = gray_of(src.row(y0)[x0]) * (256 - wx) + gray_of(src.row(y0)[x1]) * wx;
          const uint32_t bottom = gray_of(src.row(y1)[x0]) * (256 - wx) + gray_of(src.row(y1)[x1]) * wx;
          g = static_cast<int>((top * (256 - wy) + bottom * wy) >> 16);
        }
      }
      bool black;
      if (method == Halftone::Threshold) {
        black = g < 128;
      } else if (method == Halftone::Ordered) {
        black = (((255 - g) * 64 + 255) >> 8) > kBayer8[(y & 7) * 8 + (x & 7)];
      } else {
        const int v = g + cur[x];
        black = v < 128;
        const int e = v - (black ? 0 : 255);  // what the dot got wrong
        cur[x + 1] = static_cast<int16_t>(cur[x + 1] + e * 7 / 16);
        next[static_cast<int64_t>(x) - 1] = static_cast<int16_t>(next[static_cast<int64_t>(x) - 1] + e * 3 / 16);
        next[x] = static_cast<int16_t>(next[x] + e * 5 / 16);
        next[x + 1] = static_cast<int16_t>(next[x + 1] + e / 16);
      }
      page->set(x, y, black);
    }
    if (method == Halftone::ErrorDiffusion) {  // the next row's errors become this row's; start the next with none
      int16_t* t = cur;
      cur = next;
      next = t;
      for (int64_t i = -1; i <= static_cast<int64_t>(pw); ++i) next[i] = 0;
    }
  }
  release_view(allocator, err);
  return true;
}

}  // namespace hardware
