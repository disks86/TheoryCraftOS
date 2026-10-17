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

struct Lzw {
  uint16_t prefix[4096];
  uint8_t suffix[4096];
  uint8_t stack[4097];
};

// Decodes GIF's variable-width LZW data (sub-blocks starting at data[*at]) into `indices` (count entries). Returns
// false if the data is bad; fewer codes than pixels leave the rest 0.
bool lzw_decode(ConstByteView data, uint64_t* at, uint32_t min_code_size, uint8_t* indices, uint64_t count, Lzw* t) {
  if (min_code_size < 1 || min_code_size > 11) return false;
  const uint32_t clear = 1u << min_code_size, eoi = clear + 1;
  uint32_t code_size = min_code_size + 1, next = eoi + 1, previous = 0xFFFFFFFF;
  uint32_t bits = 0, have = 0;
  uint64_t out = 0;
  uint8_t first = 0;
  uint64_t pos = *at;
  uint32_t remaining = 0;  // bytes left in the current sub-block
  bool finished = false;
  for (uint32_t i = 0; i < clear; ++i) {
    t->prefix[i] = 0xFFFF;
    t->suffix[i] = static_cast<uint8_t>(i);
  }
  while (!finished) {
    while (have < code_size) {  // refill: read the next byte from the sub-block chain
      if (remaining == 0) {
        if (pos >= data.size()) return false;
        remaining = data[pos++];
        if (remaining == 0) {
          finished = true;
          break;
        }
      }
      if (pos >= data.size()) return false;
      bits |= static_cast<uint32_t>(data[pos++]) << have;
      have += 8;
      --remaining;
    }
    if (finished && have < code_size) break;
    const uint32_t code = bits & ((1u << code_size) - 1);
    bits >>= code_size;
    have -= code_size;
    if (code == clear) {
      code_size = min_code_size + 1;
      next = eoi + 1;
      previous = 0xFFFFFFFF;
      continue;
    }
    if (code == eoi) break;
    uint32_t current = code;
    uint32_t depth = 0;
    if (previous == 0xFFFFFFFF) {
      if (code >= clear) return false;
      first = static_cast<uint8_t>(code);
      if (out < count) indices[out] = first;
      ++out;
      previous = code;
      continue;
    }
    if (code >= next) {  // the code that is being defined right now: previous string + its first byte
      if (code > next) return false;
      t->stack[depth++] = first;
      current = previous;
    }
    while (current >= clear) {
      if (depth >= 4096 || current >= 4096) return false;
      t->stack[depth++] = t->suffix[current];
      current = t->prefix[current];
    }
    first = static_cast<uint8_t>(current);
    t->stack[depth++] = first;
    while (depth > 0) {
      const uint8_t v = t->stack[--depth];
      if (out < count) indices[out] = v;
      ++out;
    }
    if (next < 4096) {
      t->prefix[next] = static_cast<uint16_t>(previous);
      t->suffix[next] = first;
      ++next;
      if (next == (1u << code_size) && code_size < 12) ++code_size;
    }
    previous = code;
  }
  // Skip to the end of the block chain.
  if (!finished) {
    while (true) {
      if (remaining > 0) {
        pos += remaining;
        remaining = 0;
      }
      if (pos >= data.size()) break;
      remaining = data[pos++];
      if (remaining == 0) break;
    }
  }
  *at = pos;
  return true;
}
}  // namespace

