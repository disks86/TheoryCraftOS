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

// WebP, lossless only (the VP8L format: LZ77 + colour cache + prefix codes + the four image transforms). The lossy VP8
// format and animated files are refused. Written from the format's specification.
#include "Media/ImageCodecs.hpp"

namespace media {

namespace {
constexpr uint32_t kCodesPerGroup = 5;
constexpr uint32_t kMaxCodeLength = 15;
constexpr uint32_t kMaxGroups = 4096;
constexpr uint32_t kCodeLengthOrder[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr uint8_t kPlaneCodes[120] = {
    0x18, 0x07, 0x17, 0x19, 0x28, 0x06, 0x27, 0x29, 0x16, 0x1a, 0x26, 0x2a, 0x38, 0x05, 0x37, 0x39, 0x15, 0x1b, 0x36, 0x3a, 0x25, 0x2b, 0x48, 0x04,
    0x47, 0x49, 0x14, 0x1c, 0x35, 0x3b, 0x46, 0x4a, 0x24, 0x2c, 0x58, 0x45, 0x4b, 0x34, 0x3c, 0x03, 0x57, 0x59, 0x13, 0x1d, 0x56, 0x5a, 0x23, 0x2d,
    0x44, 0x4c, 0x55, 0x5b, 0x33, 0x3d, 0x68, 0x02, 0x67, 0x69, 0x12, 0x1e, 0x66, 0x6a, 0x22, 0x2e, 0x54, 0x5c, 0x43, 0x4d, 0x65, 0x6b, 0x32, 0x3e,
    0x78, 0x01, 0x77, 0x79, 0x53, 0x5d, 0x11, 0x1f, 0x64, 0x6c, 0x42, 0x4e, 0x76, 0x7a, 0x21, 0x2f, 0x75, 0x7b, 0x31, 0x3f, 0x63, 0x6d, 0x52, 0x5e,
    0x00, 0x74, 0x7c, 0x41, 0x4f, 0x10, 0x20, 0x62, 0x6e, 0x30, 0x73, 0x7d, 0x51, 0x5f, 0x40, 0x72, 0x7e, 0x61, 0x6f, 0x50, 0x71, 0x7f, 0x60, 0x70};

uint32_t div_round_up(uint32_t a, uint32_t bits) { return (a + (1u << bits) - 1) >> bits; }

struct BitReader {
  ConstByteView d;
  uint64_t pos = 0;  // bit position
  bool failed = false;
  uint32_t read(uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const uint64_t byte = pos >> 3;
      if (byte >= d.size()) { failed = true; return v; }
      v |= uint32_t{(d[byte] >> (pos & 7)) & 1u} << i;
      ++pos;
    }
    return v;
  }
};

// A canonical prefix code, decoded a bit at a time.
struct Code {
  uint16_t count[kMaxCodeLength + 1];
  uint16_t* symbols = nullptr;  // sorted by code length, then symbol value
  uint16_t single = 0;
  bool is_single = false;
};

struct Group {
  Code code[kCodesPerGroup];
};

uint32_t decode_symbol(BitReader& r, const Code& c) {
  if (c.is_single) return c.single;
  int code = 0, first = 0, index = 0;
  for (uint32_t len = 1; len <= kMaxCodeLength; ++len) {
    code |= static_cast<int>(r.read(1));
    const int count = c.count[len];
    if (code - count < first) return c.symbols[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
    if (r.failed) return 0;
  }
  r.failed = true;
  return 0;
}

// Builds the decoding tables from code lengths. False for an over-subscribed or incomplete code (except exactly one used
// symbol, which takes no bits).
bool build_code(const uint8_t* lengths, uint32_t alphabet, Code* c, uint16_t* storage) {
  for (uint32_t i = 0; i <= kMaxCodeLength; ++i) c->count[i] = 0;
  uint32_t used = 0, last = 0;
  for (uint32_t s = 0; s < alphabet; ++s)
    if (lengths[s]) { ++c->count[lengths[s]]; ++used; last = s; }
  c->symbols = storage;
  c->is_single = false;
  if (used == 0) return false;
  if (used == 1) {
    c->is_single = true;
    c->single = static_cast<uint16_t>(last);
    return true;
  }
  int left = 1;
  for (uint32_t len = 1; len <= kMaxCodeLength; ++len) {
    left <<= 1;
    left -= c->count[len];
    if (left < 0) return false;
  }
  if (left != 0) return false;
  uint16_t offsets[kMaxCodeLength + 2];
  offsets[1] = 0;
  for (uint32_t len = 1; len <= kMaxCodeLength; ++len) offsets[len + 1] = static_cast<uint16_t>(offsets[len] + c->count[len]);
  for (uint32_t s = 0; s < alphabet; ++s)
    if (lengths[s]) storage[offsets[lengths[s]]++] = static_cast<uint16_t>(s);
  return true;
}

uint32_t add_pixels(uint32_t a, uint32_t b) {
  return (((a & 0xFF00FF00u) + (b & 0xFF00FF00u)) & 0xFF00FF00u) | (((a & 0x00FF00FFu) + (b & 0x00FF00FFu)) & 0x00FF00FFu);
}
uint32_t average2(uint32_t a, uint32_t b) { return (((a ^ b) & 0xFEFEFEFEu) >> 1) + (a & b); }
int32_t clamp255(int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
uint32_t select_pixel(uint32_t left, uint32_t top, uint32_t topleft) {
  int32_t pl = 0, pt = 0;
  for (int s = 0; s < 32; s += 8) {
    const int32_t l = (left >> s) & 255, t = (top >> s) & 255, tl = (topleft >> s) & 255;
    const int32_t p = l + t - tl;
    pl += p > l ? p - l : l - p;
    pt += p > t ? p - t : t - p;
  }
  return pl < pt ? left : top;
}
uint32_t clamp_add_subtract_full(uint32_t a, uint32_t b, uint32_t c) {
  uint32_t out = 0;
  for (int s = 0; s < 32; s += 8) out |= static_cast<uint32_t>(clamp255(static_cast<int32_t>((a >> s) & 255) + static_cast<int32_t>((b >> s) & 255) - static_cast<int32_t>((c >> s) & 255))) << s;
  return out;
}
uint32_t clamp_add_subtract_half(uint32_t a, uint32_t b) {
  uint32_t out = 0;
  for (int s = 0; s < 32; s += 8) {
    const int32_t x = (a >> s) & 255, y = (b >> s) & 255;
    out |= static_cast<uint32_t>(clamp255(x + (x - y) / 2)) << s;
  }
  return out;
}
uint32_t predict(uint32_t mode, uint32_t left, uint32_t top, uint32_t topright, uint32_t topleft) {
  switch (mode) {
    case 0: return 0xFF000000u;
    case 1: return left;
    case 2: return top;
    case 3: return topright;
    case 4: return topleft;
    case 5: return average2(average2(left, topright), top);
    case 6: return average2(left, topleft);
    case 7: return average2(left, top);
    case 8: return average2(topleft, top);
    case 9: return average2(top, topright);
    case 10: return average2(average2(left, topleft), average2(top, topright));
    case 11: return select_pixel(left, top, topleft);
    case 12: return clamp_add_subtract_full(left, top, topleft);
    case 13: return clamp_add_subtract_half(average2(left, top), topleft);
    default: return 0xFF000000u;
  }
}

enum TransformType { Predictor = 0, CrossColor = 1, SubtractGreen = 2, ColorIndexing = 3 };
struct Transform {
  TransformType type;
  uint32_t bits = 0;
  uint32_t width_in = 0;  // the image width when this transform was applied (by the encoder)
  ArrayView<uint32_t> data;
};

class Decoder {
 public:
  Decoder(allocators::IAllocator& allocator, BitReader& r) : allocator_(allocator), r_(r) {}
  ~Decoder() {
    for (uint32_t i = 0; i < transform_count_; ++i) release_view(allocator_, transforms_[i].data);
  }

  bool decode(Image* out);

 private:
  bool decode_stream(uint32_t xsize, uint32_t ysize, bool main, ArrayView<uint32_t> pixels);
  bool read_code(uint32_t alphabet, Code* code, uint16_t* storage);
  bool inverse_transforms(ArrayView<uint32_t>* pixels, uint32_t height, uint32_t* width);

  allocators::IAllocator& allocator_;
  BitReader& r_;
  Transform transforms_[4];
  uint32_t transform_count_ = 0;
};

bool Decoder::read_code(uint32_t alphabet, Code* code, uint16_t* storage) {
  uint8_t lengths[2328 + 16];
  for (uint32_t i = 0; i < alphabet; ++i) lengths[i] = 0;
  if (r_.read(1)) {  // simple: one or two symbols, 1 or 8 bits each
    const uint32_t n = r_.read(1) + 1;
    const bool first8 = r_.read(1);
    const uint32_t s0 = r_.read(first8 ? 8 : 1);
    if (s0 >= alphabet) return false;
    lengths[s0] = 1;
    if (n == 2) {
      const uint32_t s1 = r_.read(8);
      if (s1 >= alphabet || s1 == s0) return false;
      lengths[s1] = 1;
    }
  } else {
    uint8_t cl_lengths[19] = {};
    const uint32_t num = r_.read(4) + 4;
    for (uint32_t i = 0; i < num; ++i) cl_lengths[kCodeLengthOrder[i]] = static_cast<uint8_t>(r_.read(3));
    Code cl;
    uint16_t cl_storage[19];
    if (r_.failed || !build_code(cl_lengths, 19, &cl, cl_storage)) return false;
    uint32_t max_symbol = alphabet;
    if (r_.read(1)) {
      const uint32_t length_bits = 2 + 2 * r_.read(3);
      max_symbol = 2 + r_.read(length_bits);
      if (max_symbol > alphabet) return false;
    }
    uint32_t symbol = 0, previous = 8;
    while (symbol < alphabet && max_symbol-- > 0) {
      const uint32_t c = decode_symbol(r_, cl);
      if (r_.failed) return false;
      if (c < 16) {
        lengths[symbol++] = static_cast<uint8_t>(c);
        if (c) previous = c;
      } else {
        const uint32_t extra = c == 16 ? 2 : c == 17 ? 3 : 7;
        uint32_t repeat = r_.read(extra) + (c == 16 ? 3 : c == 17 ? 3 : 11);
        const uint32_t value = c == 16 ? previous : 0;
        if (symbol + repeat > alphabet) return false;
        while (repeat-- > 0) lengths[symbol++] = static_cast<uint8_t>(value);
      }
    }
  }
  if (r_.failed) return false;
  return build_code(lengths, alphabet, code, storage);
}

bool Decoder::decode_stream(uint32_t xsize, uint32_t ysize, bool main, ArrayView<uint32_t> pixels) {
  uint32_t cache_bits = 0;
  if (r_.read(1)) {
    cache_bits = r_.read(4);
    if (cache_bits < 1 || cache_bits > 11) return false;
  }
  uint32_t meta_bits = 0, groups = 1;
  ArrayView<uint32_t> meta;
  if (main && r_.read(1)) {
    meta_bits = r_.read(3) + 2;
    const uint32_t mw = div_round_up(xsize, meta_bits), mh = div_round_up(ysize, meta_bits);
    meta = allocate_view<uint32_t>(allocator_, uint64_t{mw} * mh);
    if (meta.empty()) return false;
    bool ok = decode_stream(mw, mh, false, meta);
    uint32_t top = 0;
    for (uint32_t v : meta) { const uint32_t g = (v >> 8) & 0xFFFF; if (g > top) top = g; }
    groups = top + 1;
    if (!ok || groups > kMaxGroups) { release_view(allocator_, meta); return false; }
  }
  const uint32_t cache_size = cache_bits ? 1u << cache_bits : 0;
  const uint32_t alphabets[kCodesPerGroup] = {256 + 24 + cache_size, 256, 256, 256, 40};
  uint32_t per_group_symbols = 0;
  for (uint32_t a : alphabets) per_group_symbols += a;
  Group* group_array = static_cast<Group*>(allocator_.allocate(sizeof(Group) * groups));
  uint16_t* storage = static_cast<uint16_t*>(allocator_.allocate(sizeof(uint16_t) * uint64_t{per_group_symbols} * groups));
  uint32_t* cache = cache_size ? static_cast<uint32_t*>(allocator_.allocate(sizeof(uint32_t) * cache_size)) : nullptr;
  bool ok = group_array && storage && (cache_size == 0 || cache);
  for (uint32_t g = 0; ok && g < groups; ++g) {
    uint16_t* at = storage + uint64_t{per_group_symbols} * g;
    for (uint32_t k = 0; ok && k < kCodesPerGroup; ++k) {
      ok = read_code(alphabets[k], &group_array[g].code[k], at);
      at += alphabets[k];
    }
  }
  if (ok && cache) for (uint32_t i = 0; i < cache_size; ++i) cache[i] = 0;
  const uint64_t total = uint64_t{xsize} * ysize;
  uint64_t pos = 0;
  const uint32_t meta_w = meta_bits ? div_round_up(xsize, meta_bits) : 0;
  const Group* group = group_array;
  auto pick_group = [&](uint64_t p) {
    if (meta_bits) {
      const uint32_t x = static_cast<uint32_t>(p % xsize), y = static_cast<uint32_t>(p / xsize);
      const uint32_t g = (meta[uint64_t{y >> meta_bits} * meta_w + (x >> meta_bits)] >> 8) & 0xFFFF;
      group = &group_array[g];
    }
  };
  auto prefix_value = [&](uint32_t symbol) {
    if (symbol < 4) return symbol + 1;
    const uint32_t extra = (symbol - 2) >> 1, offset = (2 + (symbol & 1)) << extra;
    return offset + r_.read(extra) + 1;
  };
  while (ok && pos < total) {
    if (meta_bits) pick_group(pos);  // (a copy can end in the middle of a tile, so look it up for every symbol)
    const uint32_t s = decode_symbol(r_, group->code[0]);
    if (r_.failed) { ok = false; break; }
    if (s < 256) {
      const uint32_t red = decode_symbol(r_, group->code[1]), blue = decode_symbol(r_, group->code[2]), alpha = decode_symbol(r_, group->code[3]);
      const uint32_t px = alpha << 24 | red << 16 | s << 8 | blue;
      pixels[pos++] = px;
      if (cache) cache[(0x1e35a7bdu * px) >> (32 - cache_bits)] = px;
    } else if (s < 256 + 24) {
      const uint32_t length = prefix_value(s - 256);
      const uint32_t dist_code = prefix_value(decode_symbol(r_, group->code[4]));
      uint64_t dist;
      if (dist_code > 120) {
        dist = dist_code - 120;
      } else {
        const uint32_t v = kPlaneCodes[dist_code - 1];
        const int32_t dx = 8 - static_cast<int32_t>(v & 15), dy = static_cast<int32_t>(v >> 4);
        const int64_t d = int64_t{dx} + int64_t{dy} * xsize;
        dist = d < 1 ? 1 : static_cast<uint64_t>(d);
      }
      if (r_.failed || dist > pos || pos + length > total) { ok = false; break; }
      for (uint32_t i = 0; i < length; ++i, ++pos) {
        const uint32_t px = pixels[pos - dist];
        pixels[pos] = px;
        if (cache) cache[(0x1e35a7bdu * px) >> (32 - cache_bits)] = px;
      }
    } else {
      const uint32_t index = s - 280;
      if (!cache || index >= cache_size) { ok = false; break; }
      const uint32_t px = cache[index];
      pixels[pos++] = px;
      cache[(0x1e35a7bdu * px) >> (32 - cache_bits)] = px;
    }
  }
  if (r_.failed) ok = false;
  allocator_.deallocate(group_array);
  allocator_.deallocate(storage);
  allocator_.deallocate(cache);
  release_view(allocator_, meta);
  return ok;
}

// Undoes the transforms, last read first. The pixel buffer may be replaced (colour indexing widens the image).
bool Decoder::inverse_transforms(ArrayView<uint32_t>* pixels, uint32_t height, uint32_t* width) {
  for (uint32_t ti = transform_count_; ti-- > 0;) {
    Transform& t = transforms_[ti];
    const uint32_t w = t.width_in;
    ArrayView<uint32_t> px = *pixels;
    switch (t.type) {
      case SubtractGreen:
        for (uint64_t i = 0; i < px.size(); ++i) {
          const uint32_t p = px[i], g = (p >> 8) & 255;
          px[i] = (p & 0xFF00FF00u) | ((((p & 0x00FF0000u) >> 16) + g) & 255) << 16 | (((p & 255) + g) & 255);
        }
        break;
      case CrossColor: {
        const uint32_t blocks_w = div_round_up(w, t.bits);
        for (uint32_t y = 0; y < height; ++y)
          for (uint32_t x = 0; x < w; ++x) {
            const uint32_t m = t.data[uint64_t{y >> t.bits} * blocks_w + (x >> t.bits)];
            const int32_t g2r = static_cast<int8_t>(m & 255), g2b = static_cast<int8_t>((m >> 8) & 255), r2b = static_cast<int8_t>((m >> 16) & 255);
            uint32_t& p = px[uint64_t{y} * w + x];
            const int32_t green = static_cast<int8_t>((p >> 8) & 255);
            int32_t red = (p >> 16) & 255, blue = p & 255;
            red = (red + ((g2r * green) >> 5)) & 255;
            blue = (blue + ((g2b * green) >> 5)) & 255;
            blue = (blue + ((r2b * static_cast<int8_t>(red)) >> 5)) & 255;
            p = (p & 0xFF00FF00u) | static_cast<uint32_t>(red) << 16 | static_cast<uint32_t>(blue);
          }
        break;
      }
      case Predictor: {
        const uint32_t blocks_w = div_round_up(w, t.bits);
        for (uint32_t y = 0; y < height; ++y)
          for (uint32_t x = 0; x < w; ++x) {
            const uint64_t i = uint64_t{y} * w + x;
            uint32_t prediction;
            if (y == 0) prediction = x == 0 ? 0xFF000000u : px[i - 1];
            else if (x == 0) prediction = px[i - w];
            else {
              const uint32_t mode = (t.data[uint64_t{y >> t.bits} * blocks_w + (x >> t.bits)] >> 8) & 15;
              prediction = predict(mode, px[i - 1], px[i - w], px[i - w + 1], px[i - w - 1]);
            }
            px[i] = add_pixels(px[i], prediction);
          }
        break;
      }
      case ColorIndexing: {
        const uint32_t colors = static_cast<uint32_t>(t.data.size());
        ArrayView<uint32_t> wide = allocate_view<uint32_t>(allocator_, uint64_t{w} * height);
        if (wide.empty()) return false;
        const uint32_t packed_w = *width;
        const uint32_t per_byte = 1u << t.bits, bits_each = 8 >> t.bits, mask = (1u << bits_each) - 1;
        for (uint32_t y = 0; y < height; ++y)
          for (uint32_t x = 0; x < w; ++x) {
            uint32_t index;
            if (t.bits == 0) index = (px[uint64_t{y} * packed_w + x] >> 8) & 255;
            else index = ((px[uint64_t{y} * packed_w + x / per_byte] >> 8) >> (bits_each * (x % per_byte))) & mask;
            wide[uint64_t{y} * w + x] = index < colors ? t.data[index] : 0;
          }
        release_view(allocator_, *pixels);
        *pixels = wide;
        break;
      }
    }
    *width = w;
  }
  return true;
}

bool Decoder::decode(Image* out) {
  if (r_.read(8) != 0x2F) return false;
  const uint32_t width = r_.read(14) + 1, height = r_.read(14) + 1;
  r_.read(1);  // alpha_is_used: only a hint; the pixels carry the alpha
  if (r_.read(3) != 0 || r_.failed || uint64_t{width} * height > Image::kMaxPixels) return false;
  uint32_t xsize = width;
  uint32_t used = 0;
  while (r_.read(1)) {
    if (transform_count_ == 4 || r_.failed) return false;
    Transform& t = transforms_[transform_count_];
    t.type = static_cast<TransformType>(r_.read(2));
    if (used & (1u << t.type)) return false;
    used |= 1u << t.type;
    t.width_in = xsize;
    if (t.type == Predictor || t.type == CrossColor) {
      t.bits = r_.read(3) + 2;
      const uint32_t bw = div_round_up(xsize, t.bits), bh = div_round_up(height, t.bits);
      t.data = allocate_view<uint32_t>(allocator_, uint64_t{bw} * bh);
      if (t.data.empty()) return false;
      ++transform_count_;
      if (!decode_stream(bw, bh, false, t.data)) return false;
    } else if (t.type == ColorIndexing) {
      const uint32_t colors = r_.read(8) + 1;
      t.bits = colors <= 2 ? 3 : colors <= 4 ? 2 : colors <= 16 ? 1 : 0;
      t.data = allocate_view<uint32_t>(allocator_, colors);
      if (t.data.empty()) return false;
      ++transform_count_;
      if (!decode_stream(colors, 1, false, t.data)) return false;
      for (uint32_t i = 1; i < colors; ++i) t.data[i] = add_pixels(t.data[i], t.data[i - 1]);
      xsize = div_round_up(xsize, t.bits);
    } else {
      ++transform_count_;
    }
  }
  if (r_.failed) return false;
  ArrayView<uint32_t> pixels = allocate_view<uint32_t>(allocator_, uint64_t{xsize} * height);
  if (pixels.empty()) return false;
  bool ok = decode_stream(xsize, height, true, pixels);
  uint32_t w = xsize;
  ok = ok && inverse_transforms(&pixels, height, &w) && w == width && out->create(width, height, 1);
  bool alpha = false;
  if (ok) {
    ArrayView<uint32_t> dest = out->frame(0);
    for (uint64_t i = 0; i < dest.size(); ++i) {
      dest[i] = pixels[i];
      alpha = alpha || (pixels[i] >> 24) != 255;
    }
    out->set_has_alpha(alpha);
  }
  release_view(allocator_, pixels);
  return ok;
}

uint32_t le32(ConstByteView d, uint64_t at) { return d[at] | (d[at + 1] << 8) | (d[at + 2] << 16) | (uint32_t{d[at + 3]} << 24); }
bool tag_is(ConstByteView d, uint64_t at, const char* tag) { return d[at] == static_cast<uint8_t>(tag[0]) && d[at + 1] == static_cast<uint8_t>(tag[1]) && d[at + 2] == static_cast<uint8_t>(tag[2]) && d[at + 3] == static_cast<uint8_t>(tag[3]); }
}  // namespace

bool decode_webp(ConstByteView data, Image* out) {
  out->clear();
  if (data.size() < 20 || detect_image_format(data) != ImageFormat::Webp) return false;
  uint64_t at = 12;
  while (at + 8 <= data.size()) {
    const uint64_t size = le32(data, at + 4), body = at + 8;
    const uint64_t length = size < data.size() - body ? size : data.size() - body;
    if (tag_is(data, at, "VP8L")) {
      BitReader reader{data.subview(body, length)};
      Decoder decoder(out->allocator(), reader);
      if (decoder.decode(out)) return true;
      out->clear();
      return false;
    }
    if (tag_is(data, at, "VP8 ") || tag_is(data, at, "ANMF")) return false;  // lossy or animated: not handled
    at = body + size + (size & 1);
  }
  return false;
}

// ---- Encoding -------------------------------------------------------------------------------

namespace {
struct BitWriter {
  ByteBuffer* out;
  uint64_t acc = 0;
  uint32_t n = 0;
  bool ok = true;
  void write(uint32_t value, uint32_t bits) {
    acc |= uint64_t{value} << n;
    n += bits;
    while (n >= 8) {
      ok = ok && out->put8(static_cast<uint8_t>(acc));
      acc >>= 8;
      n -= 8;
    }
  }
  void finish() {
    if (n) ok = ok && out->put8(static_cast<uint8_t>(acc));
    acc = 0;
    n = 0;
  }
};

// Code lengths for `freq` (at most kMaxCodeLength): Huffman, flattening the counts until the longest code fits.
void huffman_lengths(const uint32_t* freq, uint32_t n, uint8_t* lengths, allocators::IAllocator& allocator) {
  uint32_t* f = static_cast<uint32_t*>(allocator.allocate(sizeof(uint32_t) * n));
  uint32_t* weight = static_cast<uint32_t*>(allocator.allocate(sizeof(uint32_t) * 2 * n));
  int32_t* parent = static_cast<int32_t*>(allocator.allocate(sizeof(int32_t) * 2 * n));
  for (uint32_t i = 0; i < n; ++i) lengths[i] = 0;
  if (!f || !weight || !parent) { allocator.deallocate(f); allocator.deallocate(weight); allocator.deallocate(parent); return; }
  for (uint32_t i = 0; i < n; ++i) f[i] = freq[i];
  for (int attempt = 0; attempt < 32; ++attempt) {
    uint32_t used = 0;
    for (uint32_t i = 0; i < n; ++i) { weight[i] = f[i]; parent[i] = -1; used += f[i] != 0; }
    if (used == 0) break;
    if (used == 1) { for (uint32_t i = 0; i < n; ++i) lengths[i] = f[i] ? 1 : 0; break; }
    // Repeatedly merge the two lightest live nodes (a simple O(n^2) pass is plenty for alphabets this size).
    uint32_t nodes = n;
    for (uint32_t i = 0; i < n; ++i) if (f[i] == 0) parent[i] = -2;  // unused
    for (uint32_t merges = 0; merges + 1 < used; ++merges) {
      int32_t a = -1, b = -1;
      for (uint32_t i = 0; i < nodes; ++i) {
        if (parent[i] != -1) continue;
        if (a < 0 || weight[i] < weight[a]) { b = a; a = static_cast<int32_t>(i); }
        else if (b < 0 || weight[i] < weight[b]) b = static_cast<int32_t>(i);
      }
      weight[nodes] = weight[a] + weight[b];
      parent[nodes] = -1;
      parent[a] = parent[b] = static_cast<int32_t>(nodes);
      ++nodes;
    }
    uint32_t longest = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (f[i] == 0) continue;
      uint32_t depth = 0;
      for (int32_t p = parent[i]; p >= 0; p = parent[p]) ++depth;
      lengths[i] = static_cast<uint8_t>(depth);
      if (depth > longest) longest = depth;
    }
    if (longest <= kMaxCodeLength) break;
    for (uint32_t i = 0; i < n; ++i) if (f[i]) f[i] = (f[i] + 1) / 2;
  }
  allocator.deallocate(f);
  allocator.deallocate(weight);
  allocator.deallocate(parent);
}

// Canonical codes (MSB-first bit patterns) for the lengths.
void assign_codes(const uint8_t* lengths, uint32_t n, uint16_t* codes) {
  uint32_t count[kMaxCodeLength + 2] = {}, next[kMaxCodeLength + 2] = {};
  for (uint32_t i = 0; i < n; ++i) ++count[lengths[i]];
  count[0] = 0;
  uint32_t code = 0;
  for (uint32_t len = 1; len <= kMaxCodeLength; ++len) {
    code = (code + count[len - 1]) << 1;
    next[len] = code;
  }
  for (uint32_t i = 0; i < n; ++i) codes[i] = lengths[i] ? static_cast<uint16_t>(next[lengths[i]]++) : 0;
}

uint32_t reverse_bits(uint32_t v, uint32_t n) {
  uint32_t r = 0;
  for (uint32_t i = 0; i < n; ++i) r |= ((v >> i) & 1u) << (n - 1 - i);
  return r;
}

struct Token {
  uint32_t literal_or_length;  // the ARGB pixel, or the copy length (when dist != 0)
  uint32_t dist;
};

struct PrefixSplit {
  uint32_t symbol, extra_bits, extra_value;
};
PrefixSplit split_prefix(uint32_t value) {
  const uint32_t d = value - 1;
  if (d < 4) return {d, 0, 0};
  uint32_t highest = 31;
  while (!((d >> highest) & 1)) --highest;
  const uint32_t second = (d >> (highest - 1)) & 1, extra = highest - 1;
  return {2 * highest + second, extra, d & ((1u << extra) - 1)};
}

struct CodeBook {
  uint8_t lengths[280];
  uint16_t codes[280];
  uint32_t alphabet;
};

void write_code(BitWriter& w, CodeBook& book, const uint32_t* freq, allocators::IAllocator& allocator) {
  const uint32_t n = book.alphabet;
  huffman_lengths(freq, n, book.lengths, allocator);
  uint32_t used = 0, first = 0, second = 0;
  for (uint32_t i = 0; i < n; ++i)
    if (book.lengths[i]) { if (used == 0) first = i; else if (used == 1) second = i; ++used; }
  if (used == 0) {  // nothing is ever coded: a single placeholder symbol
    book.lengths[0] = 1;
    used = 1;
    first = 0;
  }
  if (used == 1 && first < 256) {
    for (uint32_t i = 0; i < n; ++i) book.lengths[i] = i == first ? 1 : 0;
    w.write(1, 1);  // simple
    w.write(0, 1);  // one symbol
    if (first < 2) { w.write(0, 1); w.write(first, 1); } else { w.write(1, 1); w.write(first, 8); }
    assign_codes(book.lengths, n, book.codes);
    return;
  }
  if (used == 2 && second < 256 && first < 256) {
    for (uint32_t i = 0; i < n; ++i) book.lengths[i] = (i == first || i == second) ? 1 : 0;
    w.write(1, 1);
    w.write(1, 1);  // two symbols
    if (first < 2) { w.write(0, 1); w.write(first, 1); } else { w.write(1, 1); w.write(first, 8); }
    w.write(second, 8);
    assign_codes(book.lengths, n, book.codes);
    return;
  }
  // A normal code. The lengths are sent with run-length symbols for zeros, themselves coded with a small code.
  if (used == 1) {  // one symbol above 255: give it a second partner so that the code is a proper tree
    const uint32_t partner = first == 0 ? 1 : 0;
    book.lengths[partner] = 1;
    book.lengths[first] = 1;
  }
  uint8_t symbols[280 * 2], extras[280 * 2];
  uint32_t count = 0;
  for (uint32_t i = 0; i < n;) {
    if (book.lengths[i] == 0) {
      uint32_t run = 1;
      while (i + run < n && book.lengths[i + run] == 0) ++run;
      uint32_t left = run;
      while (left >= 11) { const uint32_t r = left > 138 ? 138 : left; symbols[count] = 18; extras[count++] = static_cast<uint8_t>(r - 11); left -= r; }
      while (left >= 3) { const uint32_t r = left > 10 ? 10 : left; symbols[count] = 17; extras[count++] = static_cast<uint8_t>(r - 3); left -= r; }
      while (left-- > 0) { symbols[count] = 0; extras[count++] = 0; }
      i += run;
    } else {
      symbols[count] = book.lengths[i];
      extras[count++] = 0;
      ++i;
    }
  }
  uint32_t cl_freq[19] = {};
  for (uint32_t i = 0; i < count; ++i) ++cl_freq[symbols[i]];
  uint8_t cl_lengths[19];
  uint16_t cl_codes[19];
  // The code-length code is limited to 7 bits.
  uint32_t cf[19];
  for (uint32_t i = 0; i < 19; ++i) cf[i] = cl_freq[i];
  for (int attempt = 0; attempt < 16; ++attempt) {
    huffman_lengths(cf, 19, cl_lengths, allocator);
    uint32_t longest = 0;
    for (uint32_t i = 0; i < 19; ++i) if (cl_lengths[i] > longest) longest = cl_lengths[i];
    if (longest <= 7) break;
    for (uint32_t i = 0; i < 19; ++i) if (cf[i]) cf[i] = (cf[i] + 1) / 2;
  }
  uint32_t cl_used = 0;
  for (uint32_t i = 0; i < 19; ++i) cl_used += cl_lengths[i] != 0;
  assign_codes(cl_lengths, 19, cl_codes);
  w.write(0, 1);  // normal
  uint32_t num = 19;
  while (num > 4 && cl_lengths[kCodeLengthOrder[num - 1]] == 0) --num;
  w.write(num - 4, 4);
  for (uint32_t i = 0; i < num; ++i) w.write(cl_lengths[kCodeLengthOrder[i]], 3);
  w.write(0, 1);  // all symbols are coded
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t s = symbols[i];
    if (cl_used > 1) w.write(reverse_bits(cl_codes[s], cl_lengths[s]), cl_lengths[s]);
    if (s == 16) w.write(extras[i], 2);
    else if (s == 17) w.write(extras[i], 3);
    else if (s == 18) w.write(extras[i], 7);
  }
  assign_codes(book.lengths, n, book.codes);
}
}  // namespace

// Subtract-green, then LZ77 (matches against the previous pixel, the pixel above, and a hash of the last occurrence of a pixel
// pair) with one prefix code per channel and no colour cache.
bool encode_webp(const Image& image, ByteBuffer* out) {
  if (image.empty() || image.width() > 16384 || image.height() > 16384) return false;
  allocators::IAllocator& allocator = image.allocator();
  const uint64_t start = out->size();
  const uint32_t width = image.width(), height = image.height();
  const uint64_t count = uint64_t{width} * height;
  ArrayView<uint32_t> px = allocate_view<uint32_t>(allocator, count);
  if (px.empty()) return false;
  bool has_alpha = false;
  for (uint64_t i = 0; i < count; ++i) {
    const uint32_t p = image.frame(0)[i], g = (p >> 8) & 255;
    has_alpha = has_alpha || (p >> 24) != 255;
    px[i] = (p & 0xFF00FF00u) | ((((p >> 16) & 255) - g) & 255) << 16 | (((p & 255) - g) & 255);
  }

  // Tokens.
  constexpr uint32_t kHashBits = 16;
  int64_t* last = static_cast<int64_t*>(allocator.allocate(sizeof(int64_t) << kHashBits));
  Token* tokens = static_cast<Token*>(allocator.allocate(sizeof(Token) * count));
  if (!last || !tokens) { allocator.deallocate(last); allocator.deallocate(tokens); release_view(allocator, px); return false; }
  for (uint32_t i = 0; i < (1u << kHashBits); ++i) last[i] = -1;
  uint64_t token_count = 0;
  auto match_length = [&](uint64_t pos, uint64_t dist) {
    uint32_t n = 0;
    while (pos + n < count && n < 4096 && px[pos + n] == px[pos + n - dist]) ++n;
    return n;
  };
  for (uint64_t pos = 0; pos < count;) {
    uint32_t best_len = 0;
    uint64_t best_dist = 0;
    auto try_dist = [&](uint64_t dist) {
      if (dist == 0 || dist > pos || dist > 1048576 - 120) return;
      const uint32_t n = match_length(pos, dist);
      if (n > best_len) { best_len = n; best_dist = dist; }
    };
    try_dist(1);
    try_dist(width);
    if (pos + 1 < count) {
      const uint32_t h = ((px[pos] * 0x9E3779B1u) ^ (px[pos + 1] * 0x85EBCA6Bu)) >> (32 - kHashBits);
      if (last[h] >= 0) try_dist(pos - static_cast<uint64_t>(last[h]));
      last[h] = static_cast<int64_t>(pos);
    }
    if (best_len >= 3) {
      tokens[token_count++] = {best_len, static_cast<uint32_t>(best_dist)};
      pos += best_len;
    } else {
      tokens[token_count++] = {px[pos], 0};
      ++pos;
    }
  }
  uint32_t freq[5][280] = {};
  for (uint64_t i = 0; i < token_count; ++i) {
    const Token& t = tokens[i];
    if (t.dist == 0) {
      ++freq[0][(t.literal_or_length >> 8) & 255];
      ++freq[1][(t.literal_or_length >> 16) & 255];
      ++freq[2][t.literal_or_length & 255];
      ++freq[3][t.literal_or_length >> 24];
    } else {
      ++freq[0][256 + split_prefix(t.literal_or_length).symbol];
      ++freq[4][split_prefix(t.dist + 120).symbol];
    }
  }

  bool ok = out->put_tag("RIFF") && out->put32(0) && out->put_tag("WEBP") && out->put_tag("VP8L") && out->put32(0);
  const uint64_t payload_start = out->size();
  BitWriter w{out};
  w.write(0x2F, 8);
  w.write(width - 1, 14);
  w.write(height - 1, 14);
  w.write(has_alpha ? 1 : 0, 1);
  w.write(0, 3);
  w.write(1, 1);  // a transform: subtract green
  w.write(SubtractGreen, 2);
  w.write(0, 1);  // no more
  w.write(0, 1);  // no colour cache
  w.write(0, 1);  // no meta prefix image
  CodeBook books[5];
  const uint32_t alphabets[5] = {280, 256, 256, 256, 40};
  for (int k = 0; k < 5; ++k) {
    books[k].alphabet = alphabets[k];
    write_code(w, books[k], freq[k], allocator);
  }
  auto put = [&](const CodeBook& b, uint32_t symbol) {
    if (b.lengths[symbol]) w.write(reverse_bits(b.codes[symbol], b.lengths[symbol]), b.lengths[symbol]);
  };
  // A code with a single used symbol takes no bits; `put` writes its 1-bit placeholder code only when two symbols exist.
  auto single = [](const CodeBook& b) {
    uint32_t used = 0;
    for (uint32_t i = 0; i < b.alphabet; ++i) used += b.lengths[i] != 0;
    return used == 1;
  };
  bool single_flags[5];
  for (int k = 0; k < 5; ++k) single_flags[k] = single(books[k]);
  auto emit = [&](int k, uint32_t symbol) { if (!single_flags[k]) put(books[k], symbol); };
  for (uint64_t i = 0; i < token_count; ++i) {
    const Token& t = tokens[i];
    if (t.dist == 0) {
      emit(0, (t.literal_or_length >> 8) & 255);
      emit(1, (t.literal_or_length >> 16) & 255);
      emit(2, t.literal_or_length & 255);
      emit(3, t.literal_or_length >> 24);
    } else {
      const PrefixSplit len = split_prefix(t.literal_or_length), dist = split_prefix(t.dist + 120);
      emit(0, 256 + len.symbol);
      w.write(len.extra_value, len.extra_bits);
      emit(4, dist.symbol);
      w.write(dist.extra_value, dist.extra_bits);
    }
  }
  w.finish();
  ok = ok && w.ok;
  allocator.deallocate(last);
  allocator.deallocate(tokens);
  release_view(allocator, px);
  if (ok) {
    const uint64_t payload = out->size() - payload_start;
    if (payload & 1) ok = out->put8(0);
    ok = ok && out->patch32(start + 4, static_cast<uint32_t>(out->size() - start - 8)) && out->patch32(start + 16, static_cast<uint32_t>(payload));
  }
  if (!ok) out->truncate(start);
  return ok;
}

}  // namespace media
