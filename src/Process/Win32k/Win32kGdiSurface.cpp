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
#include "Process/Win32k/Win32kGdiInternal.hpp"

// Pixel access for every format a GDI bitmap can have, and the raster operations (ROP2 for lines, ROP3 for blits).
namespace process::win32k::gdi {

namespace {

uint32_t shift_of(uint32_t mask) {
  uint32_t s = 0;
  while (s < 32 && ((mask >> s) & 1) == 0) ++s;
  return s;
}
uint32_t extract8(uint32_t v, uint32_t mask) {
  if (mask == 0) return 0;
  const uint32_t sh = shift_of(mask), maxv = mask >> sh;
  return maxv ? uint32_t((uint64_t{(v & mask) >> sh} * 255 + maxv / 2) / maxv) : 0;
}
uint32_t insert8(uint32_t c, uint32_t mask) {
  if (mask == 0) return 0;
  const uint32_t sh = shift_of(mask), maxv = mask >> sh;
  return uint32_t((uint64_t{c} * maxv + 127) / 255) << sh;
}

}  // namespace

bool Surface::valid() const {
  if (width <= 0 || height <= 0 || stride == 0) return false;
  return uint64_t{stride} * uint32_t(height) <= bytes.size() && uint64_t{stride} >= (uint64_t(width) * bpp + 7) / 8;
}

Surface surface_of(Bitmap& b) {
  Surface s;
  s.bytes = b.data.writable_view();
  s.width = b.width;
  s.height = b.height;
  s.bpp = b.bpp;
  s.stride = b.stride;
  s.bottom_up = b.bottom_up;
  s.colors = ArrayView<const uint32_t>(b.colors.view());
  for (int i = 0; i < 3; ++i) s.masks[i] = b.masks[i];
  return s;
}

uint32_t Surface::get_raw32(int32_t x, int32_t y) const {
  if (bpp != 32 || x < 0 || y < 0 || x >= width || y >= height) return 0;
  const uint64_t row = bottom_up ? uint32_t(height - 1 - y) : uint32_t(y);
  const uint64_t o = row * stride + uint64_t(x) * 4;
  if (o + 4 > bytes.size()) return 0;
  return uint32_t(bytes[o]) | uint32_t(bytes[o + 1]) << 8 | uint32_t(bytes[o + 2]) << 16 | uint32_t(bytes[o + 3]) << 24;
}

void Surface::put_raw32(int32_t x, int32_t y, uint32_t v) {
  if (bpp != 32 || x < 0 || y < 0 || x >= width || y >= height) return;
  const uint64_t row = bottom_up ? uint32_t(height - 1 - y) : uint32_t(y);
  const uint64_t o = row * stride + uint64_t(x) * 4;
  if (o + 4 > bytes.size()) return;
  for (uint32_t i = 0; i < 4; ++i) bytes[o + i] = uint8_t(v >> (8 * i));
}

ColorRef Surface::get(int32_t x, int32_t y) const {
  if (x < 0 || y < 0 || x >= width || y >= height) return 0;
  const uint64_t row = bottom_up ? uint32_t(height - 1 - y) : uint32_t(y);
  const uint64_t base = row * stride;
  switch (bpp) {
    case 32: {
      const uint64_t o = base + uint64_t(x) * 4;
      if (o + 4 > bytes.size()) return 0;
      if (masks[0] == 0 && masks[1] == 0 && masks[2] == 0) return make_rgb(bytes[o + 2], bytes[o + 1], bytes[o]);
      const uint32_t v = get_raw32(x, y);
      return make_rgb(extract8(v, masks[0]), extract8(v, masks[1]), extract8(v, masks[2]));
    }
    case 24: {
      const uint64_t o = base + uint64_t(x) * 3;
      if (o + 3 > bytes.size()) return 0;
      return make_rgb(bytes[o + 2], bytes[o + 1], bytes[o]);
    }
    case 16: {
      const uint64_t o = base + uint64_t(x) * 2;
      if (o + 2 > bytes.size()) return 0;
      const uint32_t v = uint32_t(bytes[o]) | uint32_t(bytes[o + 1]) << 8;
      if (masks[0] == 0) return make_rgb(extract8(v, 0x7C00), extract8(v, 0x03E0), extract8(v, 0x001F));
      return make_rgb(extract8(v, masks[0]), extract8(v, masks[1]), extract8(v, masks[2]));
    }
    case 8: case 4: case 1: {
      const uint64_t bit = uint64_t(x) * bpp, o = base + bit / 8;
      if (o >= bytes.size()) return 0;
      const uint32_t shift = 8 - bpp - uint32_t(bit % 8);
      const uint32_t idx = (uint32_t(bytes[o]) >> shift) & ((1u << bpp) - 1);
      return idx < colors.size() ? colors[idx] & 0xFFFFFF : 0;
    }
    default: return 0;
  }
}

uint32_t Surface::nearest_index(ColorRef c) const {
  uint32_t best = 0;
  uint64_t best_d = UINT64_MAX;
  for (uint64_t i = 0; i < colors.size(); ++i) {
    const ColorRef e = colors[i];
    const int64_t dr = int64_t(cr_r(e)) - cr_r(c), dg = int64_t(cr_g(e)) - cr_g(c), db = int64_t(cr_b(e)) - cr_b(c);
    const uint64_t d = uint64_t(dr * dr + dg * dg + db * db);
    if (d < best_d) { best_d = d; best = uint32_t(i); if (d == 0) break; }
  }
  return best;
}

void Surface::put(int32_t x, int32_t y, ColorRef c) {
  if (x < 0 || y < 0 || x >= width || y >= height) return;
  const uint64_t row = bottom_up ? uint32_t(height - 1 - y) : uint32_t(y);
  const uint64_t base = row * stride;
  switch (bpp) {
    case 32: {
      const uint64_t o = base + uint64_t(x) * 4;
      if (o + 4 > bytes.size()) return;
      if (masks[0] == 0 && masks[1] == 0 && masks[2] == 0) {
        bytes[o + 2] = uint8_t(cr_r(c)); bytes[o + 1] = uint8_t(cr_g(c)); bytes[o] = uint8_t(cr_b(c));
      } else {
        put_raw32(x, y, insert8(cr_r(c), masks[0]) | insert8(cr_g(c), masks[1]) | insert8(cr_b(c), masks[2]));
      }
      return;
    }
    case 24: {
      const uint64_t o = base + uint64_t(x) * 3;
      if (o + 3 > bytes.size()) return;
      bytes[o + 2] = uint8_t(cr_r(c)); bytes[o + 1] = uint8_t(cr_g(c)); bytes[o] = uint8_t(cr_b(c));
      return;
    }
    case 16: {
      const uint64_t o = base + uint64_t(x) * 2;
      if (o + 2 > bytes.size()) return;
      const uint32_t v = masks[0] == 0 ? (insert8(cr_r(c), 0x7C00) | insert8(cr_g(c), 0x03E0) | insert8(cr_b(c), 0x001F))
                                       : (insert8(cr_r(c), masks[0]) | insert8(cr_g(c), masks[1]) | insert8(cr_b(c), masks[2]));
      bytes[o] = uint8_t(v); bytes[o + 1] = uint8_t(v >> 8);
      return;
    }
    case 8: case 4: case 1: {
      const uint64_t bit = uint64_t(x) * bpp, o = base + bit / 8;
      if (o >= bytes.size()) return;
      const uint32_t shift = 8 - bpp - uint32_t(bit % 8), mask = ((1u << bpp) - 1) << shift;
      bytes[o] = uint8_t((bytes[o] & ~mask) | ((nearest_index(c & 0xFFFFFF) << shift) & mask));
      return;
    }
    default: return;
  }
}

ColorRef rop2_apply(int32_t rop2, ColorRef d, ColorRef s) {
  d &= 0xFFFFFF;
  s &= 0xFFFFFF;
  uint32_t r;
  switch (rop2) {
    case 1: r = 0; break;
    case 2: r = ~(d | s); break;
    case 3: r = d & ~s; break;
    case 4: r = ~s; break;
    case 5: r = s & ~d; break;
    case 6: r = ~d; break;
    case 7: r = d ^ s; break;
    case 8: r = ~(d & s); break;
    case 9: r = d & s; break;
    case 10: r = ~(d ^ s); break;
    case 11: r = d; break;
    case 12: r = d | ~s; break;
    case 14: r = s | ~d; break;
    case 15: r = d | s; break;
    case 16: r = 0xFFFFFF; break;
    default: r = s; break;
  }
  return r & 0xFFFFFF;
}

uint32_t rop3_apply(uint32_t rop, ColorRef pat, ColorRef src, ColorRef dst) {
  const uint32_t table = (rop >> 16) & 0xFF;
  pat &= 0xFFFFFF; src &= 0xFFFFFF; dst &= 0xFFFFFF;
  uint32_t r = 0;
  for (uint32_t idx = 0; idx < 8; ++idx) {
    if (((table >> idx) & 1) == 0) continue;
    const uint32_t p = (idx & 4) ? pat : ~pat, s = (idx & 2) ? src : ~src, d = (idx & 1) ? dst : ~dst;
    r |= p & s & d;
  }
  return r & 0xFFFFFF;
}

}  // namespace process::win32k::gdi
