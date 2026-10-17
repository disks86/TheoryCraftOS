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

// TIFF: baseline 6.0 plus the common extensions - strips and tiles, 1/2/4/8/16-bit samples, grey, RGB, palette, alpha,
// chunky and planar layouts, no compression / PackBits / LZW / Deflate, horizontal predictor. Orientation tags other
// than "top left" are not applied. Only the first image of a multi-page file is read.
#include "Media/ImageCodecs.hpp"
#include "Media/Zlib.hpp"

namespace media {

namespace {
struct Reader {
  ConstByteView d;
  bool big;
  uint32_t u16(uint64_t at) const { return big ? (uint32_t{d[at]} << 8 | d[at + 1]) : (uint32_t{d[at + 1]} << 8 | d[at]); }
  uint32_t u32(uint64_t at) const { return big ? (u16(at) << 16 | u16(at + 2)) : (u16(at + 2) << 16 | u16(at)); }
};

struct Tag {
  bool present = false;
  uint32_t type = 0, count = 0;
  uint64_t data = 0;  // where the values are
};

uint32_t type_size(uint32_t type) { return type == 1 || type == 2 || type == 6 || type == 7 ? 1 : type == 3 || type == 8 ? 2 : type == 4 || type == 9 || type == 11 ? 4 : 0; }

// The index'th value of a tag (BYTE, SHORT or LONG).
bool value(const Reader& r, const Tag& t, uint32_t index, uint32_t* out) {
  if (!t.present || index >= t.count) return false;
  const uint32_t size = type_size(t.type);
  if (size == 1) *out = r.d[t.data + index];
  else if (size == 2) *out = r.u16(t.data + uint64_t{index} * 2);
  else if (size == 4) *out = r.u32(t.data + uint64_t{index} * 4);
  else return false;
  return true;
}

// TIFF's LZW: codes of 9 to 12 bits, most significant bit first, "early change" (the width grows one code early).
bool lzw_decode(ConstByteView in, ByteBuffer* out, uint64_t limit, allocators::IAllocator& allocator) {
  struct Entry { uint16_t prefix; uint8_t byte; uint8_t first; uint16_t length; };
  Entry* table = static_cast<Entry*>(allocator.allocate(sizeof(Entry) * 4096));
  if (!table) return false;
  for (uint32_t i = 0; i < 256; ++i) table[i] = {0, static_cast<uint8_t>(i), static_cast<uint8_t>(i), 1};
  uint32_t next = 258, width = 9, previous = 0xFFFFFFFF;
  uint64_t bits = 0, nbits = 0, at = 0, produced = 0;
  bool ok = true;
  uint8_t chain[4096];
  while (true) {
    while (nbits < width && at < in.size()) { bits = (bits << 8) | in[at++]; nbits += 8; }
    if (nbits < width) break;  // ran out without an end code: take what there is
    const uint32_t code = static_cast<uint32_t>((bits >> (nbits - width)) & ((1u << width) - 1));
    nbits -= width;
    if (code == 257) break;
    if (code == 256) {
      next = 258;
      width = 9;
      previous = 0xFFFFFFFF;
      continue;
    }
    uint32_t length;
    if (code < next && (code < 256 || code >= 258)) {
      length = table[code].length;
      uint32_t c = code;
      for (uint32_t i = length; i > 0; --i) { chain[i - 1] = table[c].byte; c = table[c].prefix; }
    } else if (code == next && previous != 0xFFFFFFFF) {
      length = table[previous].length + 1;
      uint32_t c = previous;
      for (uint32_t i = length - 1; i > 0; --i) { chain[i - 1] = table[c].byte; c = table[c].prefix; }
      chain[length - 1] = chain[0];
    } else {
      ok = false;
      break;
    }
    if (produced + length > limit) { ok = false; break; }
    if (!out->append(ConstByteView(chain, length))) { ok = false; break; }
    produced += length;
    if (previous != 0xFFFFFFFF && next < 4096) {
      table[next] = {static_cast<uint16_t>(previous), chain[0], table[previous].first, static_cast<uint16_t>(table[previous].length + 1)};
      ++next;
    }
    previous = code;
    if (next + 1 >= (1u << width) && width < 12) ++width;
  }
  allocator.deallocate(table);
  return ok;
}

bool packbits_decode(ConstByteView in, ByteBuffer* out, uint64_t limit) {
  uint64_t at = 0, produced = 0;
  while (at < in.size() && produced < limit) {
    const int8_t n = static_cast<int8_t>(in[at++]);
    if (n >= 0) {
      const uint64_t count = uint64_t{static_cast<uint8_t>(n)} + 1;
      if (at + count > in.size() || produced + count > limit || !out->append(in.subview(at, count))) return false;
      at += count;
      produced += count;
    } else if (n != -128) {
      const uint64_t count = uint64_t{1} + static_cast<uint8_t>(-n);
      if (at >= in.size() || produced + count > limit) return false;
      for (uint64_t i = 0; i < count; ++i)
        if (!out->put8(in[at])) return false;
      ++at;
      produced += count;
    }
  }
  return true;
}
}  // namespace

bool decode_tiff(ConstByteView data, Image* out) {
  out->clear();
  if (data.size() < 16 || detect_image_format(data) != ImageFormat::Tiff) return false;
  Reader r{data, data[0] == 'M'};
  const uint64_t ifd = r.u32(4);
  if (ifd + 2 > data.size()) return false;
  const uint32_t entries = r.u16(ifd);
  if (ifd + 2 + uint64_t{entries} * 12 > data.size()) return false;
  enum { Width, Height, Bits, Compression, Photometric, StripOffsets, Spp, RowsPerStrip, StripCounts, Planar, Predictor, ColorMap, ExtraSamples, TileW, TileL,
         TileOffsets, TileCounts, SampleFormat, kTagCount };
  static const uint32_t kIds[kTagCount] = {256, 257, 258, 259, 262, 273, 277, 278, 279, 284, 317, 320, 338, 322, 323, 324, 325, 339};
  Tag t[kTagCount];
  for (uint32_t i = 0; i < entries; ++i) {
    const uint64_t e = ifd + 2 + uint64_t{i} * 12;
    const uint32_t id = r.u16(e), type = r.u16(e + 2), count = r.u32(e + 4);
    for (uint32_t k = 0; k < kTagCount; ++k) {
      if (kIds[k] != id) continue;
      const uint64_t bytes = uint64_t{type_size(type)} * count;
      if (type_size(type) == 0 || count == 0) break;
      const uint64_t where = bytes <= 4 ? e + 8 : r.u32(e + 8);
      if (where + bytes > data.size()) return false;
      t[k] = {true, type, count, where};
    }
  }
  uint32_t width = 0, height = 0, compression = 1, photometric = 2, spp = 1, planar = 1, predictor = 1, rows_per_strip = 0xFFFFFFFF, sample_format = 1;
  if (!value(r, t[Width], 0, &width) || !value(r, t[Height], 0, &height) || width == 0 || height == 0) return false;
  value(r, t[Compression], 0, &compression);
  value(r, t[Photometric], 0, &photometric);
  value(r, t[Spp], 0, &spp);
  value(r, t[Planar], 0, &planar);
  value(r, t[Predictor], 0, &predictor);
  value(r, t[RowsPerStrip], 0, &rows_per_strip);
  value(r, t[SampleFormat], 0, &sample_format);
  uint32_t bits = 1;
  value(r, t[Bits], 0, &bits);
  for (uint32_t s = 1; s < spp && s < t[Bits].count; ++s) {
    uint32_t b = 0;
    value(r, t[Bits], s, &b);
    if (b != bits) return false;  // mixed depths
  }
  if (spp < 1 || spp > 5 || (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16) || sample_format != 1 || (planar != 1 && planar != 2)) return false;
  if (compression != 1 && compression != 5 && compression != 8 && compression != 32946 && compression != 32773) return false;
  if (predictor != 1 && (predictor != 2 || (bits != 8 && bits != 16))) return false;
  const bool palette = photometric == 3;
  if (photometric > 3 || (palette && (!t[ColorMap].present || t[ColorMap].count < (3u << bits) || bits > 8))) return false;
  const uint32_t base = photometric == 2 ? 3 : 1;
  if (spp < base) return false;
  uint32_t extra_kind = 0;
  value(r, t[ExtraSamples], 0, &extra_kind);
  const bool alpha = spp > base && (t[ExtraSamples].present ? (extra_kind == 1 || extra_kind == 2) : false);
  if (uint64_t{width} * height > Image::kMaxPixels) return false;
  const uint64_t sample_count = uint64_t{width} * height * spp;

  // Chunks: tiles if the file has them, else strips.
  const bool tiled = t[TileW].present;
  uint32_t chunk_w = width, chunk_h, across = 1;
  const Tag* offsets = &t[tiled ? TileOffsets : StripOffsets];
  const Tag* counts = &t[tiled ? TileCounts : StripCounts];
  if (tiled) {
    if (!value(r, t[TileW], 0, &chunk_w) || !value(r, t[TileL], 0, &chunk_h) || chunk_w == 0 || chunk_h == 0 || chunk_w > 65536 || chunk_h > 65536) return false;
    across = (width + chunk_w - 1) / chunk_w;
  } else {
    chunk_h = rows_per_strip == 0 || rows_per_strip > height ? height : rows_per_strip;
  }
  const uint32_t down = (height + chunk_h - 1) / chunk_h;
  const uint32_t per_plane = across * down;
  const uint32_t chunk_samples = planar == 1 ? spp : 1;
  if (!offsets->present || !counts->present || offsets->count < per_plane * (planar == 1 ? 1 : spp) || counts->count < offsets->count) return false;

  allocators::IAllocator& allocator = out->allocator();
  ArrayView<uint16_t> samples = allocate_view<uint16_t>(allocator, sample_count);
  if (samples.empty()) return false;
  samples.fill(0);
  const uint64_t row_bytes = (uint64_t{chunk_w} * chunk_samples * bits + 7) / 8;
  const uint64_t chunk_bytes = row_bytes * chunk_h;
  bool ok = true;
  ByteBuffer raw(allocator);
  for (uint32_t plane = 0; ok && plane < (planar == 1 ? 1u : spp); ++plane)
    for (uint32_t c = 0; ok && c < per_plane; ++c) {
      uint32_t offset = 0, count = 0;
      const uint32_t index = plane * per_plane + c;
      if (!value(r, *offsets, index, &offset) || !value(r, *counts, index, &count) || uint64_t{offset} + count > data.size()) { ok = false; break; }
      const ConstByteView packed = data.subview(offset, count);
      raw.clear();
      if (compression == 1) ok = raw.append(packed.first(packed.size() < chunk_bytes ? packed.size() : chunk_bytes));
      else if (compression == 5) ok = lzw_decode(packed, &raw, chunk_bytes, allocator);
      else if (compression == 32773) ok = packbits_decode(packed, &raw, chunk_bytes);
      else ok = inflate(packed, &raw, chunk_bytes);
      if (!ok) break;
      // Missing data (a short last strip, or a truncated chunk) reads as zero.
      if (raw.size() < chunk_bytes && !raw.put_zeros(chunk_bytes - raw.size())) { ok = false; break; }
      uint8_t* bytes = const_cast<uint8_t*>(raw.view().data());
      const uint32_t cx = tiled ? (c % across) * chunk_w : 0, cy = (tiled ? c / across : c) * chunk_h;
      const uint32_t rows = cy + chunk_h > height ? height - cy : chunk_h;
      const uint32_t cols = cx + chunk_w > width ? width - cx : chunk_w;
      for (uint32_t y = 0; y < rows; ++y) {
        uint8_t* row = bytes + y * row_bytes;
        if (predictor == 2) {  // each sample is stored as the difference from the one before it in the row
          if (bits == 8) {
            for (uint64_t i = chunk_samples; i < uint64_t{chunk_w} * chunk_samples; ++i) row[i] = static_cast<uint8_t>(row[i] + row[i - chunk_samples]);
          } else {
            for (uint64_t i = chunk_samples; i < uint64_t{chunk_w} * chunk_samples; ++i) {
              const uint32_t a = r.big ? (row[i * 2] << 8 | row[i * 2 + 1]) : (row[i * 2 + 1] << 8 | row[i * 2]);
              const uint32_t b = r.big ? (row[(i - chunk_samples) * 2] << 8 | row[(i - chunk_samples) * 2 + 1]) : (row[(i - chunk_samples) * 2 + 1] << 8 | row[(i - chunk_samples) * 2]);
              const uint32_t s = (a + b) & 0xFFFF;
              if (r.big) { row[i * 2] = static_cast<uint8_t>(s >> 8); row[i * 2 + 1] = static_cast<uint8_t>(s); }
              else { row[i * 2] = static_cast<uint8_t>(s); row[i * 2 + 1] = static_cast<uint8_t>(s >> 8); }
            }
          }
        }
        for (uint32_t x = 0; x < cols; ++x)
          for (uint32_t s = 0; s < chunk_samples; ++s) {
            const uint64_t n = uint64_t{x} * chunk_samples + s;
            uint32_t v;
            if (bits == 8) v = row[n];
            else if (bits == 16) v = r.big ? (row[n * 2] << 8 | row[n * 2 + 1]) : (row[n * 2 + 1] << 8 | row[n * 2]);
            else v = (row[n * bits / 8] >> (8 - bits - (n * bits) % 8)) & ((1u << bits) - 1);  // packed most significant bit first
            const uint64_t pixel = uint64_t{cy + y} * width + cx + x;
            samples[pixel * spp + (planar == 1 ? s : plane)] = static_cast<uint16_t>(v);
          }
      }
    }
  if (ok) ok = out->create(width, height, 1);
  if (ok) {
    const uint32_t max = (1u << bits) - 1;
    auto to8 = [&](uint32_t v) { return bits == 8 ? v : bits == 16 ? v >> 8 : (v * 255 + max / 2) / max; };
    ArrayView<uint32_t> px = out->frame(0);
    bool any_alpha = false;
    for (uint64_t i = 0; i < px.size(); ++i) {
      const uint16_t* s = &samples[i * spp];
      uint32_t red, green, blue, a = 255;
      if (palette) {
        const uint32_t n = s[0];
        red = r.u16(t[ColorMap].data + uint64_t{n} * 2) >> 8;
        green = r.u16(t[ColorMap].data + (uint64_t{1} << bits) * 2 + uint64_t{n} * 2) >> 8;
        blue = r.u16(t[ColorMap].data + (uint64_t{2} << bits) * 2 + uint64_t{n} * 2) >> 8;
      } else if (photometric == 2) {
        red = to8(s[0]); green = to8(s[1]); blue = to8(s[2]);
      } else {
        red = green = blue = to8(photometric == 0 ? max - s[0] : s[0]);
      }
      if (alpha) {
        a = to8(s[base]);
        if (extra_kind == 1 && a > 0 && a < 255) {  // associated (premultiplied) alpha
          red = red * 255 / a < 255 ? red * 255 / a : 255;
          green = green * 255 / a < 255 ? green * 255 / a : 255;
          blue = blue * 255 / a < 255 ? blue * 255 / a : 255;
        }
        any_alpha = any_alpha || a != 255;
      }
      px[i] = a << 24 | red << 16 | green << 8 | blue;
    }
    out->set_has_alpha(any_alpha);
  }
  release_view(allocator, samples);
  if (!ok) out->clear();
  return ok;
}

namespace {
struct Entry {
  uint16_t tag, type;
  uint32_t count, value;
};
}  // namespace

// Little-endian TIFF, 8 bits per channel (RGB, or RGBA with unassociated alpha), Deflate with the horizontal predictor,
// strips of about 64 KiB of raw data.
bool encode_tiff(const Image& image, ByteBuffer* out) {
  if (image.empty()) return false;
  allocators::IAllocator& allocator = image.allocator();
  const uint64_t start = out->size();
  const uint32_t width = image.width(), height = image.height();
  const uint32_t channels = image.has_alpha() ? 4 : 3;
  const uint64_t row_bytes = uint64_t{width} * channels;
  uint32_t rows_per_strip = static_cast<uint32_t>(65536 / row_bytes);
  if (rows_per_strip < 1) rows_per_strip = 1;
  if (rows_per_strip > height) rows_per_strip = height;
  const uint32_t strips = (height + rows_per_strip - 1) / rows_per_strip;
  if (uint64_t{width} * height * channels > 0xF0000000ull) return false;

  // Layout: header, strips, then the IFD (with its out-of-line values after it).
  bool ok = out->put8('I') && out->put8('I') && out->put16(42) && out->put32(0);  // the IFD offset is patched in
  uint32_t* strip_offsets = static_cast<uint32_t*>(allocator.allocate(sizeof(uint32_t) * strips * 2));
  if (!strip_offsets) return false;
  uint32_t* strip_sizes = strip_offsets + strips;
  ByteBuffer raw(allocator), packed(allocator);
  for (uint32_t s = 0; ok && s < strips; ++s) {
    const uint32_t first = s * rows_per_strip, rows = first + rows_per_strip > height ? height - first : rows_per_strip;
    raw.clear();
    packed.clear();
    for (uint32_t y = 0; ok && y < rows; ++y) {
      ArrayView<uint32_t> row = image.frame(0).subview(uint64_t{first + y} * width, width);
      uint8_t prev[4] = {0, 0, 0, 0};
      for (uint32_t x = 0; ok && x < width; ++x) {
        const uint32_t p = row[x];
        const uint8_t cur[4] = {static_cast<uint8_t>(p >> 16), static_cast<uint8_t>(p >> 8), static_cast<uint8_t>(p), static_cast<uint8_t>(p >> 24)};
        for (uint32_t c = 0; ok && c < channels; ++c) ok = raw.put8(static_cast<uint8_t>(cur[c] - prev[c]));
        for (uint32_t c = 0; c < 4; ++c) prev[c] = cur[c];
      }
    }
    ok = ok && deflate(raw.view(), &packed, 6, allocator);
    strip_offsets[s] = static_cast<uint32_t>(out->size());
    strip_sizes[s] = static_cast<uint32_t>(packed.size());
    ok = ok && out->append(packed.view());
    if (ok && (out->size() & 1)) ok = out->put8(0);
  }
  if (ok) {
    const uint32_t entry_count = channels == 4 ? 12 : 11;
    const uint32_t ifd = static_cast<uint32_t>(out->size());
    uint32_t extra = ifd + 2 + entry_count * 12 + 4;  // where out-of-line values go
    const uint32_t bits_at = extra;
    extra += 8;
    const uint32_t offsets_at = strips > 1 ? extra : 0;
    if (strips > 1) extra += strips * 4;
    const uint32_t sizes_at = strips > 1 ? extra : 0;
    Entry e[12] = {{256, 4, 1, width}, {257, 4, 1, height}, {258, 3, channels, bits_at}, {259, 3, 1, 8}, {262, 3, 1, 2},
                   {273, 4, strips, strips > 1 ? offsets_at : strip_offsets[0]}, {277, 3, 1, channels}, {278, 4, 1, rows_per_strip},
                   {279, 4, strips, strips > 1 ? sizes_at : strip_sizes[0]}, {284, 3, 1, 1}, {317, 3, 1, 2}, {338, 3, 1, 2}};
    ok = out->patch32(4, ifd) && out->put16(entry_count);
    // (A SHORT value sits in the low half of the 4-byte field, which little-endian put32 gives.)
    for (uint32_t i = 0; ok && i < entry_count; ++i) ok = out->put16(e[i].tag) && out->put16(e[i].type) && out->put32(e[i].count) && out->put32(e[i].value);
    ok = ok && out->put32(0);  // no next IFD
    for (uint32_t i = 0; ok && i < 4; ++i) ok = out->put16(i < channels ? 8 : 0);
    if (strips > 1) {
      for (uint32_t s = 0; ok && s < strips; ++s) ok = out->put32(strip_offsets[s]);
      for (uint32_t s = 0; ok && s < strips; ++s) ok = out->put32(strip_sizes[s]);
    }
  }
  allocator.deallocate(strip_offsets);
  if (!ok) out->truncate(start);
  return ok;
}

}  // namespace media
