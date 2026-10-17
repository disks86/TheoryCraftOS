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

constexpr int32_t kConstBits = 13, kPass1Bits = 2;
constexpr int32_t kFix_0_298631336 = 2446, kFix_0_390180644 = 3196, kFix_0_541196100 = 4433, kFix_0_765366865 = 6270,
                  kFix_0_899976223 = 7373, kFix_1_175875602 = 9633, kFix_1_501321110 = 12299, kFix_1_847759065 = 15137,
                  kFix_1_961570560 = 16069, kFix_2_053119869 = 16819, kFix_2_562915447 = 20995, kFix_3_072711026 = 25172;

int32_t descale(int32_t x, int32_t n) { return (x + (int32_t{1} << (n - 1))) >> n; }
uint8_t clamp_sample(int32_t v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

// The accurate integer inverse DCT of the Independent JPEG Group: `coefficients` are dequantised, in natural order;
// the 8x8 result (level shifted back to 0..255) goes to `out` with a row stride of `stride`.
void inverse_dct(const int32_t* coefficients, uint8_t* out, uint32_t stride) {
  int32_t workspace[64];
  for (int col = 0; col < 8; ++col) {
    const int32_t* in = coefficients + col;
    int32_t* ws = workspace + col;
    if (!in[8] && !in[16] && !in[24] && !in[32] && !in[40] && !in[48] && !in[56]) {
      const int32_t dc = in[0] * (1 << kPass1Bits);
      for (int r = 0; r < 8; ++r) ws[8 * r] = dc;
      continue;
    }
    int32_t z2 = in[16], z3 = in[48];
    int32_t z1 = (z2 + z3) * kFix_0_541196100;
    int32_t tmp2 = z1 + z3 * -kFix_1_847759065;
    int32_t tmp3 = z1 + z2 * kFix_0_765366865;
    z2 = in[0];
    z3 = in[32];
    int32_t tmp0 = (z2 + z3) * (1 << kConstBits);
    int32_t tmp1 = (z2 - z3) * (1 << kConstBits);
    const int32_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
    tmp0 = in[56];
    tmp1 = in[40];
    tmp2 = in[24];
    tmp3 = in[8];
    z1 = tmp0 + tmp3;
    z2 = tmp1 + tmp2;
    z3 = tmp0 + tmp2;
    int32_t z4 = tmp1 + tmp3;
    const int32_t z5 = (z3 + z4) * kFix_1_175875602;
    tmp0 *= kFix_0_298631336;
    tmp1 *= kFix_2_053119869;
    tmp2 *= kFix_3_072711026;
    tmp3 *= kFix_1_501321110;
    z1 *= -kFix_0_899976223;
    z2 *= -kFix_2_562915447;
    z3 *= -kFix_1_961570560;
    z4 *= -kFix_0_390180644;
    z3 += z5;
    z4 += z5;
    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;
    ws[0] = descale(tmp10 + tmp3, kConstBits - kPass1Bits);
    ws[56] = descale(tmp10 - tmp3, kConstBits - kPass1Bits);
    ws[8] = descale(tmp11 + tmp2, kConstBits - kPass1Bits);
    ws[48] = descale(tmp11 - tmp2, kConstBits - kPass1Bits);
    ws[16] = descale(tmp12 + tmp1, kConstBits - kPass1Bits);
    ws[40] = descale(tmp12 - tmp1, kConstBits - kPass1Bits);
    ws[24] = descale(tmp13 + tmp0, kConstBits - kPass1Bits);
    ws[32] = descale(tmp13 - tmp0, kConstBits - kPass1Bits);
  }
  for (int row = 0; row < 8; ++row) {
    const int32_t* ws = workspace + 8 * row;
    uint8_t* o = out + static_cast<uint64_t>(row) * stride;
    int32_t z2 = ws[2], z3 = ws[6];
    int32_t z1 = (z2 + z3) * kFix_0_541196100;
    int32_t tmp2 = z1 + z3 * -kFix_1_847759065;
    int32_t tmp3 = z1 + z2 * kFix_0_765366865;
    int32_t tmp0 = (ws[0] + ws[4]) * (1 << kConstBits);
    int32_t tmp1 = (ws[0] - ws[4]) * (1 << kConstBits);
    const int32_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
    tmp0 = ws[7];
    tmp1 = ws[5];
    tmp2 = ws[3];
    tmp3 = ws[1];
    z1 = tmp0 + tmp3;
    z2 = tmp1 + tmp2;
    z3 = tmp0 + tmp2;
    int32_t z4 = tmp1 + tmp3;
    const int32_t z5 = (z3 + z4) * kFix_1_175875602;
    tmp0 *= kFix_0_298631336;
    tmp1 *= kFix_2_053119869;
    tmp2 *= kFix_3_072711026;
    tmp3 *= kFix_1_501321110;
    z1 *= -kFix_0_899976223;
    z2 *= -kFix_2_562915447;
    z3 *= -kFix_1_961570560;
    z4 *= -kFix_0_390180644;
    z3 += z5;
    z4 += z5;
    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;
    constexpr int32_t shift = kConstBits + kPass1Bits + 3;
    o[0] = clamp_sample(descale(tmp10 + tmp3, shift) + 128);
    o[7] = clamp_sample(descale(tmp10 - tmp3, shift) + 128);
    o[1] = clamp_sample(descale(tmp11 + tmp2, shift) + 128);
    o[6] = clamp_sample(descale(tmp11 - tmp2, shift) + 128);
    o[2] = clamp_sample(descale(tmp12 + tmp1, shift) + 128);
    o[5] = clamp_sample(descale(tmp12 - tmp1, shift) + 128);
    o[3] = clamp_sample(descale(tmp13 + tmp0, shift) + 128);
    o[4] = clamp_sample(descale(tmp13 - tmp0, shift) + 128);
  }
}

// A genuine dequantised coefficient of an 8-bit picture is within a few thousand; bounding it keeps a damaged file
// from overflowing the transform's arithmetic.
int32_t limit_coefficient(int32_t v) { return v < -4096 ? -4096 : v > 4096 ? 4096 : v; }

uint32_t be16(ConstByteView v, uint64_t at) { return (uint32_t{v[at]} << 8) | v[at + 1]; }
}  // namespace

