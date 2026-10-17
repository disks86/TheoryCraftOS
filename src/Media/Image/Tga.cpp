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
}

bool decode_tga(ConstByteView data, Image* out) {
  out->clear();
  if (data.size() < 18 || detect_image_format(data) != ImageFormat::Tga) return false;
  const uint32_t id_length = data[0], map_type = data[1], type = data[2];
  const uint32_t map_first = le16(data, 3), map_length = le16(data, 5), map_entry_bits = data[7];
  const uint32_t width = le16(data, 12), height = le16(data, 14), bits = data[16], descriptor = data[17];
  const bool rle = type >= 9, mapped = type == 1 || type == 9, grey = type == 3 || type == 11;
  const bool top = descriptor & 0x20, right = descriptor & 0x10;
  const uint32_t alpha_bits = descriptor & 0x0F;
  if (width == 0 || height == 0) return false;
  uint64_t at = 18 + id_length;
  // The colour map, if any.
  uint32_t palette[256] = {};
  if (map_type == 1) {
    const uint32_t entry_bytes = (map_entry_bits + 7) / 8;
    if (map_entry_bits != 15 && map_entry_bits != 16 && map_entry_bits != 24 && map_entry_bits != 32) return false;
    if (at + uint64_t{map_length} * entry_bytes > data.size()) return false;
    for (uint32_t i = 0; i < map_length; ++i) {
      const uint8_t* p = data.data() + at + uint64_t{i} * entry_bytes;
      uint32_t c;
      if (entry_bytes == 2) {
        const uint32_t v = p[0] | (p[1] << 8);
        c = 0xFF000000u | ((((v >> 10) & 31) * 255 / 31) << 16) | ((((v >> 5) & 31) * 255 / 31) << 8) | ((v & 31) * 255 / 31);
      } else {
        c = 0xFF000000u | (uint32_t{p[2]} << 16) | (uint32_t{p[1]} << 8) | p[0];
        if (entry_bytes == 4) c = (c & 0x00FFFFFF) | (uint32_t{p[3]} << 24);
      }
      if (map_first + i < 256) palette[map_first + i] = c;
    }
    at += uint64_t{map_length} * entry_bytes;
  } else if (mapped) {
    return false;
  }
  if ((mapped && bits != 8) || (grey && bits != 8 && bits != 16) || (!mapped && !grey && bits != 15 && bits != 16 && bits != 24 && bits != 32)) return false;
  if (type != 1 && type != 2 && type != 3 && type != 9 && type != 10 && type != 11) return false;
  const uint32_t pixel_bytes = (bits + 7) / 8;
  if (!out->create(width, height, 1)) return false;
  ArrayView<uint32_t> pixels = out->frame(0);
  bool has_alpha = false;
  auto convert = [&](const uint8_t* p) -> uint32_t {
    if (mapped) return palette[p[0]];
    if (grey) return 0xFF000000u | (uint32_t{p[0]} << 16) | (uint32_t{p[0]} << 8) | p[0];
    switch (bits) {
      case 15:
      case 16: {
        const uint32_t v = p[0] | (p[1] << 8);
        const uint32_t a = bits == 16 && alpha_bits ? ((v & 0x8000) ? 255 : 0) : 255;
        return (a << 24) | ((((v >> 10) & 31) * 255 / 31) << 16) | ((((v >> 5) & 31) * 255 / 31) << 8) | ((v & 31) * 255 / 31);
      }
      case 24: return 0xFF000000u | (uint32_t{p[2]} << 16) | (uint32_t{p[1]} << 8) | p[0];
      default: return ((alpha_bits ? uint32_t{p[3]} : 255u) << 24) | (uint32_t{p[2]} << 16) | (uint32_t{p[1]} << 8) | p[0];
    }
  };
  const uint64_t total = uint64_t{width} * height;
  uint64_t done = 0;
  while (done < total) {
    uint32_t count = 1;
    bool repeat = false;
    if (rle) {
      if (at >= data.size()) goto fail;
      const uint32_t header = data[at++];
      count = (header & 0x7F) + 1;
      repeat = header & 0x80;
    } else {
      count = static_cast<uint32_t>(total - done < 4096 ? total - done : 4096);
    }
    if (repeat) {
      if (at + pixel_bytes > data.size()) goto fail;
      const uint32_t c = convert(data.data() + at);
      at += pixel_bytes;
      for (uint32_t i = 0; i < count && done < total; ++i, ++done) pixels[done] = c;
    } else {
      if (at + uint64_t{count} * pixel_bytes > data.size()) goto fail;
      for (uint32_t i = 0; i < count && done < total; ++i, ++done) pixels[done] = convert(data.data() + at + uint64_t{i} * pixel_bytes);
      at += uint64_t{count} * pixel_bytes;
    }
  }
  // The file is stored bottom-up and left to right unless the descriptor says otherwise: put it top-down, left-right.
  if (!top)
    for (uint32_t y = 0; y < height / 2; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        const uint32_t t = pixels[uint64_t{y} * width + x];
        pixels[uint64_t{y} * width + x] = pixels[uint64_t{height - 1 - y} * width + x];
        pixels[uint64_t{height - 1 - y} * width + x] = t;
      }
  if (right)
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width / 2; ++x) {
        const uint32_t t = pixels[uint64_t{y} * width + x];
        pixels[uint64_t{y} * width + x] = pixels[uint64_t{y} * width + width - 1 - x];
        pixels[uint64_t{y} * width + width - 1 - x] = t;
      }
  for (uint64_t i = 0; i < total && !has_alpha; ++i) has_alpha = (pixels[i] >> 24) != 255;
  out->set_has_alpha(has_alpha);
  return true;
fail:
  out->clear();
  return false;
}

bool encode_tga(const Image& image, ByteBuffer* out) {
  if (image.empty() || image.width() > 65535 || image.height() > 65535) return false;
  bool ok = out->put8(0) && out->put8(0) && out->put8(2) && out->put_zeros(5) && out->put16(0) && out->put16(0) && out->put16(image.width()) &&
            out->put16(image.height()) && out->put8(32) && out->put8(0x28);  // 32 bits, 8 alpha bits, top-left origin
  const ArrayView<uint32_t> pixels = image.frame(0);
  for (uint64_t i = 0; i < pixels.size() && ok; ++i) {
    const uint32_t p = pixels[i];
    ok = out->put8(static_cast<uint8_t>(p)) && out->put8(static_cast<uint8_t>(p >> 8)) && out->put8(static_cast<uint8_t>(p >> 16)) && out->put8(static_cast<uint8_t>(p >> 24));
  }
  return ok;
}

}  // namespace media
