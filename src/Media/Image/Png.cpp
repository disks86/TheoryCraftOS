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
#include "Media/Zlib.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/Crc32.hpp"

namespace media {

namespace {
uint32_t be32(ConstByteView v, uint64_t at) { return (uint32_t{v[at]} << 24) | (uint32_t{v[at + 1]} << 16) | (uint32_t{v[at + 2]} << 8) | v[at + 3]; }
bool tag_is(ConstByteView v, uint64_t at, const char* tag) {
  for (int i = 0; i < 4; ++i)
    if (v[at + i] != static_cast<uint8_t>(tag[i])) return false;
  return true;
}

struct Header {
  uint32_t width = 0, height = 0;
  uint8_t depth = 0, color = 0, interlace = 0;
  uint32_t channels() const { return color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : 4; }
  uint32_t bits_per_pixel() const { return channels() * depth; }
  uint64_t row_bytes(uint32_t w) const { return (uint64_t{w} * bits_per_pixel() + 7) / 8; }
};

struct Palette {
  uint32_t colors[256];
  uint32_t count = 0;
};

// What tRNS says: alpha per palette entry, or the one colour (grey or RGB) that is transparent.
struct Transparency {
  uint8_t palette_alpha[256];
  uint32_t palette_count = 0;
  bool has_key = false;
  uint16_t key[3] = {0, 0, 0};
};

uint8_t paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
  return static_cast<uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

// Undoes the row filters in place: `data` holds `rows` rows each of a filter byte and `row_bytes` bytes.
bool unfilter(uint8_t* data, uint32_t rows, uint64_t row_bytes, uint32_t bpp) {
  const uint64_t stride = row_bytes + 1;
  for (uint32_t y = 0; y < rows; ++y) {
    uint8_t* row = data + y * stride + 1;
    const uint8_t* up = y ? data + (y - 1) * stride + 1 : nullptr;
    const uint8_t filter = data[y * stride];
    switch (filter) {
      case 0: break;
      case 1:
        for (uint64_t i = bpp; i < row_bytes; ++i) row[i] = static_cast<uint8_t>(row[i] + row[i - bpp]);
        break;
      case 2:
        if (up)
          for (uint64_t i = 0; i < row_bytes; ++i) row[i] = static_cast<uint8_t>(row[i] + up[i]);
        break;
      case 3:
        for (uint64_t i = 0; i < row_bytes; ++i) {
          const int left = i >= bpp ? row[i - bpp] : 0, above = up ? up[i] : 0;
          row[i] = static_cast<uint8_t>(row[i] + ((left + above) >> 1));
        }
        break;
      case 4:
        for (uint64_t i = 0; i < row_bytes; ++i) {
          const int left = i >= bpp ? row[i - bpp] : 0, above = up ? up[i] : 0, upper_left = (up && i >= bpp) ? up[i - bpp] : 0;
          row[i] = static_cast<uint8_t>(row[i] + paeth(left, above, upper_left));
        }
        break;
      default: return false;
    }
  }
  return true;
}

// The value of sample `index` in a row of samples of `depth` bits (1, 2, 4, 8 or 16).
uint32_t sample_at(const uint8_t* row, uint32_t index, uint32_t depth) {
  switch (depth) {
    case 8: return row[index];
    case 16: return (uint32_t{row[2 * index]} << 8) | row[2 * index + 1];
    default: {
      const uint32_t bit = index * depth;
      return (row[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
    }
  }
}

// One decoded row to pixels: `row` holds `count` pixels; they go to dest[0], dest[step], dest[2 * step] ...
void expand_row(const Header& h, const Palette& palette, const Transparency& trns, const uint8_t* row, uint32_t count, uint32_t* dest,
                uint32_t step, bool* has_alpha) {
  const uint32_t depth = h.depth;
  const uint32_t scale = depth < 8 ? 255 / ((1u << depth) - 1) : 1;
  for (uint32_t x = 0; x < count; ++x) {
    uint32_t a = 255, r, g, b;
    switch (h.color) {
      case 0: {
        const uint32_t v = sample_at(row, x, depth);
        r = g = b = depth == 16 ? v >> 8 : depth < 8 ? v * scale : v;
        if (trns.has_key && v == trns.key[0]) a = 0;
        break;
      }
      case 2: {
        const uint32_t vr = sample_at(row, 3 * x, depth), vg = sample_at(row, 3 * x + 1, depth), vb = sample_at(row, 3 * x + 2, depth);
        r = depth == 16 ? vr >> 8 : vr;
        g = depth == 16 ? vg >> 8 : vg;
        b = depth == 16 ? vb >> 8 : vb;
        if (trns.has_key && vr == trns.key[0] && vg == trns.key[1] && vb == trns.key[2]) a = 0;
        break;
      }
      case 3: {
        const uint32_t i = sample_at(row, x, depth);
        const uint32_t c = i < palette.count ? palette.colors[i] : 0xFF000000u;
        r = (c >> 16) & 0xFF;
        g = (c >> 8) & 0xFF;
        b = c & 0xFF;
        a = i < trns.palette_count ? trns.palette_alpha[i] : 255;
        break;
      }
      case 4: {
        const uint32_t v = sample_at(row, 2 * x, depth), va = sample_at(row, 2 * x + 1, depth);
        r = g = b = depth == 16 ? v >> 8 : v;
        a = depth == 16 ? va >> 8 : va;
        break;
      }
      default: {
        const uint32_t vr = sample_at(row, 4 * x, depth), vg = sample_at(row, 4 * x + 1, depth), vb = sample_at(row, 4 * x + 2, depth), va = sample_at(row, 4 * x + 3, depth);
        r = depth == 16 ? vr >> 8 : vr;
        g = depth == 16 ? vg >> 8 : vg;
        b = depth == 16 ? vb >> 8 : vb;
        a = depth == 16 ? va >> 8 : va;
        break;
      }
    }
    if (a != 255) *has_alpha = true;
    dest[x * step] = (a << 24) | (r << 16) | (g << 8) | b;
  }
}

// Decodes the filtered, compressed image data of one (sub)image of width x height into `pixels` (stride `pitch` pixels).
bool decode_image_data(const Header& h, uint32_t width, uint32_t height, const Palette& palette, const Transparency& trns,
                       ConstByteView compressed, allocators::IAllocator& allocator, uint32_t* pixels, uint32_t pitch, bool* has_alpha) {
  static const uint8_t kStartX[7] = {0, 4, 0, 2, 0, 1, 0}, kStartY[7] = {0, 0, 4, 0, 2, 0, 1};
  static const uint8_t kStepX[7] = {8, 8, 4, 4, 2, 2, 1}, kStepY[7] = {8, 8, 8, 4, 4, 2, 2};
  Header sub = h;
  // How much data to expect: bounds the inflater.
  uint64_t expected = 0;
  if (!h.interlace) {
    expected = (h.row_bytes(width) + 1) * height;
  } else {
    for (int p = 0; p < 7; ++p) {
      const uint32_t pw = (width + kStepX[p] - 1 - kStartX[p]) / kStepX[p], ph = (height + kStepY[p] - 1 - kStartY[p]) / kStepY[p];
      if (width > kStartX[p] && height > kStartY[p]) expected += (h.row_bytes(pw) + 1) * ph;
    }
  }
  ByteBuffer raw(allocator);
  if (!inflate(compressed, &raw, expected + 16) || raw.size() < expected) return false;
  uint8_t* data = raw.writable_view().data();
  const uint32_t bpp = h.bits_per_pixel() / 8 ? h.bits_per_pixel() / 8 : 1;
  if (!h.interlace) {
    if (!unfilter(data, height, h.row_bytes(width), bpp)) return false;
    for (uint32_t y = 0; y < height; ++y)
      expand_row(sub, palette, trns, data + y * (h.row_bytes(width) + 1) + 1, width, pixels + uint64_t{y} * pitch, 1, has_alpha);
    return true;
  }
  uint64_t offset = 0;
  for (int p = 0; p < 7; ++p) {
    if (width <= kStartX[p] || height <= kStartY[p]) continue;
    const uint32_t pw = (width + kStepX[p] - 1 - kStartX[p]) / kStepX[p], ph = (height + kStepY[p] - 1 - kStartY[p]) / kStepY[p];
    const uint64_t row_bytes = h.row_bytes(pw);
    if (!unfilter(data + offset, ph, row_bytes, bpp)) return false;
    for (uint32_t y = 0; y < ph; ++y)
      expand_row(sub, palette, trns, data + offset + y * (row_bytes + 1) + 1, pw,
                 pixels + uint64_t{kStartY[p] + y * kStepY[p]} * pitch + kStartX[p], kStepX[p], has_alpha);
    offset += (row_bytes + 1) * ph;
  }
  return true;
}

uint32_t blend_over(uint32_t src, uint32_t dst) {  // straight alpha, 0xAARRGGBB
  const uint32_t sa = src >> 24;
  if (sa == 255) return src;
  if (sa == 0) return dst;
  const uint32_t da = dst >> 24;
  const uint32_t out_a = sa * 255 + da * (255 - sa);  // scaled by 255
  if (out_a == 0) return 0;
  auto channel = [&](int shift) {
    const uint32_t s = (src >> shift) & 0xFF, d = (dst >> shift) & 0xFF;
    return (s * sa * 255 + d * da * (255 - sa)) / out_a;
  };
  return ((out_a / 255) << 24) | (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

// A frame of an animated PNG: where it goes and how, and where its compressed data is (a run of the data chunks).
struct FrameRecord {
  uint32_t width, height, x, y, delay_num, delay_den;
  uint8_t dispose, blend;
  uint32_t first_span, span_count;
};
struct Span {
  uint64_t offset;
  uint32_t length;
};

// Joins the data chunks of one frame into one run of zlib data.
bool gather(ConstByteView file, const List<Span>& spans, uint32_t first, uint32_t count, ByteBuffer* out) {
  for (uint32_t i = 0; i < count; ++i) {
    const Span& s = spans[first + i];
    if (!out->append(file.subview(s.offset, s.length))) return false;
  }
  return true;
}
}  // namespace

bool decode_png(ConstByteView data, Image* out) {
  out->clear();
  if (data.size() < 8 + 12 || detect_image_format(data) != ImageFormat::Png) return false;
  allocators::IAllocator& allocator = out->allocator();
  Header h;
  Palette palette;
  Transparency trns;
  bool have_header = false, saw_data = false, animated = false, ended = false;
  uint32_t declared_frames = 0, plays = 0;
  List<Span> spans(allocator);
  List<FrameRecord> frames(allocator);
  FrameRecord default_image{};
  bool default_has_data = false, current_is_default = true;
  uint64_t pos = 8;
  while (pos + 12 <= data.size() && !ended) {
    const uint32_t length = be32(data, pos);
    if (length > 0x7FFFFFFF || pos + 12 + uint64_t{length} > data.size()) return false;
    const uint64_t body = pos + 8;
    if (crc32(data.subview(pos + 4, 4 + uint64_t{length})) != be32(data, body + length)) return false;
    if (tag_is(data, pos + 4, "IHDR")) {
      if (length != 13 || have_header) return false;
      h.width = be32(data, body);
      h.height = be32(data, body + 4);
      h.depth = data[body + 8];
      h.color = data[body + 9];
      h.interlace = data[body + 12];
      if (data[body + 10] != 0 || data[body + 11] != 0 || h.interlace > 1 || h.width == 0 || h.height == 0) return false;
      const bool ok = (h.color == 0 && (h.depth == 1 || h.depth == 2 || h.depth == 4 || h.depth == 8 || h.depth == 16)) ||
                      ((h.color == 2 || h.color == 4 || h.color == 6) && (h.depth == 8 || h.depth == 16)) ||
                      (h.color == 3 && (h.depth == 1 || h.depth == 2 || h.depth == 4 || h.depth == 8));
      if (!ok || uint64_t{h.width} * h.height > Image::kMaxPixels) return false;
      have_header = true;
    } else if (!have_header) {
      return false;
    } else if (tag_is(data, pos + 4, "PLTE")) {
      if (length % 3 != 0 || length > 768 || length == 0) return false;
      palette.count = length / 3;
      for (uint32_t i = 0; i < palette.count; ++i)
        palette.colors[i] = 0xFF000000u | (uint32_t{data[body + 3 * i]} << 16) | (uint32_t{data[body + 3 * i + 1]} << 8) | data[body + 3 * i + 2];
    } else if (tag_is(data, pos + 4, "tRNS")) {
      if (h.color == 3) {
        trns.palette_count = length < 256 ? length : 256;
        for (uint32_t i = 0; i < trns.palette_count; ++i) trns.palette_alpha[i] = data[body + i];
      } else if (h.color == 0 && length >= 2) {
        trns.has_key = true;
        trns.key[0] = static_cast<uint16_t>((data[body] << 8) | data[body + 1]);
        if (h.depth < 16) trns.key[0] &= (1u << h.depth) - 1;
      } else if (h.color == 2 && length >= 6) {
        trns.has_key = true;
        for (int i = 0; i < 3; ++i) trns.key[i] = static_cast<uint16_t>((data[body + 2 * i] << 8) | data[body + 2 * i + 1]);
        if (h.depth < 16)
          for (int i = 0; i < 3; ++i) trns.key[i] &= (1u << h.depth) - 1;
      }
    } else if (tag_is(data, pos + 4, "acTL")) {
      if (length != 8 || saw_data) return false;
      declared_frames = be32(data, body);
      plays = be32(data, body + 4);
      animated = declared_frames > 0 && declared_frames <= 4096;
    } else if (tag_is(data, pos + 4, "fcTL")) {
      if (length != 26) return false;
      FrameRecord f{};
      f.width = be32(data, body + 4);
      f.height = be32(data, body + 8);
      f.x = be32(data, body + 12);
      f.y = be32(data, body + 16);
      f.delay_num = (data[body + 20] << 8) | data[body + 21];
      f.delay_den = (data[body + 22] << 8) | data[body + 23];
      f.dispose = data[body + 24];
      f.blend = data[body + 25];
      if (f.width == 0 || f.height == 0 || uint64_t{f.x} + f.width > h.width || uint64_t{f.y} + f.height > h.height || f.dispose > 2 || f.blend > 1) return false;
      f.first_span = static_cast<uint32_t>(spans.count());
      if (!animated) {
        current_is_default = true;  // frame control without an animation header: ignore
      } else if (!frames.push_back(f)) {
        return false;
      } else {
        current_is_default = false;
      }
    } else if (tag_is(data, pos + 4, "IDAT")) {
      saw_data = true;
      if (!spans.push_back({body, length})) return false;
      if (current_is_default) {
        if (!default_has_data) default_image.first_span = static_cast<uint32_t>(spans.count()) - 1;
        ++default_image.span_count;
        default_has_data = true;
      } else if (frames.count() > 0) {
        ++frames[frames.count() - 1].span_count;
      }
    } else if (tag_is(data, pos + 4, "fdAT")) {
      if (length < 4 || frames.count() == 0 || current_is_default) return false;
      if (!spans.push_back({body + 4, length - 4})) return false;
      ++frames[frames.count() - 1].span_count;
    } else if (tag_is(data, pos + 4, "IEND")) {
      ended = true;
    } else if ((data[pos + 4] & 0x20) == 0) {
      return false;  // an unknown critical chunk
    }
    pos = body + length + 4;
  }
  if (!have_header || (!default_has_data && frames.count() == 0)) return false;
  if (h.color == 3 && palette.count == 0) return false;

  bool has_alpha = false;
  if (!animated || frames.count() == 0) {
    if (!default_has_data) return false;
    ByteBuffer z(allocator);
    if (!gather(data, spans, default_image.first_span, default_image.span_count, &z)) return false;
    if (!out->create(h.width, h.height, 1)) return false;
    if (!decode_image_data(h, h.width, h.height, palette, trns, z.view(), allocator, out->frame(0).data(), h.width, &has_alpha)) {
      out->clear();
      return false;
    }
    out->set_has_alpha(has_alpha);
    return true;
  }

  // An animation: draw each frame onto a canvas and keep a snapshot of the canvas after each.
  if (!out->create(h.width, h.height, static_cast<uint32_t>(frames.count()))) return false;
  const uint64_t canvas_pixels = uint64_t{h.width} * h.height;
  ArrayView<uint32_t> saved = allocate_view<uint32_t>(allocator, canvas_pixels);
  if (saved.empty()) {
    out->clear();
    return false;
  }
  ArrayView<uint32_t> canvas = ArrayView<uint32_t>();
  bool ok = true;
  for (uint32_t fi = 0; fi < frames.count() && ok; ++fi) {
    const FrameRecord& f = frames[fi];
    ByteBuffer z(allocator);
    ArrayView<uint32_t> tile = allocate_view<uint32_t>(allocator, uint64_t{f.width} * f.height);
    ok = !tile.empty() && gather(data, spans, f.first_span, f.span_count, &z);
    if (ok) {
      tile.fill(0);
      bool frame_alpha = false;
      ok = decode_image_data(h, f.width, f.height, palette, trns, z.view(), allocator, tile.data(), f.width, &frame_alpha);
      has_alpha = has_alpha || frame_alpha;
    }
    if (ok) {
      canvas = out->frame(fi);
      // (the canvas already holds what the previous frame left: see the end of the loop body)
      const uint8_t dispose = (fi == 0 && f.dispose == 2) ? 1 : f.dispose;
      if (dispose == 2) saved.copy_from(canvas);
      for (uint32_t y = 0; y < f.height; ++y)
        for (uint32_t x = 0; x < f.width; ++x) {
          uint32_t& dst = canvas[uint64_t{f.y + y} * h.width + f.x + x];
          const uint32_t src = tile[uint64_t{y} * f.width + x];
          dst = f.blend == 0 ? src : blend_over(src, dst);
        }
      // The snapshot for this frame is the canvas as it is now; the disposal applies to the next frame's starting point.
      ArrayView<uint32_t> next_start = fi + 1 < frames.count() ? out->frame(fi + 1) : ArrayView<uint32_t>();
      if (!next_start.empty()) {
        next_start.copy_from(canvas);
        if (dispose == 1)
          for (uint32_t y = 0; y < f.height; ++y)
            for (uint32_t x = 0; x < f.width; ++x) next_start[uint64_t{f.y + y} * h.width + f.x + x] = 0;
        else if (dispose == 2) next_start.copy_from(saved);
      }
      out->set_delay_ms(fi, f.delay_den == 0 ? f.delay_num * 10 : static_cast<uint32_t>(uint64_t{f.delay_num} * 1000 / f.delay_den));
    }
    release_view(allocator, tile);
  }
  release_view(allocator, saved);
  if (!ok) {
    out->clear();
    return false;
  }
  out->set_loop_count(plays);
  out->set_has_alpha(has_alpha);
  return true;
}

// ---- Encoding ----------------------------------------------------------------------------

namespace {
bool put_chunk(ByteBuffer* out, const char* tag, ConstByteView body) {
  const uint8_t t[4] = {static_cast<uint8_t>(tag[0]), static_cast<uint8_t>(tag[1]), static_cast<uint8_t>(tag[2]), static_cast<uint8_t>(tag[3])};
  const uint32_t crc = crc32(body, crc32(ConstByteView(t, 4)));
  const uint32_t n = static_cast<uint32_t>(body.size());
  const uint8_t length[4] = {static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
  const uint8_t check[4] = {static_cast<uint8_t>(crc >> 24), static_cast<uint8_t>(crc >> 16), static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc)};
  return out->append(ConstByteView(length, 4)) && out->append(ConstByteView(t, 4)) && out->append(body) && out->append(ConstByteView(check, 4));
}

void put_be32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

// One frame as filtered scanlines (a filter byte, then the row), ready to deflate. With `adaptive` each row uses
// whichever of the five filters leaves the smallest sum of magnitudes.
bool filter_frame(ArrayView<uint32_t> pixels, uint32_t w, uint32_t h, bool alpha, bool adaptive, allocators::IAllocator& allocator, ByteBuffer* out) {
  const uint32_t bpp = alpha ? 4 : 3;
  const uint64_t row = uint64_t{w} * bpp;
  ArrayView<uint8_t> scratch = allocate_view<uint8_t>(allocator, row * 7);
  if (scratch.empty()) return false;
  uint8_t* previous = scratch.data();        // the row above, unfiltered
  uint8_t* current = scratch.data() + row;   // this row, unfiltered
  uint8_t* candidate = scratch.data() + 2 * row;  // five filtered candidates follow
  __builtin_memset(previous, 0, row);
  bool ok = true;
  for (uint32_t y = 0; y < h && ok; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t p = pixels[uint64_t{y} * w + x];
      current[x * bpp] = static_cast<uint8_t>(p >> 16);
      current[x * bpp + 1] = static_cast<uint8_t>(p >> 8);
      current[x * bpp + 2] = static_cast<uint8_t>(p);
      if (alpha) current[x * bpp + 3] = static_cast<uint8_t>(p >> 24);
    }
    uint32_t best = 0;
    uint64_t best_cost = ~uint64_t{0};
    const uint32_t kinds = adaptive ? 5 : 1;
    for (uint32_t f = 0; f < kinds; ++f) {
      uint8_t* dest = candidate + f * row;
      uint64_t cost = 0;
      for (uint64_t i = 0; i < row; ++i) {
        const int left = i >= bpp ? current[i - bpp] : 0, up = previous[i], upper_left = i >= bpp ? previous[i - bpp] : 0;
        int predictor = 0;
        switch (f) {
          case 1: predictor = left; break;
          case 2: predictor = up; break;
          case 3: predictor = (left + up) >> 1; break;
          case 4: predictor = paeth(left, up, upper_left); break;
          default: break;
        }
        const uint8_t v = static_cast<uint8_t>(current[i] - predictor);
        dest[i] = v;
        cost += v < 128 ? v : 256 - v;
      }
      if (cost < best_cost) {
        best_cost = cost;
        best = f;
      }
    }
    ok = out->put8(static_cast<uint8_t>(best)) && out->append(ConstByteView(candidate + best * row, row));
    uint8_t* swap = previous;
    previous = current;
    current = swap;
  }
  release_view(allocator, scratch);
  return ok;
}
}  // namespace

bool encode_png(const Image& image, uint32_t level, ByteBuffer* out) {
  if (image.empty()) return false;
  allocators::IAllocator& allocator = image.allocator();
  const bool alpha = image.has_alpha();
  const bool animated = image.animated();
  static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (level > 9) level = 9;
  uint8_t chunk[26];
  put_be32(chunk, image.width());
  put_be32(chunk + 4, image.height());
  chunk[8] = 8;
  chunk[9] = alpha ? 6 : 2;
  chunk[10] = chunk[11] = chunk[12] = 0;
  if (!out->append(ConstByteView(kSignature, 8)) || !put_chunk(out, "IHDR", ConstByteView(chunk, 13))) return false;
  uint32_t sequence = 0;
  if (animated) {
    put_be32(chunk, image.frame_count());
    put_be32(chunk + 4, image.loop_count());
    if (!put_chunk(out, "acTL", ConstByteView(chunk, 8))) return false;
  }
  for (uint32_t f = 0; f < image.frame_count(); ++f) {
    if (animated) {  // frame control: the whole canvas, no disposal, replace
      put_be32(chunk, sequence++);
      put_be32(chunk + 4, image.width());
      put_be32(chunk + 8, image.height());
      put_be32(chunk + 12, 0);
      put_be32(chunk + 16, 0);
      const uint32_t ms = image.delay_ms(f) > 65535 ? 65535 : image.delay_ms(f);
      chunk[20] = static_cast<uint8_t>(ms >> 8);
      chunk[21] = static_cast<uint8_t>(ms);
      chunk[22] = static_cast<uint8_t>(1000 >> 8);
      chunk[23] = static_cast<uint8_t>(1000);
      chunk[24] = 0;
      chunk[25] = 0;
      if (!put_chunk(out, "fcTL", ConstByteView(chunk, 26))) return false;
    }
    ByteBuffer raw(allocator), z(allocator);
    if (!filter_frame(image.frame(f), image.width(), image.height(), alpha, level >= 2, allocator, &raw) || !deflate(raw.view(), &z, level == 0 ? 0 : level, allocator))
      return false;
    // The compressed data in chunks of at most 64 KiB (the first frame's are IDAT, the others' fdAT with a sequence number).
    uint64_t at = 0;
    const uint64_t piece = 65536;
    do {
      const uint64_t n = z.size() - at < piece ? z.size() - at : piece;
      if (f == 0) {
        if (!put_chunk(out, "IDAT", z.view().subview(at, n))) return false;
      } else {
        ByteBuffer body(allocator);
        uint8_t seq[4];
        put_be32(seq, sequence++);
        if (!body.append(ConstByteView(seq, 4)) || !body.append(z.view().subview(at, n)) || !put_chunk(out, "fdAT", body.view())) return false;
      }
      at += n;
    } while (at < z.size());
  }
  return put_chunk(out, "IEND", ConstByteView());
}

}  // namespace media