// Reads the entropy-coded part of a scan bit by bit: removes the 0 byte stuffed after each 0xFF and stops at a
// marker (feeding zero bits from then on, so a truncated scan decodes to something rather than reading outside).
struct JpegDecoder::Bits {
  ConstByteView data;
  uint64_t position;
  uint32_t buffer = 0;
  int32_t count = 0;
  bool hit_marker = false;

  int32_t bit() {
    if (count == 0) {
      uint32_t byte = 0;
      if (!hit_marker && position < data.size()) {
        byte = data[position];
        if (byte == 0xFF) {
          const uint32_t next = position + 1 < data.size() ? data[position + 1] : 0xD9;
          if (next == 0) position += 2;  // a stuffed 0xFF
          else {
            hit_marker = true;
            byte = 0;
          }
        } else {
          ++position;
        }
      } else {
        hit_marker = true;
      }
      buffer = byte;
      count = 8;
    }
    --count;
    return (buffer >> count) & 1;
  }
  uint32_t receive(int32_t n) {
    uint32_t v = 0;
    while (n-- > 0) v = (v << 1) | static_cast<uint32_t>(bit());
    return v;
  }
  // After a restart interval: drop the partial byte and move past the RSTn marker.
  void restart() {
    count = 0;
    hit_marker = false;
    while (position + 1 < data.size()) {
      if (data[position] == 0xFF && data[position + 1] >= 0xD0 && data[position + 1] <= 0xD7) {
        position += 2;
        return;
      }
      if (data[position] == 0xFF && data[position + 1] != 0 && data[position + 1] != 0xFF) return;  // some other marker: stay
      ++position;
    }
  }
};

void JpegDecoder::release() {
  for (Component& c : components_) release_view(allocator_, c.plane);
  decoded_ = false;
  frame_seen_ = false;
}

bool JpegDecoder::parse_quant(ConstByteView s) {
  uint64_t at = 0;
  while (at < s.size()) {
    const uint32_t precision = s[at] >> 4, id = s[at] & 15;
    ++at;
    const uint64_t need = precision ? 128 : 64;
    if (id > 3 || precision > 1 || at + need > s.size()) return false;
    for (int i = 0; i < 64; ++i) {
      quant_[id][kZigZag[i]] = precision ? static_cast<uint16_t>(be16(s, at + 2 * i)) : s[at + i];
    }
    quant_defined_[id] = true;
    at += need;
  }
  return true;
}