bool decode_gif(ConstByteView data, Image* out) {
  out->clear();
  if (data.size() < 13 || detect_image_format(data) != ImageFormat::Gif) return false;
  allocators::IAllocator& allocator = out->allocator();
  const uint32_t width = le16(data, 6), height = le16(data, 8);
  const uint32_t flags = data[10];
  if (width == 0 || height == 0 || uint64_t{width} * height > Image::kMaxPixels) return false;
  uint64_t at = 13;
  uint32_t global_palette[256] = {};
  uint32_t global_count = 0;
  if (flags & 0x80) {
    global_count = 2u << (flags & 7);
    if (at + 3 * uint64_t{global_count} > data.size()) return false;
    for (uint32_t i = 0; i < global_count; ++i)
      global_palette[i] = 0xFF000000u | (uint32_t{data[at + 3 * i]} << 16) | (uint32_t{data[at + 3 * i + 1]} << 8) | data[at + 3 * i + 2];
    at += 3 * uint64_t{global_count};
  }

  // First pass: count the frames, so the Image can be made.
  uint32_t frame_count = 0;
  {
    uint64_t p = at;
    while (p < data.size()) {
      const uint8_t block = data[p++];
      if (block == 0x3B) break;
      if (block == 0x21) {
        if (p >= data.size()) return false;
        ++p;
        while (p < data.size() && data[p] != 0) p += data[p] + 1;
        ++p;
      } else if (block == 0x2C) {
        if (p + 9 > data.size()) return false;
        const uint32_t local = data[p + 8];
        p += 9;
        if (local & 0x80) p += 3 * (2u << (local & 7));
        ++p;  // the minimum code size
        while (p < data.size() && data[p] != 0) p += data[p] + 1;
        ++p;
        ++frame_count;
        if (frame_count > 4096) return false;
      } else {
        return false;
      }
    }
  }
  if (frame_count == 0 || !out->create(width, height, frame_count)) return false;

  ArrayView<uint8_t> indices = allocate_view<uint8_t>(allocator, uint64_t{width} * height);
  Lzw* lzw = static_cast<Lzw*>(allocator.allocate(sizeof(Lzw)));
  if (indices.empty() || !lzw) {
    release_view(allocator, indices);
    allocator.deallocate(lzw);
    out->clear();
    return false;
  }
  ArrayView<uint32_t> saved = allocate_view<uint32_t>(allocator, uint64_t{width} * height);
  bool ok = !saved.empty();
  // Graphic control state, applying to the next frame.
  uint32_t disposal = 0, delay = 10, transparent = 0xFFFFFFFF;
  uint32_t frame = 0;
  uint32_t previous_disposal = 0, previous_x = 0, previous_y = 0, previous_w = 0, previous_h = 0;
  bool has_alpha = false;
  uint32_t loops = 1;
  while (ok && at < data.size() && frame < frame_count) {
    const uint8_t block = data[at++];
    if (block == 0x3B) break;
    if (block == 0x21) {
      if (at >= data.size()) { ok = false; break; }
      const uint8_t label = data[at++];
      if (label == 0xF9 && at + 5 <= data.size() && data[at] >= 4) {
        const uint32_t packed = data[at + 1];
        disposal = (packed >> 2) & 7;
        delay = le16(data, at + 2);
        transparent = (packed & 1) ? data[at + 4] : 0xFFFFFFFF;
      } else if (label == 0xFF && at + 17 <= data.size() && data[at] == 11 && data[at + 1] == 'N' && data[at + 2] == 'E' && data[at + 12] == 3 && data[at + 13] == 1) {
        loops = le16(data, at + 14);  // NETSCAPE2.0: 0 means for ever
      }
      while (at < data.size() && data[at] != 0) at += data[at] + 1;
      ++at;
    } else if (block == 0x2C) {
      if (at + 9 > data.size()) { ok = false; break; }
      const uint32_t x0 = le16(data, at), y0 = le16(data, at + 2), fw = le16(data, at + 4), fh = le16(data, at + 6), local = data[at + 8];
      at += 9;
      uint32_t palette[256];
      uint32_t count = global_count;
      for (uint32_t i = 0; i < 256; ++i) palette[i] = global_palette[i];
      if (local & 0x80) {
        count = 2u << (local & 7);
        if (at + 3 * uint64_t{count} > data.size()) { ok = false; break; }
        for (uint32_t i = 0; i < count; ++i) palette[i] = 0xFF000000u | (uint32_t{data[at + 3 * i]} << 16) | (uint32_t{data[at + 3 * i + 1]} << 8) | data[at + 3 * i + 2];
        at += 3 * uint64_t{count};
      }
      if (at >= data.size()) { ok = false; break; }
      const uint32_t min_code = data[at++];
      const bool interlaced = local & 0x40;
      if (x0 + uint64_t{fw} > width || y0 + uint64_t{fh} > height || fw == 0 || fh == 0) { ok = false; break; }
      indices.subview(0, uint64_t{fw} * fh).fill(0);
      if (!lzw_decode(data, &at, min_code, indices.data(), uint64_t{fw} * fh, lzw)) { ok = false; break; }

      // The canvas this frame starts from: the previous frame's, after its disposal.
      ArrayView<uint32_t> canvas = out->frame(frame);
      if (frame > 0) {
        canvas.copy_from(out->frame(frame - 1));
        if (previous_disposal == 2) {
          for (uint32_t y = 0; y < previous_h; ++y)
            for (uint32_t x = 0; x < previous_w; ++x) canvas[uint64_t{previous_y + y} * width + previous_x + x] = 0;
        } else if (previous_disposal == 3) {
          canvas.copy_from(saved);
        }
      }
      if (disposal == 3) saved.copy_from(canvas);
      static const uint8_t kPassStart[4] = {0, 4, 2, 1}, kPassStep[4] = {8, 8, 4, 2};
      uint32_t pass = 0, row = 0;
      for (uint32_t i = 0; i < fh; ++i) {
        uint32_t y;
        if (interlaced) {
          while (row >= fh) row = kPassStart[++pass];
          y = row;
          row += kPassStep[pass];
        } else {
          y = i;
        }
        for (uint32_t x = 0; x < fw; ++x) {
          const uint32_t index = indices[uint64_t{i} * fw + x];
          if (index == transparent) continue;  // leaves what is below
          canvas[uint64_t{y0 + y} * width + x0 + x] = index < count ? palette[index] : 0xFF000000u;
        }
      }
      uint32_t d = delay * 10;
      out->set_delay_ms(frame, d == 0 ? 100 : d);
      if (transparent != 0xFFFFFFFF) has_alpha = true;
      previous_disposal = disposal;
      previous_x = x0; previous_y = y0; previous_w = fw; previous_h = fh;
      disposal = 0;
      transparent = 0xFFFFFFFF;
      delay = 10;
      ++frame;
    } else {
      ok = false;
    }
  }
  release_view(allocator, indices);
  release_view(allocator, saved);
  allocator.deallocate(lzw);
  if (!ok || frame == 0) {
    out->clear();
    return false;
  }
  out->limit_frames(frame);  // a frame never reached (damaged file) is dropped: keep what was decoded
  out->set_loop_count(frame_count == 1 ? 0 : loops);
  out->set_has_alpha(has_alpha);
  return true;
}

