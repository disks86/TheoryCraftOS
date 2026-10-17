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

#include "Media/ImageCodecs.hpp"

namespace media {

namespace {
uint32_t le16(ConstByteView v, uint64_t at) { return v[at] | (v[at + 1] << 8); }
uint32_t le32(ConstByteView v, uint64_t at) { return le16(v, at) | (le16(v, at + 2) << 16); }

// A colour channel given by a bit mask, scaled to 0..255.
struct Channel {
  uint32_t mask = 0;
  uint32_t shift = 0, max = 0;
  void set(uint32_t m) {
    mask = m;
    if (!m) return;
    shift = 0;
    while (!((m >> shift) & 1)) ++shift;
    max = m >> shift;  // a run of ones: the largest value
  }
  uint32_t get(uint32_t pixel) const { return mask ? (((pixel & mask) >> shift) * 255 + max / 2) / max : 0; }
};
}  // namespace

bool decode_bmp(ConstByteView data, Image* out) {
  out->clear();
  if (data.size() < 26 || detect_image_format(data) != ImageFormat::Bmp) return false;
  const uint32_t pixel_offset = le32(data, 10);
  const uint32_t dib = le32(data, 14);
  int32_t width, height;
  uint32_t planes, bpp, compression = 0, colors_used = 0;
  uint32_t header_end = 14 + dib;
  if (dib == 12) {
    width = static_cast<int16_t>(le16(data, 18));
    height = static_cast<int16_t>(le16(data, 20));
    planes = le16(data, 22);
    bpp = le16(data, 24);
  } else if (dib >= 40 && 14 + uint64_t{dib} <= data.size()) {
    width = static_cast<int32_t>(le32(data, 18));
    height = static_cast<int32_t>(le32(data, 22));
    planes = le16(data, 26);
    bpp = le16(data, 28);
    compression = le32(data, 30);
    colors_used = le32(data, 46);
  } else {
    return false;
  }
  if (planes != 1 || width <= 0 || height == 0 || height == INT32_MIN) return false;
  const bool top_down = height < 0;
  const uint32_t rows = static_cast<uint32_t>(top_down ? -height : height);
  const uint32_t w = static_cast<uint32_t>(width);
  if (compression != 0 && compression != 1 && compression != 2 && compression != 3 && compression != 6) return false;
  if ((compression == 1 && bpp != 8) || (compression == 2 && bpp != 4)) return false;
  if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return false;
  if ((compression == 3 || compression == 6) && bpp != 16 && bpp != 32) return false;

  // Masks for 16 and 32 bits.
  Channel red, green, blue, alpha;
  if (bpp == 16 || bpp == 32) {
    if (compression == 3 || compression == 6) {
      uint64_t at = 54;  // after a 40-byte header the masks follow it; in larger headers they are inside
      if (dib == 40) header_end += (compression == 6 ? 16 : 12);
      if (at + 12 > data.size()) return false;
      red.set(le32(data, at));
      green.set(le32(data, at + 4));
      blue.set(le32(data, at + 8));
      if ((dib == 40 && compression == 6 && at + 16 <= data.size()) || dib >= 56) alpha.set(le32(data, at + 12));
    } else if (bpp == 16) {
      red.set(0x7C00);
      green.set(0x03E0);
      blue.set(0x001F);
    } else {
      red.set(0x00FF0000);
      green.set(0x0000FF00);
      blue.set(0x000000FF);
    }
  }

  // The palette.
  uint32_t palette[256] = {};
  if (bpp <= 8) {
    const uint32_t entry = dib == 12 ? 3 : 4;
    uint32_t count = colors_used ? colors_used : 1u << bpp;
    if (count > 256) count = 256;
    if (header_end + uint64_t{count} * entry > data.size()) return false;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* p = data.data() + header_end + uint64_t{i} * entry;
      palette[i] = 0xFF000000u | (uint32_t{p[2]} << 16) | (uint32_t{p[1]} << 8) | p[0];
    }
  }
  if (pixel_offset > data.size()) return false;
  if (!out->create(w, rows, 1)) return false;
  ArrayView<uint32_t> pixels = out->frame(0);
  auto row_of = [&](uint32_t y) { return top_down ? y : rows - 1 - y; };  // file row y to picture row

  if (compression == 1 || compression == 2) {
    uint64_t at = pixel_offset;
    uint32_t x = 0, y = 0;
    auto put = [&](uint32_t index) {
      if (x < w && y < rows) pixels[uint64_t{row_of(y)} * w + x] = palette[index & 255];
      ++x;
    };
    pixels.fill(palette[0]);
    while (at + 2 <= data.size() && y < rows) {
      const uint32_t count = data[at], value = data[at + 1];
      at += 2;
      if (count > 0) {
        for (uint32_t i = 0; i < count; ++i) put(compression == 1 ? value : (i % 2 == 0 ? value >> 4 : value & 15));
      } else if (value == 0) {
        x = 0;
        ++y;
      } else if (value == 1) {
        break;
      } else if (value == 2) {
        if (at + 2 > data.size()) break;
        x += data[at];
        y += data[at + 1];
        at += 2;
      } else {  // a run of literal pixels
        const uint32_t n = value;
        const uint64_t bytes = compression == 1 ? n : (n + 1) / 2;
        if (at + bytes > data.size()) break;
        for (uint32_t i = 0; i < n; ++i) put(compression == 1 ? data[at + i] : (i % 2 == 0 ? data[at + i / 2] >> 4 : data[at + i / 2] & 15));
        at += bytes + (bytes & 1);  // padded to a 16-bit boundary
      }
    }
    return true;
  }