bool JpegDecoder::parse_huffman(ConstByteView s) {
  uint64_t at = 0;
  while (at + 17 <= s.size()) {
    const uint32_t cls = s[at] >> 4, id = s[at] & 15;
    if (cls > 1 || id > 3) return false;
    uint8_t counts[17] = {};
    uint32_t total = 0;
    for (int i = 1; i <= 16; ++i) {
      counts[i] = s[at + i];
      total += counts[i];
    }
    at += 17;
    if (total > 256 || at + total > s.size()) return false;
    Huffman& h = cls ? ac_[id] : dc_[id];
    for (uint32_t i = 0; i < total; ++i) h.symbols[i] = s[at + i];
    at += total;
    // Canonical codes: for each length the smallest code, the largest, and where its symbols start.
    int32_t code = 0, index = 0;
    for (int len = 1; len <= 16; ++len) {
      h.first_index[len] = index;
      h.min_code[len] = code;
      code += counts[len];
      index += counts[len];
      h.max_code[len] = counts[len] ? code - 1 : -1;
      code <<= 1;
    }
    h.max_code[17] = 0x7FFFFFFF;
    h.defined = true;
  }
  return at == s.size();
}

bool JpegDecoder::parse_frame_header(ConstByteView s) {
  if (s.size() < 6 || s[0] != 8) return false;  // 8-bit samples only
  height_ = be16(s, 1);
  width_ = be16(s, 3);
  component_count_ = s[5];
  if (width_ == 0 || height_ == 0 || (component_count_ != 1 && component_count_ != 3) || s.size() < 6 + 3 * uint64_t{component_count_})
    return false;
  max_h_ = max_v_ = 1;
  for (uint32_t i = 0; i < component_count_; ++i) {
    Component& c = components_[i];
    c.id = s[6 + 3 * i];
    c.h = s[7 + 3 * i] >> 4;
    c.v = s[7 + 3 * i] & 15;
    c.quant = s[8 + 3 * i];
    if (c.h < 1 || c.h > 2 || c.v < 1 || c.v > 2 || c.quant > 3) return false;
    if (c.h > max_h_) max_h_ = c.h;
    if (c.v > max_v_) max_v_ = c.v;
  }
  if (component_count_ == 1) {  // a grey picture is always one block per MCU, whatever the sampling says
    components_[0].h = components_[0].v = 1;
    max_h_ = max_v_ = 1;
  }
  const uint32_t mcu_w = 8 * max_h_, mcu_h = 8 * max_v_;
  const uint32_t mcus_x = (width_ + mcu_w - 1) / mcu_w, mcus_y = (height_ + mcu_h - 1) / mcu_h;
  for (uint32_t i = 0; i < component_count_; ++i) {
    Component& c = components_[i];
    c.blocks_x = mcus_x * c.h;
    c.blocks_y = mcus_y * c.v;
    release_view(allocator_, c.plane);
    c.plane = allocate_view<uint8_t>(allocator_, uint64_t{c.blocks_x} * 8 * c.blocks_y * 8, 16);
    if (c.plane.empty()) return false;
    c.plane.fill(128);
  }
  frame_seen_ = true;
  return true;
}

// Decodes one 8x8 block: the DC difference and the AC run-length codes, dequantises and transforms into the plane.
bool JpegDecoder::decode_block(Bits& bits, Component& c, uint8_t* destination, uint32_t stride) {
  const Huffman& dc = dc_[c.dc_table];
  const Huffman& ac = ac_[c.ac_table];
  auto decode_symbol = [&](const Huffman& h) -> int32_t {
    int32_t code = 0;
    for (int len = 1; len <= 16; ++len) {
      code = (code << 1) | bits.bit();
      if (h.max_code[len] >= 0 && code <= h.max_code[len] && code >= h.min_code[len]) return h.symbols[h.first_index[len] + code - h.min_code[len]];
    }
    return -1;
  };
  auto extend = [](int32_t v, int32_t t) { return v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; };

  int32_t coefficients[64] = {};
  const uint16_t* q = quant_[c.quant];
  const int32_t t = decode_symbol(dc);
  if (t < 0 || t > 11) return false;
  c.dc_prediction += t ? extend(static_cast<int32_t>(bits.receive(t)), t) : 0;
  if (c.dc_prediction > 32767) c.dc_prediction = 32767;  // only a damaged file gets here
  if (c.dc_prediction < -32768) c.dc_prediction = -32768;
  coefficients[0] = limit_coefficient(c.dc_prediction * q[0]);
  for (int k = 1; k < 64;) {
    const int32_t rs = decode_symbol(ac);
    if (rs < 0) return false;
    const int32_t size = rs & 15, run = rs >> 4;
    if (size == 0) {
      if (run == 15) {  // sixteen zeros
        k += 16;
        continue;
      }
      break;  // end of block
    }
    k += run;
    if (k > 63) return false;
    coefficients[kZigZag[k]] = limit_coefficient(extend(static_cast<int32_t>(bits.receive(size)), size) * q[kZigZag[k]]);
    ++k;
  }
  inverse_dct(coefficients, destination, stride);
  return true;
}

