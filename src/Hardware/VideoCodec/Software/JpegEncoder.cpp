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

#include "Hardware/VideoCodec/Software/Jpeg.hpp"
#include "Hardware/VideoCodec/ColorConvert.hpp"

namespace hardware {

namespace {
constexpr uint8_t kZigZag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// The example quantisation tables of the standard (natural order); quality scales them.
constexpr uint8_t kLumaBase[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,  14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                   18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,  49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr uint8_t kChromaBase[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

constexpr int32_t kConstBits = 13, kPass1Bits = 2;
constexpr int32_t kFix_0_298631336 = 2446, kFix_0_390180644 = 3196, kFix_0_541196100 = 4433, kFix_0_765366865 = 6270,
                  kFix_0_899976223 = 7373, kFix_1_175875602 = 9633, kFix_1_501321110 = 12299, kFix_1_847759065 = 15137,
                  kFix_1_961570560 = 16069, kFix_2_053119869 = 16819, kFix_2_562915447 = 20995, kFix_3_072711026 = 25172;

int32_t descale(int32_t x, int32_t n) { return (x + (int32_t{1} << (n - 1))) >> n; }

// The accurate integer forward DCT of the Independent JPEG Group: 8x8 level-shifted samples in, coefficients
// (scaled up by 8, natural order) out.
void forward_dct(int32_t* data) {
  for (int row = 0; row < 8; ++row) {
    int32_t* d = data + 8 * row;
    const int32_t tmp0 = d[0] + d[7], tmp7 = d[0] - d[7], tmp1 = d[1] + d[6], tmp6 = d[1] - d[6];
    const int32_t tmp2 = d[2] + d[5], tmp5 = d[2] - d[5], tmp3 = d[3] + d[4], tmp4 = d[3] - d[4];
    const int32_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
    d[0] = (tmp10 + tmp11) * (1 << kPass1Bits);
    d[4] = (tmp10 - tmp11) * (1 << kPass1Bits);
    int32_t z1 = (tmp12 + tmp13) * kFix_0_541196100;
    d[2] = descale(z1 + tmp13 * kFix_0_765366865, kConstBits - kPass1Bits);
    d[6] = descale(z1 + tmp12 * -kFix_1_847759065, kConstBits - kPass1Bits);
    z1 = tmp4 + tmp7;
    int32_t z2 = tmp5 + tmp6, z3 = tmp4 + tmp6, z4 = tmp5 + tmp7;
    const int32_t z5 = (z3 + z4) * kFix_1_175875602;
    const int32_t t4 = tmp4 * kFix_0_298631336, t5 = tmp5 * kFix_2_053119869, t6 = tmp6 * kFix_3_072711026, t7 = tmp7 * kFix_1_501321110;
    z1 *= -kFix_0_899976223;
    z2 *= -kFix_2_562915447;
    z3 *= -kFix_1_961570560;
    z4 *= -kFix_0_390180644;
    z3 += z5;
    z4 += z5;
    d[7] = descale(t4 + z1 + z3, kConstBits - kPass1Bits);
    d[5] = descale(t5 + z2 + z4, kConstBits - kPass1Bits);
    d[3] = descale(t6 + z2 + z3, kConstBits - kPass1Bits);
    d[1] = descale(t7 + z1 + z4, kConstBits - kPass1Bits);
  }
  for (int col = 0; col < 8; ++col) {
    int32_t* d = data + col;
    const int32_t tmp0 = d[0] + d[56], tmp7 = d[0] - d[56], tmp1 = d[8] + d[48], tmp6 = d[8] - d[48];
    const int32_t tmp2 = d[16] + d[40], tmp5 = d[16] - d[40], tmp3 = d[24] + d[32], tmp4 = d[24] - d[32];
    const int32_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
    d[0] = descale(tmp10 + tmp11, kPass1Bits);
    d[32] = descale(tmp10 - tmp11, kPass1Bits);
    int32_t z1 = (tmp12 + tmp13) * kFix_0_541196100;
    d[16] = descale(z1 + tmp13 * kFix_0_765366865, kConstBits + kPass1Bits);
    d[48] = descale(z1 + tmp12 * -kFix_1_847759065, kConstBits + kPass1Bits);
    z1 = tmp4 + tmp7;
    int32_t z2 = tmp5 + tmp6, z3 = tmp4 + tmp6, z4 = tmp5 + tmp7;
    const int32_t z5 = (z3 + z4) * kFix_1_175875602;
    const int32_t t4 = tmp4 * kFix_0_298631336, t5 = tmp5 * kFix_2_053119869, t6 = tmp6 * kFix_3_072711026, t7 = tmp7 * kFix_1_501321110;
    z1 *= -kFix_0_899976223;
    z2 *= -kFix_2_562915447;
    z3 *= -kFix_1_961570560;
    z4 *= -kFix_0_390180644;
    z3 += z5;
    z4 += z5;
    d[56] = descale(t4 + z1 + z3, kConstBits + kPass1Bits);
    d[40] = descale(t5 + z2 + z4, kConstBits + kPass1Bits);
    d[24] = descale(t6 + z2 + z3, kConstBits + kPass1Bits);
    d[8] = descale(t7 + z1 + z4, kConstBits + kPass1Bits);
  }
}

// How many bits the magnitude of `v` needs (0 for 0).
int32_t bit_length(int32_t v) {
  uint32_t m = static_cast<uint32_t>(v < 0 ? -v : v);
  int32_t n = 0;
  while (m) {
    ++n;
    m >>= 1;
  }
  return n;
}

// The optimal length-limited Huffman table for the symbol counts `frequency` (Annex K.3 of the standard, as in libjpeg):
// `bits[n]` codes of length n and `values` in code order. Returns the number of symbols.
uint32_t build_huffman(const uint32_t* frequency_in, uint8_t* bits, uint8_t* values) {
  int64_t frequency[257];
  int32_t code_size[257], others[257];
  for (int i = 0; i < 256; ++i) frequency[i] = frequency_in[i];
  frequency[256] = 1;  // one code point is reserved so that no real symbol gets the all-ones code
  for (int i = 0; i < 257; ++i) {
    code_size[i] = 0;
    others[i] = -1;
  }
  for (;;) {
    int32_t c1 = -1, c2 = -1;
    int64_t v = INT64_MAX;
    for (int i = 0; i <= 256; ++i)
      if (frequency[i] && frequency[i] <= v) {
        v = frequency[i];
        c1 = i;
      }
    v = INT64_MAX;
    for (int i = 0; i <= 256; ++i)
      if (frequency[i] && frequency[i] <= v && i != c1) {
        v = frequency[i];
        c2 = i;
      }
    if (c2 < 0) break;
    frequency[c1] += frequency[c2];
    frequency[c2] = 0;
    ++code_size[c1];
    while (others[c1] >= 0) {
      c1 = others[c1];
      ++code_size[c1];
    }
    others[c1] = c2;
    ++code_size[c2];
    while (others[c2] >= 0) {
      c2 = others[c2];
      ++code_size[c2];
    }
  }
  int32_t counts[34] = {};
  for (int i = 0; i <= 256; ++i)
    if (code_size[i]) counts[code_size[i] > 32 ? 32 : code_size[i]]++;
  for (int i = 32; i > 16; --i)  // no code longer than 16 bits
    while (counts[i] > 0) {
      int j = i - 2;
      while (counts[j] == 0) --j;
      counts[i] -= 2;
      counts[i - 1] += 1;
      counts[j + 1] += 2;
      counts[j] -= 1;
    }
  int i = 16;
  while (counts[i] == 0) --i;
  --counts[i];  // the reserved code point goes
  for (int n = 1; n <= 16; ++n) bits[n] = static_cast<uint8_t>(counts[n]);
  uint32_t total = 0;
  for (int len = 1; len <= 32; ++len)
    for (int s = 0; s < 256; ++s)
      if (code_size[s] == len) values[total++] = static_cast<uint8_t>(s);
  return total;
}
}  // namespace

// The state of one encode: the quantised coefficients of every block (in the order they are written), the
// symbol statistics, the Huffman tables derived from them and the file being written.
struct JpegEncoder::Tables {
  uint8_t quant[2][64];  // luma, chroma; natural order
  uint32_t dc_frequency[2][256], ac_frequency[2][256];
  uint8_t dc_bits[2][17], ac_bits[2][17];
  uint8_t dc_values[2][256], ac_values[2][256];
  uint32_t dc_count[2], ac_count[2];
  uint32_t dc_code[2][256], dc_size[2][256], ac_code[2][256], ac_size[2][256];
};

struct JpegEncoder::Writer {
  JpegEncoder& e;
  bool ok = true;
  uint32_t buffer = 0;
  int32_t count = 0;

  void byte(uint8_t b) {
    if (e.length_ == e.buffer_.size()) {
      ByteView bigger = allocate_view<uint8_t>(e.allocator_, e.buffer_.empty() ? 4096 : e.buffer_.size() * 2);
      if (bigger.empty()) {
        ok = false;
        return;
      }
      if (e.length_) bigger.first(e.length_).copy_from(ConstByteView(e.buffer_).first(e.length_));
      release_view(e.allocator_, e.buffer_);
      e.buffer_ = bigger;
    }
    e.buffer_[e.length_++] = b;
  }
  void word(uint32_t v) {
    byte(static_cast<uint8_t>(v >> 8));
    byte(static_cast<uint8_t>(v));
  }
  void marker(uint8_t m) {
    byte(0xFF);
    byte(m);
  }
  // Entropy-coded bits, most significant first, with a 0 stuffed after every 0xFF.
  void bits(uint32_t value, int32_t n) {
    for (int32_t i = n - 1; i >= 0; --i) {
      buffer = (buffer << 1) | ((value >> i) & 1);
      if (++count == 8) {
        byte(static_cast<uint8_t>(buffer));
        if ((buffer & 0xFF) == 0xFF) byte(0);
        buffer = 0;
        count = 0;
      }
    }
  }
  void finish_bits() {
    while (count != 0) bits(1, 1);  // pad with ones
  }
};

bool JpegEncoder::encode(const VideoFrame& frame, uint32_t quality) {
  const VideoFormat fmt = frame.format;
  length_ = 0;
  if (!fmt.valid() || !frame.plane[0] || fmt.width > 65535 || fmt.height > 65535) return false;
  if (quality < 1) quality = 1;
  if (quality > 100) quality = 100;

  // The picture as full-range I420.
  VideoFrameBuffer converted(allocator_);
  VideoFrame yuv = frame;
  if (fmt.pixel_format != PixelFormat::I420 || frame.limited_range) {
    VideoFrameBuffer bgra(allocator_);
    VideoFrame source = frame;
    if (fmt.pixel_format != PixelFormat::Bgra8888 && frame.limited_range) {  // widen the range by way of RGB
      if (!bgra.allocate({fmt.width, fmt.height, PixelFormat::Bgra8888}) || !convert_frame(frame, bgra)) return false;
      source = bgra.frame();
    }
    if (!converted.allocate({fmt.width, fmt.height, PixelFormat::I420}) || !convert_frame(source, converted)) return false;
    yuv = converted.frame();
  }
  const VideoFormat yfmt = yuv.format;

  // Block layout: 16x16 MCUs of four luma blocks and one block of each chroma.
  const uint32_t mcus_x = (fmt.width + 15) / 16, mcus_y = (fmt.height + 15) / 16;
  const uint64_t block_count = uint64_t{mcus_x} * mcus_y * 6;
  ArrayView<int16_t> coefficients = allocate_view<int16_t>(allocator_, block_count * 64);
  Tables* tables = static_cast<Tables*>(allocator_.allocate(sizeof(Tables)));
  if (coefficients.empty() || !tables) {
    release_view(allocator_, coefficients);
    allocator_.deallocate(tables);
    return false;
  }
  const int32_t scale = quality < 50 ? static_cast<int32_t>(5000 / quality) : static_cast<int32_t>(200 - 2 * quality);
  for (int i = 0; i < 64; ++i) {
    auto scaled = [&](uint8_t base) {
      int32_t v = (base * scale + 50) / 100;
      return static_cast<uint8_t>(v < 1 ? 1 : v > 255 ? 255 : v);
    };
    tables->quant[0][i] = scaled(kLumaBase[i]);
    tables->quant[1][i] = scaled(kChromaBase[i]);
  }

  // Transform and quantise every block.
  auto do_block = [&](uint32_t plane, uint32_t block_x, uint32_t block_y, const uint8_t* quant, int16_t* out) {
    const uint32_t pw = yfmt.plane_width(plane), ph = yfmt.plane_height(plane);
    int32_t data[64];
    for (uint32_t y = 0; y < 8; ++y)
      for (uint32_t x = 0; x < 8; ++x) {
        uint32_t sx = block_x * 8 + x, sy = block_y * 8 + y;
        if (sx >= pw) sx = pw - 1;  // the part beyond the edge repeats it
        if (sy >= ph) sy = ph - 1;
        data[y * 8 + x] = static_cast<int32_t>(yuv.plane[plane][uint64_t{sy} * yuv.stride[plane] + sx]) - 128;
      }
    forward_dct(data);
    for (int k = 0; k < 64; ++k) {
      const int32_t q = quant[kZigZag[k]] * 8;
      const int32_t c = data[kZigZag[k]];
      out[k] = static_cast<int16_t>(c >= 0 ? (c + q / 2) / q : -((-c + q / 2) / q));
    }
  };
  uint64_t index = 0;
  for (uint32_t my = 0; my < mcus_y; ++my)
    for (uint32_t mx = 0; mx < mcus_x; ++mx) {
      for (uint32_t by = 0; by < 2; ++by)
        for (uint32_t bx = 0; bx < 2; ++bx) do_block(0, mx * 2 + bx, my * 2 + by, tables->quant[0], &coefficients[64 * index++]);
      do_block(1, mx, my, tables->quant[1], &coefficients[64 * index++]);
      do_block(2, mx, my, tables->quant[1], &coefficients[64 * index++]);
    }

  // Walks the blocks in file order. The first time it only counts the symbols; then it writes them.
  Writer w{*this};
  bool counting = true;
  auto walk = [&]() {
    int32_t previous_dc[3] = {0, 0, 0};
    auto emit = [&](bool dc, uint32_t table, uint32_t symbol, int32_t extra, int32_t extra_bits) {
      if (counting) {
        (dc ? tables->dc_frequency : tables->ac_frequency)[table][symbol]++;
      } else {
        const uint32_t code = (dc ? tables->dc_code : tables->ac_code)[table][symbol];
        const uint32_t size = (dc ? tables->dc_size : tables->ac_size)[table][symbol];
        w.bits(code, static_cast<int32_t>(size));
        if (extra_bits) w.bits(static_cast<uint32_t>(extra) & ((1u << extra_bits) - 1), extra_bits);
      }
    };
    uint64_t block = 0;
    for (uint64_t mcu = 0; mcu < uint64_t{mcus_x} * mcus_y; ++mcu)
      for (uint32_t b = 0; b < 6; ++b, ++block) {
        const uint32_t component = b < 4 ? 0 : b - 3, table = b < 4 ? 0 : 1;
        const int16_t* zz = &coefficients[64 * block];
        const int32_t diff = zz[0] - previous_dc[component];
        previous_dc[component] = zz[0];
        const int32_t dc_size = bit_length(diff);
        emit(true, table, static_cast<uint32_t>(dc_size), diff < 0 ? diff - 1 : diff, dc_size);
        int32_t run = 0;
        for (int k = 1; k < 64; ++k) {
          if (zz[k] == 0) {
            ++run;
            continue;
          }
          while (run > 15) {
            emit(false, table, 0xF0, 0, 0);
            run -= 16;
          }
          const int32_t size = bit_length(zz[k]);
          emit(false, table, static_cast<uint32_t>((run << 4) | size), zz[k] < 0 ? zz[k] - 1 : zz[k], size);
          run = 0;
        }
        if (run > 0) emit(false, table, 0x00, 0, 0);
      }
  };
  for (int t = 0; t < 2; ++t)
    for (int s = 0; s < 256; ++s) tables->dc_frequency[t][s] = tables->ac_frequency[t][s] = 0;
  walk();
  for (int t = 0; t < 2; ++t) {
    tables->dc_count[t] = build_huffman(tables->dc_frequency[t], tables->dc_bits[t], tables->dc_values[t]);
    tables->ac_count[t] = build_huffman(tables->ac_frequency[t], tables->ac_bits[t], tables->ac_values[t]);
    auto assign = [](const uint8_t* bits, const uint8_t* values, uint32_t* code, uint32_t* size) {
      uint32_t next = 0, k = 0;
      for (uint32_t len = 1; len <= 16; ++len) {
        for (uint32_t n = 0; n < bits[len]; ++n, ++k) {
          code[values[k]] = next++;
          size[values[k]] = len;
        }
        next <<= 1;
      }
    };
    assign(tables->dc_bits[t], tables->dc_values[t], tables->dc_code[t], tables->dc_size[t]);
    assign(tables->ac_bits[t], tables->ac_values[t], tables->ac_code[t], tables->ac_size[t]);
  }

  // Headers.
  w.marker(0xD8);
  w.marker(0xE0);  // JFIF
  w.word(16);
  static const uint8_t kJfif[8] = {'J', 'F', 'I', 'F', 0, 1, 1, 0};
  for (uint8_t b : kJfif) w.byte(b);
  w.word(1);
  w.word(1);
  w.byte(0);
  w.byte(0);
  for (uint32_t t = 0; t < 2; ++t) {
    w.marker(0xDB);
    w.word(67);
    w.byte(static_cast<uint8_t>(t));
    for (int k = 0; k < 64; ++k) w.byte(tables->quant[t][kZigZag[k]]);
  }
  w.marker(0xC0);
  w.word(17);
  w.byte(8);
  w.word(fmt.height);
  w.word(fmt.width);
  w.byte(3);
  w.byte(1); w.byte(0x22); w.byte(0);   // Y: 2x2, quantisation table 0
  w.byte(2); w.byte(0x11); w.byte(1);   // Cb
  w.byte(3); w.byte(0x11); w.byte(1);   // Cr
  for (uint32_t t = 0; t < 2; ++t)
    for (uint32_t cls = 0; cls < 2; ++cls) {
      const uint8_t* bits = cls ? tables->ac_bits[t] : tables->dc_bits[t];
      const uint8_t* values = cls ? tables->ac_values[t] : tables->dc_values[t];
      const uint32_t n = cls ? tables->ac_count[t] : tables->dc_count[t];
      w.marker(0xC4);
      w.word(19 + n);
      w.byte(static_cast<uint8_t>((cls << 4) | t));
      for (int len = 1; len <= 16; ++len) w.byte(bits[len]);
      for (uint32_t i = 0; i < n; ++i) w.byte(values[i]);
    }
  w.marker(0xDA);
  w.word(12);
  w.byte(3);
  w.byte(1); w.byte(0x00);
  w.byte(2); w.byte(0x11);
  w.byte(3); w.byte(0x11);
  w.byte(0); w.byte(63); w.byte(0);

  counting = false;
  walk();
  w.finish_bits();
  w.marker(0xD9);

  release_view(allocator_, coefficients);
  allocator_.deallocate(tables);
  return w.ok;
}

}  // namespace hardware