  const uint64_t stride = ((uint64_t{w} * bpp + 31) / 32) * 4;
  if (pixel_offset + stride * rows > data.size()) {
    out->clear();
    return false;
  }
  bool alpha_seen = false;
  bool any_alpha_value = false;
  if (bpp == 32 && !alpha.mask && compression == 0)  // BI_RGB with 32 bits: alpha only if the file really uses it
    for (uint32_t y = 0; y < rows && !any_alpha_value; ++y)
      for (uint32_t x = 0; x < w; ++x)
        if (data[pixel_offset + y * stride + 4 * uint64_t{x} + 3]) {
          any_alpha_value = true;
          break;
        }
  for (uint32_t y = 0; y < rows; ++y) {
    const uint8_t* row = data.data() + pixel_offset + y * stride;
    uint32_t* dest = pixels.data() + uint64_t{row_of(y)} * w;
    for (uint32_t x = 0; x < w; ++x) {
      uint32_t c;
      switch (bpp) {
        case 1: c = palette[(row[x / 8] >> (7 - x % 8)) & 1]; break;
        case 4: c = palette[(row[x / 2] >> (x % 2 ? 0 : 4)) & 15]; break;
        case 8: c = palette[row[x]]; break;
        case 16: {
          const uint32_t v = row[2 * x] | (row[2 * x + 1] << 8);
          c = (alpha.mask ? alpha.get(v) << 24 : 0xFF000000u) | (red.get(v) << 16) | (green.get(v) << 8) | blue.get(v);
          break;
        }
        case 24: c = 0xFF000000u | (uint32_t{row[3 * x + 2]} << 16) | (uint32_t{row[3 * x + 1]} << 8) | row[3 * x]; break;
        default: {
          const uint32_t v = row[4 * x] | (row[4 * x + 1] << 8) | (row[4 * x + 2] << 16) | (uint32_t{row[4 * x + 3]} << 24);
          uint32_t a = 255;
          if (alpha.mask) a = alpha.get(v);
          else if (compression == 0 && any_alpha_value) a = v >> 24;
          c = (a << 24) | (red.get(v) << 16) | (green.get(v) << 8) | blue.get(v);
          break;
        }
      }
      if ((c >> 24) != 255) alpha_seen = true;
      dest[x] = c;
    }
  }
  out->set_has_alpha(alpha_seen);
  return true;
}

bool encode_bmp(const Image& image, ByteBuffer* out) {
  if (image.empty() || image.width() > 0x7FFFFFFF / 4 || image.height() > 0x7FFFFFFF) return false;
  const bool alpha = image.has_alpha();
  const uint32_t w = image.width(), h = image.height();
  const uint32_t bpp = alpha ? 32 : 24;
  const uint64_t stride = ((uint64_t{w} * bpp + 31) / 32) * 4;
  const uint32_t dib = alpha ? 108 : 40;  // a V4 header carries the masks that mark the alpha channel
  const uint64_t image_size = stride * h;
  const uint64_t file_size = 14 + dib + image_size;
  if (file_size > 0xFFFFFFF0ull) return false;
  bool ok = out->put8('B') && out->put8('M') && out->put32(static_cast<uint32_t>(file_size)) && out->put32(0) && out->put32(14 + dib);
  ok = ok && out->put32(dib) && out->put32(w) && out->put32(h) && out->put16(1) && out->put16(bpp) && out->put32(alpha ? 3 : 0) &&
       out->put32(static_cast<uint32_t>(image_size)) && out->put32(2835) && out->put32(2835) && out->put32(0) && out->put32(0);
  if (alpha) {
    ok = ok && out->put32(0x00FF0000) && out->put32(0x0000FF00) && out->put32(0x000000FF) && out->put32(0xFF000000) && out->put_tag("Win ") && out->put_zeros(36 + 12);
  }
  const ArrayView<uint32_t> pixels = image.frame(0);
  for (uint32_t row = 0; row < h && ok; ++row) {  // bottom-up
    const uint32_t y = h - 1 - row;
    for (uint32_t x = 0; x < w && ok; ++x) {
      const uint32_t p = pixels[uint64_t{y} * w + x];
      ok = out->put8(static_cast<uint8_t>(p)) && out->put8(static_cast<uint8_t>(p >> 8)) && out->put8(static_cast<uint8_t>(p >> 16)) && (!alpha || out->put8(static_cast<uint8_t>(p >> 24)));
    }
    ok = ok && out->put_zeros(stride - uint64_t{w} * bpp / 8);
  }
  return ok;
}

}  // namespace media