bool JpegDecoder::decode_scan(ConstByteView data, uint64_t* position, const uint8_t* scan_components, uint32_t scan_count) {
  Bits bits{data, *position};
  for (uint32_t i = 0; i < component_count_; ++i) components_[i].dc_prediction = 0;
  uint32_t until_restart = restart_interval_;

  Component* scan[3];
  for (uint32_t i = 0; i < scan_count; ++i) scan[i] = &components_[scan_components[i]];

  // One component per scan: its blocks in raster order (only those that cover the picture); several: MCU by MCU.
  uint32_t units_x, units_y;
  if (scan_count == 1) {
    const Component& c = *scan[0];
    const uint32_t cw = (width_ * c.h + max_h_ - 1) / max_h_, ch = (height_ * c.v + max_v_ - 1) / max_v_;
    units_x = (cw + 7) / 8;
    units_y = (ch + 7) / 8;
  } else {
    units_x = components_[0].blocks_x / components_[0].h;
    units_y = components_[0].blocks_y / components_[0].v;
  }
  for (uint32_t uy = 0; uy < units_y; ++uy) {
    for (uint32_t ux = 0; ux < units_x; ++ux) {
      if (restart_interval_ && until_restart == 0) {
        bits.restart();
        for (uint32_t i = 0; i < component_count_; ++i) components_[i].dc_prediction = 0;
        until_restart = restart_interval_;
      }
      for (uint32_t s = 0; s < scan_count; ++s) {
        Component& c = *scan[s];
        const uint32_t bh = scan_count == 1 ? 1 : c.h, bv = scan_count == 1 ? 1 : c.v;
        for (uint32_t by = 0; by < bv; ++by)
          for (uint32_t bx = 0; bx < bh; ++bx) {
            const uint32_t block_x = scan_count == 1 ? ux : ux * c.h + bx, block_y = scan_count == 1 ? uy : uy * c.v + by;
            if (block_x >= c.blocks_x || block_y >= c.blocks_y) {  // outside the padded plane (cannot happen for valid files)
              return false;
            }
            const uint32_t stride = c.blocks_x * 8;
            uint8_t* destination = c.plane.data() + uint64_t{block_y} * 8 * stride + uint64_t{block_x} * 8;
            if (!decode_block(bits, c, destination, stride)) return false;
          }
      }
      if (restart_interval_) --until_restart;
    }
  }
  *position = bits.position;
  return true;
}