// ---- Encoding -------------------------------------------------------------------------------

namespace {
// The palette: 6 x 7 x 6 colours (indices 0-251) and index 252 for transparent pixels; 253-255 unused (black).
constexpr uint32_t kReds = 6, kGreens = 7, kBlues = 6, kTransparentIndex = 252;

uint32_t level_value(uint32_t level, uint32_t levels) { return level * 255 / (levels - 1); }

// LZW with 8-bit symbols into GIF sub-blocks.
struct LzwWriter {
  ByteBuffer* out;
  uint8_t block[255];
  uint32_t block_size = 0;
  uint32_t bits = 0, have = 0;
  bool ok = true;
  void put_byte(uint8_t b) {
    block[block_size++] = b;
    if (block_size == 255) flush_block();
  }
  void flush_block() {
    if (block_size == 0) return;
    ok = ok && out->put8(static_cast<uint8_t>(block_size)) && out->append(ConstByteView(block, block_size));
    block_size = 0;
  }
  void code(uint32_t value, uint32_t size) {
    bits |= value << have;
    have += size;
    while (have >= 8) {
      put_byte(static_cast<uint8_t>(bits));
      bits >>= 8;
      have -= 8;
    }
  }
  void finish() {
    if (have > 0) put_byte(static_cast<uint8_t>(bits));
    flush_block();
    ok = ok && out->put8(0);
  }
};

bool lzw_encode(ByteBuffer* out, const uint8_t* indices, uint64_t count, allocators::IAllocator& allocator) {
  constexpr uint32_t kMinCode = 8, kClear = 256, kEoi = 257;
  LzwWriter w{out, {}, 0, 0, 0, true};
  // A dictionary of (prefix code, next byte) -> code, in a hash table of 8192 slots.
  struct Entry {
    int32_t prefix;
    int32_t byte;
    int32_t code;
  };
  ArrayView<Entry> table = allocate_view<Entry>(allocator, 8192);
  if (table.empty()) return false;
  auto reset = [&]() {
    for (Entry& e : table) e.code = -1;
  };
  reset();
  uint32_t next = kEoi + 1, size = kMinCode + 1;
  w.code(kClear, size);
  if (count > 0) {
    int32_t prefix = indices[0];
    for (uint64_t i = 1; i < count; ++i) {
      const int32_t byte = indices[i];
      uint32_t slot = (static_cast<uint32_t>(prefix) * 40503u ^ static_cast<uint32_t>(byte) * 2654435761u) & 8191;
      bool found = false;
      while (table[slot].code >= 0) {
        if (table[slot].prefix == prefix && table[slot].byte == byte) {
          found = true;
          break;
        }
        slot = (slot + 1) & 8191;
      }
      if (found) {
        prefix = table[slot].code;
        continue;
      }
      w.code(static_cast<uint32_t>(prefix), size);
      if (next < 4096) {
        table[slot] = {prefix, byte, static_cast<int32_t>(next)};
        if (next == (1u << size) && size < 12) ++size;
        ++next;
      } else {
        w.code(kClear, size);
        reset();
        next = kEoi + 1;
        size = kMinCode + 1;
      }
      prefix = byte;
    }
    w.code(static_cast<uint32_t>(prefix), size);
  }
  w.code(kEoi, size);
  w.finish();
  release_view(allocator, table);
  return w.ok;
}
}  // namespace