bool JpegDecoder::decode(ConstByteView data) {
  release();
  for (bool& d : quant_defined_) d = false;
  for (Huffman& h : dc_) h.defined = false;
  for (Huffman& h : ac_) h.defined = false;
  restart_interval_ = 0;
  if (data.size() < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
  uint64_t at = 2;
  bool scanned = false;
  while (at + 4 <= data.size()) {
    if (data[at] != 0xFF) {  // garbage between segments: look for the next marker
      ++at;
      continue;
    }
    const uint32_t marker = data[at + 1];
    if (marker == 0xFF) {  // fill byte
      ++at;
      continue;
    }
    if (marker == 0xD9) break;  // EOI
    if (marker == 0x00 || (marker >= 0xD0 && marker <= 0xD8) || marker == 0x01) {
      at += 2;
      continue;
    }
    const uint32_t length = be16(data, at + 2);
    if (length < 2 || at + 2 + length > data.size()) return false;
    const ConstByteView segment = data.subview(at + 4, length - 2);
    at += 2 + length;
    switch (marker) {
      case 0xDB: if (!parse_quant(segment)) return false; break;
      case 0xC4: if (!parse_huffman(segment)) return false; break;
      case 0xDD:
        if (segment.size() < 2) return false;
        restart_interval_ = be16(segment, 0);
        break;
      case 0xC0:
      case 0xC1:
        if (frame_seen_ || !parse_frame_header(segment)) return false;
        break;
      case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
        return false;  // progressive, lossless, arithmetic or hierarchical: not supported
      case 0xDA: {
        if (!frame_seen_ || segment.size() < 1 + 2 * uint64_t{segment[0]} + 3) return false;
        const uint32_t count = segment[0];
        if (count < 1 || count > component_count_) return false;
        uint8_t scan[3];
        for (uint32_t i = 0; i < count; ++i) {
          const uint32_t id = segment[1 + 2 * i];
          uint32_t found = component_count_;
          for (uint32_t k = 0; k < component_count_; ++k)
            if (components_[k].id == id) found = k;
          if (found == component_count_) return false;
          scan[i] = static_cast<uint8_t>(found);
          components_[found].dc_table = segment[2 + 2 * i] >> 4;
          components_[found].ac_table = segment[2 + 2 * i] & 15;
          if (components_[found].dc_table > 3 || components_[found].ac_table > 3 || !dc_[components_[found].dc_table].defined ||
              !ac_[components_[found].ac_table].defined || !quant_defined_[components_[found].quant])
            return false;
        }
        if (count != 1 && count != component_count_) return false;
        if (!decode_scan(data, &at, scan, count)) return false;
        scanned = true;
        break;
      }
      default: break;  // APPn, COM and the like
    }
  }
  decoded_ = scanned && frame_seen_;
  return decoded_;
}

// One component's value at picture pixel (x, y), whatever its sampling.
uint32_t JpegDecoder::sample(uint32_t component, uint32_t x, uint32_t y) const {
  const Component& c = components_[component];
  const uint32_t cx = x * c.h / max_h_, cy = y * c.v / max_v_;
  return c.plane[uint64_t{cy} * c.blocks_x * 8 + cx];
}

bool JpegDecoder::output(const VideoFrameBuffer& out) const {
  if (!decoded_ || !out.valid() || out.format().width != width_ || out.format().height != height_) return false;
  const VideoFormat& fmt = out.format();
  const bool grey = component_count_ == 1;
  if (fmt.pixel_format == PixelFormat::Bgra8888) {
    uint32_t* dest = reinterpret_cast<uint32_t*>(out.plane(0).data());
    for (uint32_t y = 0; y < height_; ++y)
      for (uint32_t x = 0; x < width_; ++x) {
        const int32_t yy = static_cast<int32_t>(sample(0, x, y));
        int32_t u = 0, v = 0;
        if (!grey) {
          u = static_cast<int32_t>(sample(1, x, y)) - 128;
          v = static_cast<int32_t>(sample(2, x, y)) - 128;
        }
        auto clamp = [](int32_t value) { return static_cast<uint32_t>(value < 0 ? 0 : value > 255 ? 255 : value); };
        const uint32_t r = clamp(yy + ((91881 * v + 32768) >> 16));
        const uint32_t g = clamp(yy - ((22554 * u + 46802 * v + 32768) >> 16));
        const uint32_t b = clamp(yy + ((116130 * u + 32768) >> 16));
        dest[uint64_t{y} * width_ + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
      }
    return true;
  }
  // I420 or NV12: the luma as is, the chroma averaged over 2x2 pixels (grey: neutral).
  for (uint32_t y = 0; y < height_; ++y)
    for (uint32_t x = 0; x < width_; ++x) out.plane(0)[uint64_t{y} * fmt.plane_stride(0) + x] = static_cast<uint8_t>(sample(0, x, y));
  for (uint32_t cy = 0; cy < fmt.chroma_height(); ++cy)
    for (uint32_t cx = 0; cx < fmt.chroma_width(); ++cx) {
      uint32_t u = 128, v = 128;
      if (!grey) {
        uint32_t su = 0, sv = 0;
        for (uint32_t dy = 0; dy < 2; ++dy)
          for (uint32_t dx = 0; dx < 2; ++dx) {
            const uint32_t px = 2 * cx + dx < width_ ? 2 * cx + dx : width_ - 1, py = 2 * cy + dy < height_ ? 2 * cy + dy : height_ - 1;
            su += sample(1, px, py);
            sv += sample(2, px, py);
          }
        u = (su + 2) / 4;
        v = (sv + 2) / 4;
      }
      if (fmt.pixel_format == PixelFormat::I420) {
        out.plane(1)[uint64_t{cy} * fmt.plane_stride(1) + cx] = static_cast<uint8_t>(u);
        out.plane(2)[uint64_t{cy} * fmt.plane_stride(2) + cx] = static_cast<uint8_t>(v);
      } else {
        out.plane(1)[uint64_t{cy} * fmt.plane_stride(1) + 2 * cx] = static_cast<uint8_t>(u);
        out.plane(1)[uint64_t{cy} * fmt.plane_stride(1) + 2 * cx + 1] = static_cast<uint8_t>(v);
      }
    }
  return true;
}

}  // namespace hardware