bool encode_gif(const Image& image, ByteBuffer* out) {
  if (image.empty() || image.width() > 65535 || image.height() > 65535) return false;
  allocators::IAllocator& allocator = image.allocator();
  const uint32_t w = image.width(), h = image.height();
  bool ok = out->put_tag("GIF8") && out->put8('9') && out->put8('a') && out->put16(w) && out->put16(h) && out->put8(0xF7) /* global table, 256 entries */ &&
            out->put8(0) && out->put8(0);
  for (uint32_t i = 0; i < 256 && ok; ++i) {
    uint32_t r = 0, g = 0, b = 0;
    if (i < kReds * kGreens * kBlues) {
      r = level_value(i / (kGreens * kBlues), kReds);
      g = level_value(i / kBlues % kGreens, kGreens);
      b = level_value(i % kBlues, kBlues);
    }
    ok = out->put8(static_cast<uint8_t>(r)) && out->put8(static_cast<uint8_t>(g)) && out->put8(static_cast<uint8_t>(b));
  }
  if (image.animated()) ok = ok && out->put8(0x21) && out->put8(0xFF) && out->put8(11) && out->put_tag("NETS") && out->put_tag("CAPE") && out->put8('2') && out->put8('.') && out->put8('0') &&
                              out->put8(3) && out->put8(1) && out->put16(image.loop_count()) && out->put8(0);
  ArrayView<uint8_t> indices = allocate_view<uint8_t>(allocator, uint64_t{w} * h);
  ArrayView<int32_t> errors = allocate_view<int32_t>(allocator, uint64_t{(w + 2)} * 2 * 3);  // two rows of error per channel
  if (indices.empty() || errors.empty()) {
    release_view(allocator, indices);
    release_view(allocator, errors);
    return false;
  }
  for (uint32_t f = 0; f < image.frame_count() && ok; ++f) {
    const ArrayView<uint32_t> pixels = image.frame(f);
    // Floyd-Steinberg dithering to the palette. Error rows: current and next, per channel, with a pixel of margin each side.
    errors.fill(0);
    const uint64_t row_len = uint64_t{w} + 2;
    bool any_transparent = false;
    for (uint32_t y = 0; y < h; ++y) {
      int32_t* cur = errors.data() + (y % 2) * 3 * row_len;
      int32_t* nxt = errors.data() + ((y + 1) % 2) * 3 * row_len;
      for (uint64_t k = 0; k < 3 * row_len; ++k) nxt[k] = 0;
      for (uint32_t x = 0; x < w; ++x) {
        const uint32_t p = pixels[uint64_t{y} * w + x];
        if ((p >> 24) < 128) {
          indices[uint64_t{y} * w + x] = static_cast<uint8_t>(kTransparentIndex);
          any_transparent = true;
          continue;
        }
        const int32_t channel[3] = {static_cast<int32_t>((p >> 16) & 0xFF), static_cast<int32_t>((p >> 8) & 0xFF), static_cast<int32_t>(p & 0xFF)};
        const uint32_t levels[3] = {kReds, kGreens, kBlues};
        uint32_t chosen[3];
        for (int c = 0; c < 3; ++c) {
          int32_t v = channel[c] + cur[c * row_len + x + 1] / 16;
          v = v < 0 ? 0 : v > 255 ? 255 : v;
          const uint32_t step = 255 / (levels[c] - 1);
          uint32_t level = (static_cast<uint32_t>(v) * (levels[c] - 1) + 127) / 255;
          if (level >= levels[c]) level = levels[c] - 1;
          chosen[c] = level;
          const int32_t error = v - static_cast<int32_t>(level * 255 / (levels[c] - 1));
          (void)step;
          cur[c * row_len + x + 2] += error * 7;
          nxt[c * row_len + x] += error * 3;
          nxt[c * row_len + x + 1] += error * 5;
          nxt[c * row_len + x + 2] += error * 1;
        }
        indices[uint64_t{y} * w + x] = static_cast<uint8_t>((chosen[0] * kGreens + chosen[1]) * kBlues + chosen[2]);
      }
    }
    // Graphic control: delay, transparency, and "do not dispose" (1) so each full frame replaces the last.
    const uint32_t cs = image.delay_ms(f) / 10;
    ok = ok && out->put8(0x21) && out->put8(0xF9) && out->put8(4) && out->put8(static_cast<uint8_t>((1 << 2) | (any_transparent ? 1 : 0))) &&
         out->put16(cs > 65535 ? 65535 : cs) && out->put8(static_cast<uint8_t>(kTransparentIndex)) && out->put8(0);
    ok = ok && out->put8(0x2C) && out->put16(0) && out->put16(0) && out->put16(w) && out->put16(h) && out->put8(0) && out->put8(8);
    ok = ok && lzw_encode(out, indices.data(), uint64_t{w} * h, allocator);
  }
  release_view(allocator, indices);
  release_view(allocator, errors);
  return ok && out->put8(0x3B);
}

}  // namespace media
