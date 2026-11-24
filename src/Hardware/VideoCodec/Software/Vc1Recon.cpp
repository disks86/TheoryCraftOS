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


// VC-1 decoding, part 3: the inverse transforms, motion compensation, overlap smoothing and the in-loop filter (SMPTE 421M clauses 8.3, 8.4 and 8.6).
#include "Hardware/VideoCodec/Software/Vc1Core.hpp"

namespace hardware {
namespace vc1 {

namespace {
#include "Hardware/VideoCodec/Software/Vc1Tables.inc"

int clip255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
int clip_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int16_t s16(int v) { return static_cast<int16_t>(v); }
int iabs(int v) { return v < 0 ? -v : v; }
}  // namespace

// ---- inverse transforms --------------------------------------------------------------------------------------------------------------

// 8.1.2 the 8x8 inverse transform, in place. (The coefficients are in the transposed layout the scans produce; the samples come out row by row.)
void inv_trans_8x8(int16_t* block) {
  int16_t temp[64];
  const int16_t* src = block;
  int16_t* dst = temp;
  for (int i = 0; i < 8; ++i) {
    int t1 = 12 * (src[0] + src[32]) + 4;
    int t2 = 12 * (src[0] - src[32]) + 4;
    int t3 = 16 * src[16] + 6 * src[48];
    int t4 = 6 * src[16] - 16 * src[48];
    const int t5 = t1 + t3, t6 = t2 + t4, t7 = t2 - t4, t8 = t1 - t3;
    t1 = 16 * src[8] + 15 * src[24] + 9 * src[40] + 4 * src[56];
    t2 = 15 * src[8] - 4 * src[24] - 16 * src[40] - 9 * src[56];
    t3 = 9 * src[8] - 16 * src[24] + 4 * src[40] + 15 * src[56];
    t4 = 4 * src[8] - 9 * src[24] + 15 * src[40] - 16 * src[56];
    dst[0] = s16((t5 + t1) >> 3);
    dst[1] = s16((t6 + t2) >> 3);
    dst[2] = s16((t7 + t3) >> 3);
    dst[3] = s16((t8 + t4) >> 3);
    dst[4] = s16((t8 - t4) >> 3);
    dst[5] = s16((t7 - t3) >> 3);
    dst[6] = s16((t6 - t2) >> 3);
    dst[7] = s16((t5 - t1) >> 3);
    src += 1;
    dst += 8;
  }
  src = temp;
  dst = block;
  for (int i = 0; i < 8; ++i) {
    int t1 = 12 * (src[0] + src[32]) + 64;
    int t2 = 12 * (src[0] - src[32]) + 64;
    int t3 = 16 * src[16] + 6 * src[48];
    int t4 = 6 * src[16] - 16 * src[48];
    const int t5 = t1 + t3, t6 = t2 + t4, t7 = t2 - t4, t8 = t1 - t3;
    t1 = 16 * src[8] + 15 * src[24] + 9 * src[40] + 4 * src[56];
    t2 = 15 * src[8] - 4 * src[24] - 16 * src[40] - 9 * src[56];
    t3 = 9 * src[8] - 16 * src[24] + 4 * src[40] + 15 * src[56];
    t4 = 4 * src[8] - 9 * src[24] + 15 * src[40] - 16 * src[56];
    dst[0] = s16((t5 + t1) >> 7);
    dst[8] = s16((t6 + t2) >> 7);
    dst[16] = s16((t7 + t3) >> 7);
    dst[24] = s16((t8 + t4) >> 7);
    dst[32] = s16((t8 - t4 + 1) >> 7);
    dst[40] = s16((t7 - t3 + 1) >> 7);
    dst[48] = s16((t6 - t2 + 1) >> 7);
    dst[56] = s16((t5 - t1 + 1) >> 7);
    src += 1;
    dst += 1;
  }
}

namespace {
void add_pixel(ByteView plane, uint64_t index, int delta) { plane[index] = static_cast<uint8_t>(clip255(plane[index] + delta)); }
}  // namespace

// 8x4 transform of a pair of 4-row halves: `block` is the half's 32 coefficients; the result is added to the 8 x 4 samples at `offset`.
void inv_trans_8x4_add(ByteView plane, uint64_t offset, int stride, int16_t* block) {
  int16_t* src = block;
  int16_t* dst = block;
  for (int i = 0; i < 4; ++i) {
    int t1 = 12 * (src[0] + src[4]) + 4;
    int t2 = 12 * (src[0] - src[4]) + 4;
    int t3 = 16 * src[2] + 6 * src[6];
    int t4 = 6 * src[2] - 16 * src[6];
    const int t5 = t1 + t3, t6 = t2 + t4, t7 = t2 - t4, t8 = t1 - t3;
    t1 = 16 * src[1] + 15 * src[3] + 9 * src[5] + 4 * src[7];
    t2 = 15 * src[1] - 4 * src[3] - 16 * src[5] - 9 * src[7];
    t3 = 9 * src[1] - 16 * src[3] + 4 * src[5] + 15 * src[7];
    t4 = 4 * src[1] - 9 * src[3] + 15 * src[5] - 16 * src[7];
    dst[0] = s16((t5 + t1) >> 3);
    dst[1] = s16((t6 + t2) >> 3);
    dst[2] = s16((t7 + t3) >> 3);
    dst[3] = s16((t8 + t4) >> 3);
    dst[4] = s16((t8 - t4) >> 3);
    dst[5] = s16((t7 - t3) >> 3);
    dst[6] = s16((t6 - t2) >> 3);
    dst[7] = s16((t5 - t1) >> 3);
    src += 8;
    dst += 8;
  }
  src = block;
  const uint64_t st = static_cast<uint64_t>(stride);
  for (int i = 0; i < 8; ++i) {
    const int t1 = 17 * (src[0] + src[16]) + 64;
    const int t2 = 17 * (src[0] - src[16]) + 64;
    const int t3 = 22 * src[8] + 10 * src[24];
    const int t4 = 22 * src[24] - 10 * src[8];
    const uint64_t o = offset + static_cast<uint64_t>(i);
    add_pixel(plane, o + 0 * st, (t1 + t3) >> 7);
    add_pixel(plane, o + 1 * st, (t2 - t4) >> 7);
    add_pixel(plane, o + 2 * st, (t2 + t4) >> 7);
    add_pixel(plane, o + 3 * st, (t1 - t3) >> 7);
    src += 1;
  }
}

void inv_trans_4x8_add(ByteView plane, uint64_t offset, int stride, int16_t* block) {
  int16_t* src = block;
  int16_t* dst = block;
  for (int i = 0; i < 8; ++i) {
    const int t1 = 17 * (src[0] + src[2]) + 4;
    const int t2 = 17 * (src[0] - src[2]) + 4;
    const int t3 = 22 * src[1] + 10 * src[3];
    const int t4 = 22 * src[3] - 10 * src[1];
    dst[0] = s16((t1 + t3) >> 3);
    dst[1] = s16((t2 - t4) >> 3);
    dst[2] = s16((t2 + t4) >> 3);
    dst[3] = s16((t1 - t3) >> 3);
    src += 8;
    dst += 8;
  }
  src = block;
  const uint64_t st = static_cast<uint64_t>(stride);
  for (int i = 0; i < 4; ++i) {
    int t1 = 12 * (src[0] + src[32]) + 64;
    int t2 = 12 * (src[0] - src[32]) + 64;
    int t3 = 16 * src[16] + 6 * src[48];
    int t4 = 6 * src[16] - 16 * src[48];
    const int t5 = t1 + t3, t6 = t2 + t4, t7 = t2 - t4, t8 = t1 - t3;
    t1 = 16 * src[8] + 15 * src[24] + 9 * src[40] + 4 * src[56];
    t2 = 15 * src[8] - 4 * src[24] - 16 * src[40] - 9 * src[56];
    t3 = 9 * src[8] - 16 * src[24] + 4 * src[40] + 15 * src[56];
    t4 = 4 * src[8] - 9 * src[24] + 15 * src[40] - 16 * src[56];
    const uint64_t o = offset + static_cast<uint64_t>(i);
    add_pixel(plane, o + 0 * st, (t5 + t1) >> 7);
    add_pixel(plane, o + 1 * st, (t6 + t2) >> 7);
    add_pixel(plane, o + 2 * st, (t7 + t3) >> 7);
    add_pixel(plane, o + 3 * st, (t8 + t4) >> 7);
    add_pixel(plane, o + 4 * st, (t8 - t4 + 1) >> 7);
    add_pixel(plane, o + 5 * st, (t7 - t3 + 1) >> 7);
    add_pixel(plane, o + 6 * st, (t6 - t2 + 1) >> 7);
    add_pixel(plane, o + 7 * st, (t5 - t1 + 1) >> 7);
    src += 1;
  }
}

void inv_trans_4x4_add(ByteView plane, uint64_t offset, int stride, int16_t* block) {
  int16_t* src = block;
  int16_t* dst = block;
  for (int i = 0; i < 4; ++i) {
    const int t1 = 17 * (src[0] + src[2]) + 4;
    const int t2 = 17 * (src[0] - src[2]) + 4;
    const int t3 = 22 * src[1] + 10 * src[3];
    const int t4 = 22 * src[3] - 10 * src[1];
    dst[0] = s16((t1 + t3) >> 3);
    dst[1] = s16((t2 - t4) >> 3);
    dst[2] = s16((t2 + t4) >> 3);
    dst[3] = s16((t1 - t3) >> 3);
    src += 8;
    dst += 8;
  }
  src = block;
  const uint64_t st = static_cast<uint64_t>(stride);
  for (int i = 0; i < 4; ++i) {
    const int t1 = 17 * (src[0] + src[16]) + 64;
    const int t2 = 17 * (src[0] - src[16]) + 64;
    const int t3 = 22 * src[8] + 10 * src[24];
    const int t4 = 22 * src[24] - 10 * src[8];
    const uint64_t o = offset + static_cast<uint64_t>(i);
    add_pixel(plane, o + 0 * st, (t1 + t3) >> 7);
    add_pixel(plane, o + 1 * st, (t2 - t4) >> 7);
    add_pixel(plane, o + 2 * st, (t2 + t4) >> 7);
    add_pixel(plane, o + 3 * st, (t1 - t3) >> 7);
    src += 1;
  }
}

void add_block_clamped(ByteView plane, uint64_t offset, int stride, const int16_t* block) {
  for (int r = 0; r < 8; ++r)
    for (int c = 0; c < 8; ++c) add_pixel(plane, offset + static_cast<uint64_t>(r) * static_cast<uint64_t>(stride) + static_cast<uint64_t>(c), block[r * 8 + c]);
}

void put_block_clamped(ByteView plane, uint64_t offset, int stride, const int16_t* block, bool signed_put) {
  for (int r = 0; r < 8; ++r)
    for (int c = 0; c < 8; ++c) plane[offset + static_cast<uint64_t>(r) * static_cast<uint64_t>(stride) + static_cast<uint64_t>(c)] = static_cast<uint8_t>(clip255(block[r * 8 + c] + (signed_put ? 128 : 0)));
}

// ---- motion compensation -------------------------------------------------------------------------------------------------------------

namespace {
// How a reference plane is read, which decides what is outside it: the samples there repeat the edge ones of the frame, or of the field the line belongs to.
enum RefMode : int {
  kRefFrame,           // a progressive picture predicting from a progressive one
  kRefFrameOfFields,   // a progressive picture predicting from an interlaced one: each line from its own field
  kRefFieldOfFields,   // a field predicting from a field of an interlaced picture
  kRefFieldOfFrame,    // a field predicting from the lines of one parity of a progressive picture (the frame's edges)
};

// A reference plane with its real size: samples outside repeat the edge ones (of the frame, or of the field: see RefMode).
struct Ref {
  ConstByteView data;
  int stride = 0, width = 0;
  int rows = 0;                     // the lines of the frame, or of each field
  int mode = kRefFrame;
  int parity = 0;                   // the field read (field modes)
  ArrayView<const uint8_t> lut[2];  // intensity compensation of the reference samples: by field, or by the parity of the frame line (empty: none)
  int at(int x, int y) const {
    x = x < 0 ? 0 : x >= width ? width - 1 : x;
    auto clamp_rows = [&](int v) { return v < 0 ? 0 : v >= rows ? rows - 1 : v; };
    int row, sel;
    switch (mode) {
      case kRefFrameOfFields:
        row = 2 * clamp_rows(y >> 1) + (y & 1);
        sel = y & 1;
        break;
      case kRefFieldOfFields:
        row = 2 * clamp_rows(y) + parity;
        sel = parity;
        break;
      case kRefFieldOfFrame:
        row = clamp_rows(2 * y + parity);
        sel = parity;
        break;
      default:
        row = clamp_rows(y);
        sel = y & 1;
        break;
    }
    const int v = data[static_cast<uint64_t>(row) * static_cast<uint64_t>(stride) + static_cast<uint64_t>(x)];
    return lut[sel].empty() ? v : lut[sel][static_cast<uint64_t>(v)];
  }
};

int mspel_filter(const int* s, int mode, int r, int shift) {
  (void)shift;
  switch (mode) {
    case 0: return s[1];
    case 1: return (-4 * s[0] + 53 * s[1] + 18 * s[2] - 3 * s[3] + 32 - r) >> 6;
    case 2: return (-1 * s[0] + 9 * s[1] + 9 * s[2] - 1 * s[3] + 8 - r) >> 4;
    default: return (-3 * s[0] + 18 * s[1] + 53 * s[2] - 4 * s[3] + 32 - r) >> 6;
  }
}

// 16-bit variants of the two stage filter (the first stage keeps more bits).
int mspel_taps(const int* s, int mode) {
  switch (mode) {
    case 1: return -4 * s[0] + 53 * s[1] + 18 * s[2] - 3 * s[3];
    case 2: return -1 * s[0] + 9 * s[1] + 9 * s[2] - 1 * s[3];
    default: return -3 * s[0] + 18 * s[1] + 53 * s[2] - 4 * s[3];
  }
}

// 8.3.4.3 the bicubic interpolation of a w x h block whose top left sample is (x, y) in `ref`: hmode and vmode (0 to 3) are the quarter sample fractions.
// `avg`: the result is averaged with what is in the destination (the second prediction of a bidirectional block).
void mspel_mc(ByteView dst, uint64_t dst_offset, int dst_stride, const Ref& ref, int x, int y, int hmode, int vmode, int rnd, int w, int h, bool avg) {
  auto put = [&](uint64_t index, int value) {
    const int v = clip255(value);
    dst[index] = static_cast<uint8_t>(avg ? (dst[index] + v + 1) >> 1 : v);
  };
  const uint64_t ds = static_cast<uint64_t>(dst_stride);
  if (vmode) {
    if (hmode) {
      static const int shift_value[] = {0, 5, 1, 5};
      const int shift = (shift_value[hmode] + shift_value[vmode]) >> 1;
      int16_t tmp[19 * 16];
      const int r = (1 << (shift - 1)) + rnd - 1;
      const int tw = w + 3;
      for (int j = 0; j < h; ++j)
        for (int i = 0; i < tw; ++i) {
          int s[4];
          for (int k = 0; k < 4; ++k) s[k] = ref.at(x - 1 + i, y + j - 1 + k);
          tmp[j * tw + i] = s16((mspel_taps(s, vmode) + r) >> shift);
        }
      const int r2 = 64 - rnd;
      for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
          int s[4];
          for (int k = 0; k < 4; ++k) s[k] = tmp[j * tw + i + k];
          put(dst_offset + static_cast<uint64_t>(j) * ds + static_cast<uint64_t>(i), (mspel_taps(s, hmode) + r2) >> 7);
        }
      return;
    }
    const int r = 1 - rnd;
    for (int j = 0; j < h; ++j)
      for (int i = 0; i < w; ++i) {
        int s[4];
        for (int k = 0; k < 4; ++k) s[k] = ref.at(x + i, y + j - 1 + k);
        put(dst_offset + static_cast<uint64_t>(j) * ds + static_cast<uint64_t>(i), mspel_filter(s, vmode, r, 0));
      }
    return;
  }
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) {
      int s[4];
      for (int k = 0; k < 4; ++k) s[k] = ref.at(x + i - 1 + k, y + j);
      put(dst_offset + static_cast<uint64_t>(j) * ds + static_cast<uint64_t>(i), mspel_filter(s, hmode, rnd, 0));
    }
}

// The half sample (bilinear) prediction of a w x h block: dxy bit 0 is the horizontal half sample, bit 1 the vertical one. `rnd` selects the rounding down variants.
void hpel_mc(ByteView dst, uint64_t dst_offset, int dst_stride, const Ref& ref, int x, int y, int dxy, int rnd, int w, int h, bool avg) {
  const uint64_t ds = static_cast<uint64_t>(dst_stride);
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) {
      int v;
      const int a = ref.at(x + i, y + j);
      switch (dxy) {
        case 0: v = a; break;
        case 1: v = (a + ref.at(x + i + 1, y + j) + 1 - rnd) >> 1; break;
        case 2: v = (a + ref.at(x + i, y + j + 1) + 1 - rnd) >> 1; break;
        default: v = (a + ref.at(x + i + 1, y + j) + ref.at(x + i, y + j + 1) + ref.at(x + i + 1, y + j + 1) + 2 - rnd) >> 2; break;
      }
      const uint64_t index = dst_offset + static_cast<uint64_t>(j) * ds + static_cast<uint64_t>(i);
      dst[index] = static_cast<uint8_t>(avg ? (dst[index] + v + 1) >> 1 : v);
    }
}

// The chroma prediction: bilinear in eighths (fx, fy in 0..7, even). `rnd` selects the variant that rounds down (bias 28 instead of 32).
void chroma_mc(ByteView dst, uint64_t dst_offset, int dst_stride, const Ref& ref, int x, int y, int fx, int fy, int rnd, int size, bool avg) {
  const int A = (8 - fx) * (8 - fy), B = fx * (8 - fy), C = (8 - fx) * fy, D = fx * fy;
  const uint64_t ds = static_cast<uint64_t>(dst_stride);
  const int bias = rnd ? 28 : 32;
  for (int j = 0; j < size; ++j)
    for (int i = 0; i < size; ++i) {
      const int v = (A * ref.at(x + i, y + j) + B * ref.at(x + i + 1, y + j) + C * ref.at(x + i, y + j + 1) + D * ref.at(x + i + 1, y + j + 1) + bias) >> 6;
      const uint64_t index = dst_offset + static_cast<uint64_t>(j) * ds + static_cast<uint64_t>(i);
      dst[index] = static_cast<uint8_t>(avg ? (dst[index] + v + 1) >> 1 : v);
    }
}

int median4(int a, int b, int c, int d) {
  auto mn = [](int x, int y) { return x < y ? x : y; };
  auto mx = [](int x, int y) { return x > y ? x : y; };
  if (a < b) {
    if (c < d) return (mn(b, d) + mx(a, c)) / 2;
    return (mn(b, c) + mx(a, d)) / 2;
  }
  if (c < d) return (mn(a, d) + mx(b, c)) / 2;
  return (mn(a, c) + mx(b, d)) / 2;
}
int mid3(int a, int b, int c) {
  const int lo = a < b ? a : b, hi = a < b ? b : a;
  return c < lo ? lo : c > hi ? hi : c;
}
constexpr uint8_t kPopcount4[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};

// The chroma vector of a macroblock from the luma one (8.3.4.5): the quarter sample vector halved, a three quarter fraction rounded up.
void chroma_vector(int mx, int my, int* uvmx, int* uvmy) {
  *uvmx = (mx + ((mx & 3) == 3)) >> 1;
  *uvmy = (my + ((my & 3) == 3)) >> 1;
}
}  // namespace

// The picture (and field) a vector of direction `dir` refers to, and the intensity compensation it is read through. A field picture's vector refers to field
// `ref_field` of the forward or the backward anchor; the second field of a frame finds the field of the other parity in its own frame (the first field, as the
// second field's compensation of it has made it).
Core::RefSel Core::reference_for(int dir, int ref_field) const {
  RefSel s;
  const bool p = pic_.type == kPictureP;
  if (!field_mode_) {
    s.pic = p ? anchor_[1] : dir == 0 ? anchor_[0] : anchor_[1];
    s.ic = dir == 0 ? &ic_last_ : &ic_next_;
    if (range_scale_[dir & 1]) s.ic = &range_luts_[dir & 1];   // (range reduction, then the intensity compensation: see prepare_range_scaling)
    return s;
  }
  s.field = ref_field & 1;
  if (dir == 0) {
    if (second_field_ && ref_field != cur_field_) {
      s.pic = cur_;
      s.ic = ic_curr_is_aux_ ? &ic_aux_ : &ic_next_;
    } else {
      s.pic = p ? anchor_[1] : anchor_[0];
      s.ic = &ic_last_;
    }
  } else {
    s.pic = anchor_[1];
    s.ic = &ic_next_;
  }
  return s;
}

namespace {
// The reference plane c (0 luma) of `pic` for a picture being decoded as a field or as a frame (`field` the field read, -1 for a frame).
Ref make_ref(const Picture* pic, int c, int field, int ref_w, int ref_h) {
  Ref r;
  r.data = ConstByteView(pic->plane[c].data(), pic->plane[c].size());
  r.stride = pic->stride[c];
  const bool chroma = c > 0;
  r.width = chroma ? ref_w >> 1 : ref_w;
  if (field >= 0) {
    r.parity = field & 1;
    r.mode = pic->interlaced ? kRefFieldOfFields : kRefFieldOfFrame;
    r.rows = pic->interlaced ? (chroma ? ref_h >> 2 : ref_h >> 1) : (chroma ? ref_h >> 1 : ref_h);
  } else {
    r.mode = pic->interlaced ? kRefFrameOfFields : kRefFrame;
    r.rows = pic->interlaced ? (chroma ? ref_h >> 2 : ref_h >> 1) : (chroma ? ref_h >> 1 : ref_h);
  }
  return r;
}
}  // namespace

// The prediction of the whole macroblock (luma 16x16 and chroma 8x8) from `ref`; the vectors are the luma one and the chroma one, both already adjusted.
void Core::predict_mb(const RefSel& ref, int mx, int my, int uvmx, int uvmy, bool avg) {
  Ref y = make_ref(ref.pic, 0, ref.field, ref_w_, ref_h_);
  Ref u = make_ref(ref.pic, 1, ref.field, ref_w_, ref_h_);
  Ref v = make_ref(ref.pic, 2, ref.field, ref_w_, ref_h_);
  if (ref.ic && ref.ic->use) {
    for (int k = 0; k < 2; ++k) {
      y.lut[k] = ArrayView<const uint8_t>(ref.ic->y[k], 256);
      u.lut[k] = v.lut[k] = ArrayView<const uint8_t>(ref.ic->uv[k], 256);
    }
  }
  const bool adv = seq_.profile == kAdvanced;
  int src_x = mb_x_ * 16 + (mx >> 2), src_y = mb_y_ * 16 + (my >> 2);
  int uvsrc_x = mb_x_ * 8 + (uvmx >> 2), uvsrc_y = mb_y_ * 8 + (uvmy >> 2);
  if (!adv) {
    src_x = clip_int(src_x, -16, mb_w_ * 16);
    src_y = clip_int(src_y, -16, mb_h_ * 16);
    uvsrc_x = clip_int(uvsrc_x, -8, mb_w_ * 8);
    uvsrc_y = clip_int(uvsrc_y, -8, mb_h_ * 8);
  } else {
    src_x = clip_int(src_x, -17, seq_.width);
    uvsrc_x = clip_int(uvsrc_x, -8, seq_.width >> 1);
    if (frame_interlace()) {   // (an interlaced frame keeps the line parity: each line stays in its field)
      src_y = clip_int(src_y, -18 + (src_y & 1), seq_.height + (src_y & 1));
      uvsrc_y = clip_int(uvsrc_y, -8 + (uvsrc_y & 1), (seq_.height >> 1) + (uvsrc_y & 1));
    } else {
      src_y = clip_int(src_y, -18, seq_.height + 1);
      uvsrc_y = clip_int(uvsrc_y, -8, seq_.height >> 1);
    }
  }
  ByteView dy = dst_plane(0), du = dst_plane(1), dv = dst_plane(2);
  const uint64_t loff = dst_off_[0] + static_cast<uint64_t>(mb_y_ * 16) * static_cast<uint64_t>(dst_stride_[0]) + static_cast<uint64_t>(mb_x_ * 16);
  const uint64_t coff = dst_off_[1] + static_cast<uint64_t>(mb_y_ * 8) * static_cast<uint64_t>(dst_stride_[1]) + static_cast<uint64_t>(mb_x_ * 8);
  const int rnd = pic_.rnd;
  if (pic_.mspel) {
    mspel_mc(dy, loff, dst_stride_[0], y, src_x, src_y, mx & 3, my & 3, rnd, 16, 16, avg);
  } else {
    hpel_mc(dy, loff, dst_stride_[0], y, src_x, src_y, (my & 2) | ((mx & 2) >> 1), rnd, 16, 16, avg);
  }
  const int fx = (uvmx & 3) << 1, fy = (uvmy & 3) << 1;
  chroma_mc(du, coff, dst_stride_[1], u, uvsrc_x, uvsrc_y, fx, fy, rnd, 8, avg);
  chroma_mc(dv, coff, dst_stride_[2], v, uvsrc_x, uvsrc_y, fx, fy, rnd, 8, avg);
}

// One direction of a 1MV macroblock (also the prediction of the first direction of a bidirectional one).
void Core::mc_1mv(int dir) {
  const RefSel ref = reference_for(dir, ref_field_[dir & 1]);
  if (!ref.pic) return;
  const uint64_t mb_pos = static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_);
  int mx = mv_cur_[dir][0][0], my = mv_cur_[dir][0][1];
  if (pic_.type == kPictureP) {
    const uint64_t f = field_mb_index(mb_pos);
    cur_->direct_mv[f * 2] = s16(mx);
    cur_->direct_mv[f * 2 + 1] = s16(my);
    if (frame_interlace()) store_frame_direct_mv(2, mx, my);
  }
  int uvmx, uvmy;
  chroma_vector(mx, my, &uvmx, &uvmy);
  uvmv_[mb_pos * 2] = s16(uvmx);
  uvmv_[mb_pos * 2 + 1] = s16(uvmy);
  const bool opposite = field_mode_ && cur_field_ != ref_field_[dir & 1];
  if (opposite) {   // (the other field's lines are half a line up or down)
    my = my - 2 + 4 * cur_field_;
    uvmy = uvmy - 2 + 4 * cur_field_;
  }
  if (seq_.fastuvmc && !frame_interlace()) {   // (FASTUVMC does not apply to an interlaced frame's 1MV macroblocks, as in the reference decoder)
    uvmx = uvmx + ((uvmx < 0) ? (uvmx & 1) : -(uvmx & 1));
    uvmy = uvmy + ((uvmy < 0) ? (uvmy & 1) : -(uvmy & 1));
  }
  predict_mb(ref, mx, my, uvmx, uvmy, false);
  if (field_mode_ && dir == 0) uvmv_f_[mb_pos] = opposite ? 1 : 0;
}

// The luma block n of a 4MV macroblock, direction dir.
void Core::mc_4mv_luma(int n, int dir) {
  const RefSel ref = reference_for(dir, ref_field_[dir & 1]);
  if (!ref.pic) return;
  const int mx = mv_cur_[dir][n][0];
  int my = mv_cur_[dir][n][1];
  if (field_mode_ && cur_field_ != ref_field_[dir & 1]) my = my - 2 + 4 * cur_field_;
  Ref y = make_ref(ref.pic, 0, ref.field, ref_w_, ref_h_);
  if (ref.ic && ref.ic->use)
    for (int k = 0; k < 2; ++k) y.lut[k] = ArrayView<const uint8_t>(ref.ic->y[k], 256);
  int src_x = mb_x_ * 16 + (n & 1) * 8 + (mx >> 2);
  int src_y = mb_y_ * 16 + (n & 2) * 4 + (my >> 2);
  if (seq_.profile != kAdvanced) {
    src_x = clip_int(src_x, -16, mb_w_ * 16);
    src_y = clip_int(src_y, -16, mb_h_ * 16);
  } else {
    src_x = clip_int(src_x, -17, seq_.width);
    src_y = clip_int(src_y, -18, seq_.height + 1);
  }
  const uint64_t off = dst_off_[0] + static_cast<uint64_t>(mb_y_ * 16 + (n >> 1) * 8) * static_cast<uint64_t>(dst_stride_[0]) + static_cast<uint64_t>(mb_x_ * 16 + (n & 1) * 8);
  if (pic_.mspel) mspel_mc(dst_plane(0), off, dst_stride_[0], y, src_x, src_y, mx & 3, my & 3, pic_.rnd, 8, 8, false);
  else hpel_mc(dst_plane(0), off, dst_stride_[0], y, src_x, src_y, (my & 2) | ((mx & 2) >> 1), pic_.rnd, 8, 8, false);
}

// The chroma blocks of a 4MV macroblock: one vector derived from the luma vectors - of the blocks that are not intra, or (a field picture with two reference
// fields) of the blocks that refer to the field most of them refer to.
void Core::mc_4mv_chroma(int dir) {
  const uint64_t mb_pos = static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_);
  int mv[4][2];
  for (int k = 0; k < 4; ++k) {
    mv[k][0] = mv_cur_[dir][k][0];
    mv[k][1] = mv_cur_[dir][k][1];
  }
  int tx = 0, ty = 0, chroma_ref = 0;
  if (!field_mode_ || !pic_.numref) {
    static const uint8_t index2[16] = {0, 0, 0, 0x01, 0, 0x02, 0x12, 0, 0, 0x03, 0x13, 0, 0x23, 0, 0, 0};
    int idx = 0;
    for (int k = 0; k < 4; ++k) idx |= (((intra_mask_[mb_pos] >> k) & 1) ^ 1) << k;
    switch (kPopcount4[idx]) {
      case 4:
        tx = median4(mv[0][0], mv[1][0], mv[2][0], mv[3][0]);
        ty = median4(mv[0][1], mv[1][1], mv[2][1], mv[3][1]);
        break;
      case 3:
        tx = mid3(mv[idx > 0xd][0], mv[1 + (idx > 0xb)][0], mv[2 + (idx > 0x7)][0]);
        ty = mid3(mv[idx > 0xd][1], mv[1 + (idx > 0xb)][1], mv[2 + (idx > 0x7)][1]);
        break;
      case 2:
        tx = (mv[index2[idx] >> 4][0] + mv[index2[idx] & 0xf][0]) / 2;
        ty = (mv[index2[idx] >> 4][1] + mv[index2[idx] & 0xf][1]) / 2;
        break;
      default:
        if (pic_.type == kPictureP) {
          const uint64_t f = field_mb_index(mb_pos);
          cur_->direct_mv[f * 2] = cur_->direct_mv[f * 2 + 1] = 0;
        }
        uvmv_[mb_pos * 2] = uvmv_[mb_pos * 2 + 1] = 0;
        return;   // no chroma prediction for intra blocks
    }
    chroma_ref = ref_field_[dir & 1];
  } else {
    // The blocks' vectors that refer to the dominant field: all four (median), three (median of three) or two (their mean).
    static const uint8_t index2[16] = {0, 0, 0, 0x23, 0, 0x13, 0x03, 0, 0, 0x12, 0x02, 0, 0x01, 0, 0, 0};
    const int bw = 2 * mb_w_;
    int idx = 0;
    for (int k = 0; k < 4; ++k)
      idx |= mv_f_[dir & 1][static_cast<uint64_t>(2 * mb_y_ + (k >> 1)) * static_cast<uint64_t>(bw) + static_cast<uint64_t>(2 * mb_x_ + (k & 1))] << k;
    const int opp_count = kPopcount4[idx];
    switch (opp_count) {
      case 1:
        tx = mid3(mv[idx < 2][0], mv[1 + (idx < 4)][0], mv[2 + (idx < 8)][0]);
        ty = mid3(mv[idx < 2][1], mv[1 + (idx < 4)][1], mv[2 + (idx < 8)][1]);
        break;
      case 3:
        tx = mid3(mv[idx > 0xd][0], mv[1 + (idx > 0xb)][0], mv[2 + (idx > 0x7)][0]);
        ty = mid3(mv[idx > 0xd][1], mv[1 + (idx > 0xb)][1], mv[2 + (idx > 0x7)][1]);
        break;
      case 2:
        tx = (mv[index2[idx] >> 4][0] + mv[index2[idx] & 0xf][0]) / 2;
        ty = (mv[index2[idx] >> 4][1] + mv[index2[idx] & 0xf][1]) / 2;
        break;
      default:
        tx = median4(mv[0][0], mv[1][0], mv[2][0], mv[3][0]);
        ty = median4(mv[0][1], mv[1][1], mv[2][1], mv[3][1]);
        break;
    }
    chroma_ref = cur_field_ ^ (opp_count > 2 ? 1 : 0);
  }
  if (pic_.type == kPictureP) {
    const uint64_t f = field_mb_index(mb_pos);
    cur_->direct_mv[f * 2] = s16(tx);
    cur_->direct_mv[f * 2 + 1] = s16(ty);
  }
  const RefSel ref = reference_for(dir, chroma_ref);
  if (!ref.pic) return;
  int uvmx, uvmy;
  chroma_vector(tx, ty, &uvmx, &uvmy);
  uvmv_[mb_pos * 2] = s16(uvmx);
  uvmv_[mb_pos * 2 + 1] = s16(uvmy);
  if (seq_.fastuvmc) {
    uvmx = uvmx + ((uvmx < 0) ? (uvmx & 1) : -(uvmx & 1));
    uvmy = uvmy + ((uvmy < 0) ? (uvmy & 1) : -(uvmy & 1));
  }
  const bool opposite = field_mode_ && cur_field_ != chroma_ref;
  if (opposite) uvmy += 2 - 4 * chroma_ref;   // (after the rounding of FASTUVMC, unlike a 1MV macroblock)
  Ref u = make_ref(ref.pic, 1, ref.field, ref_w_, ref_h_);
  Ref v = make_ref(ref.pic, 2, ref.field, ref_w_, ref_h_);
  if (ref.ic && ref.ic->use)
    for (int k = 0; k < 2; ++k) u.lut[k] = v.lut[k] = ArrayView<const uint8_t>(ref.ic->uv[k], 256);
  int uvsrc_x = mb_x_ * 8 + (uvmx >> 2), uvsrc_y = mb_y_ * 8 + (uvmy >> 2);
  if (seq_.profile != kAdvanced) {
    uvsrc_x = clip_int(uvsrc_x, -8, mb_w_ * 8);
    uvsrc_y = clip_int(uvsrc_y, -8, mb_h_ * 8);
  } else {
    uvsrc_x = clip_int(uvsrc_x, -8, seq_.width >> 1);
    uvsrc_y = clip_int(uvsrc_y, -8, seq_.height >> 1);
  }
  const uint64_t coff = dst_off_[1] + static_cast<uint64_t>(mb_y_ * 8) * static_cast<uint64_t>(dst_stride_[1]) + static_cast<uint64_t>(mb_x_ * 8);
  const int fx = (uvmx & 3) << 1, fy = (uvmy & 3) << 1;
  chroma_mc(dst_plane(1), coff, dst_stride_[1], u, uvsrc_x, uvsrc_y, fx, fy, pic_.rnd, 8, false);
  chroma_mc(dst_plane(2), coff, dst_stride_[2], v, uvsrc_x, uvsrc_y, fx, fy, pic_.rnd, 8, false);
  if (field_mode_ && dir == 0) uvmv_f_[mb_pos] = opposite ? 1 : 0;
}

// An interlaced frame P picture keeps the vectors of its luma blocks 0 and 2 for direct mode in the B pictures that follow it (the second is a 2MV field
// macroblock's bottom field vector).
void Core::store_frame_direct_mv(int n, int mx, int my) {
  if (pic_.type != kPictureP) return;
  const uint64_t mb_pos = static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_);
  if (n == 0) {
    cur_->direct_mv[mb_pos * 2] = s16(mx);
    cur_->direct_mv[mb_pos * 2 + 1] = s16(my);
  } else if (n == 2 && cur_->direct_mv_bottom.size() >= (mb_pos + 1) * 2) {
    cur_->direct_mv_bottom[mb_pos * 2] = s16(mx);
    cur_->direct_mv_bottom[mb_pos * 2 + 1] = s16(my);
  }
}

// The luma block n of an interlaced frame macroblock with a vector per block or per field, direction dir (`avg`: averaged with the prediction already there, the
// second direction of an interpolated or direct macroblock). A field vector's block is a field's 8 lines (blocks 0 and 1 the top field, 2 and 3 the bottom one)
// predicted from the reference's lines of the parity the vector points to; the vector is pulled back to within a block of the picture first.
void Core::mc_4mv_luma_frame(int n, int dir, bool avg) {
  const RefSel ref = reference_for(dir, 0);
  if (!ref.pic) return;
  const int bw = 2 * mb_w_;
  const bool fieldmv = blk_mv_type_[static_cast<uint64_t>(2 * mb_y_ + (n >> 1)) * static_cast<uint64_t>(bw) + static_cast<uint64_t>(2 * mb_x_ + (n & 1))] != 0;
  int mx = mv_cur_[dir & 1][n][0], my = mv_cur_[dir & 1][n][1];
  store_frame_direct_mv(n, mx, my);
  {
    const int width = seq_.width, height = seq_.height >> 1;
    const int qx = mb_x_ * 16 + (mx >> 2), qy = mb_y_ * 8 + (my >> 3);
    if (qx < -17) mx -= 4 * (qx + 17);
    else if (qx > width) mx -= 4 * (qx - width);
    if (qy < -18) my -= 8 * (qy + 18);
    else if (qy > height + 1) my -= 8 * (qy - height - 1);
  }
  int src_x = mb_x_ * 16 + (n & 1) * 8 + (mx >> 2);
  int src_y = mb_y_ * 16 + (fieldmv ? (n > 1 ? 1 : 0) : (n & 2) * 4) + (my >> 2);
  src_x = clip_int(src_x, -17, seq_.width);
  src_y = clip_int(src_y, -18 + (src_y & 1), seq_.height + (src_y & 1));
  const int stride = dst_stride_[0];
  const int dst_row = mb_y_ * 16 + (fieldmv ? (n > 1 ? 1 : 0) : (n & 2) * 4);
  const uint64_t off = dst_off_[0] + static_cast<uint64_t>(dst_row) * static_cast<uint64_t>(stride) + static_cast<uint64_t>(mb_x_ * 16 + (n & 1) * 8);
  Ref y = make_ref(ref.pic, 0, fieldmv ? (src_y & 1) : -1, ref_w_, ref_h_);
  if (ref.ic && ref.ic->use)
    for (int k = 0; k < 2; ++k) y.lut[k] = ArrayView<const uint8_t>(ref.ic->y[k], 256);
  // (a field block reads the lines of one parity: its rows are counted in that field)
  const int ry = fieldmv ? (src_y >> 1) : src_y;
  mspel_mc(dst_plane(0), off, fieldmv ? stride * 2 : stride, y, src_x, ry, mx & 3, my & 3, pic_.rnd, 8, 8, avg);
}

// The chroma of an interlaced frame macroblock with vectors per block or per field: a 4x4 chroma block for each luma block (the upper two from direction dir,
// the lower two from dir2), each with the chroma vector of its luma block; with field vectors each is 4 lines of a field and the vertical vector is rounded
// for fields.
void Core::mc_4mv_chroma_frame(int dir, int dir2, bool avg) {
  static constexpr uint8_t kRoundField[16] = {0, 0, 1, 2, 4, 4, 5, 6, 2, 2, 3, 8, 6, 6, 7, 12};
  const int bw = 2 * mb_w_;
  const bool fieldmv = blk_mv_type_[static_cast<uint64_t>(2 * mb_y_) * static_cast<uint64_t>(bw) + static_cast<uint64_t>(2 * mb_x_)] != 0;
  const int v_dist = fieldmv ? 1 : 4;   // (where the lower blocks start)
  int uvmx[4], uvmy[4];
  for (int i = 0; i < 4; ++i) {
    const int d = (i < 2 ? dir : dir2) & 1;
    const int tx = mv_cur_[d][i][0], ty = mv_cur_[d][i][1];
    uvmx[i] = (tx + ((tx & 3) == 3)) >> 1;
    uvmy[i] = fieldmv ? (ty >> 4) * 8 + kRoundField[ty & 0xF] : (ty + ((ty & 3) == 3)) >> 1;
  }
  const int cstride = dst_stride_[1];
  for (int i = 0; i < 4; ++i) {
    const int d = i < 2 ? dir : dir2;
    const RefSel ref = reference_for(d, 0);
    if (!ref.pic) return;
    int uvsrc_x = mb_x_ * 8 + (i & 1) * 4 + (uvmx[i] >> 2);
    int uvsrc_y = mb_y_ * 8 + ((i & 2) ? v_dist : 0) + (uvmy[i] >> 2);
    uvsrc_x = clip_int(uvsrc_x, -8, seq_.width >> 1);
    uvsrc_y = clip_int(uvsrc_y, -8 + (uvsrc_y & 1), (seq_.height >> 1) + (uvsrc_y & 1));
    Ref u = make_ref(ref.pic, 1, fieldmv ? (uvsrc_y & 1) : -1, ref_w_, ref_h_);
    Ref v = make_ref(ref.pic, 2, fieldmv ? (uvsrc_y & 1) : -1, ref_w_, ref_h_);
    if (ref.ic && ref.ic->use)
      for (int k = 0; k < 2; ++k) u.lut[k] = v.lut[k] = ArrayView<const uint8_t>(ref.ic->uv[k], 256);
    const int ry = fieldmv ? (uvsrc_y >> 1) : uvsrc_y;
    const int dst_row = mb_y_ * 8 + ((i & 2) ? v_dist : 0);
    const uint64_t coff = dst_off_[1] + static_cast<uint64_t>(dst_row) * static_cast<uint64_t>(cstride) + static_cast<uint64_t>(mb_x_ * 8 + (i & 1) * 4);
    const int ds = fieldmv ? cstride * 2 : cstride;
    const int fx = (uvmx[i] & 3) << 1, fy = (uvmy[i] & 3) << 1;
    chroma_mc(dst_plane(1), coff, ds, u, uvsrc_x, ry, fx, fy, pic_.rnd, 4, avg);
    chroma_mc(dst_plane(2), coff, ds, v, uvsrc_x, ry, fx, fy, pic_.rnd, 4, avg);
  }
}

// The backward prediction of an interpolated (or direct) B macroblock, averaged with the forward one already in the picture.
void Core::interp_mc() {
  const RefSel ref = reference_for(1, ref_field_[1]);
  if (!ref.pic) return;
  const int mx = mv_cur_[1][0][0];
  int my = mv_cur_[1][0][1];
  int uvmx, uvmy;
  chroma_vector(mx, my, &uvmx, &uvmy);
  if (field_mode_ && cur_field_ != ref_field_[1]) {
    my = my - 2 + 4 * cur_field_;
    uvmy = uvmy - 2 + 4 * cur_field_;
  }
  if (seq_.fastuvmc) {   // (the reference decoders round this one away from zero, unlike the single direction case)
    uvmx = uvmx + ((uvmx < 0) ? -(uvmx & 1) : (uvmx & 1));
    uvmy = uvmy + ((uvmy < 0) ? -(uvmy & 1) : (uvmy & 1));
  }
  predict_mb(ref, mx, my, uvmx, uvmy, true);
}

void Core::b_mc(bool direct, int mode) {
  if (direct || mode == kBmvInterpolated) {
    mc_1mv(0);
    interp_mc();
    return;
  }
  mc_1mv(mode == kBmvBackward ? 1 : 0);
}

// ---- overlap smoothing ---------------------------------------------------------------------------------------------------------------

namespace {
// 8.6.? the smoothing of the two columns on each side of a vertical edge between two blocks of samples.
void h_overlap(int16_t* left, int16_t* right) {
  int rnd1 = 4, rnd2 = 3;
  for (int i = 0; i < 8; ++i) {
    const int a = left[i * 8 + 6], b = left[i * 8 + 7], c = right[i * 8], d = right[i * 8 + 1];
    const int d1 = a - d, d2 = a - d + b - c;
    left[i * 8 + 6] = s16(((a * 8) - d1 + rnd1) >> 3);
    left[i * 8 + 7] = s16(((b * 8) - d2 + rnd2) >> 3);
    right[i * 8] = s16(((c * 8) + d2 + rnd1) >> 3);
    right[i * 8 + 1] = s16(((d * 8) + d1 + rnd2) >> 3);
    rnd2 = 7 - rnd2;
    rnd1 = 7 - rnd1;
  }
}

// ... and of a horizontal edge.
void v_overlap(int16_t* top, int16_t* bottom) {
  int rnd1 = 4, rnd2 = 3;
  for (int i = 0; i < 8; ++i) {
    const int a = top[48 + i], b = top[56 + i], c = bottom[i], d = bottom[8 + i];
    const int d1 = a - d, d2 = a - d + b - c;
    top[48 + i] = s16(((a * 8) - d1 + rnd1) >> 3);
    top[56 + i] = s16(((b * 8) - d2 + rnd2) >> 3);
    bottom[i] = s16(((c * 8) + d2 + rnd1) >> 3);
    bottom[8 + i] = s16(((d * 8) + d1 + rnd2) >> 3);
    rnd2 = 7 - rnd2;
    rnd1 = 7 - rnd1;
  }
}
}  // namespace

int16_t* Core::mb_block(int mx, int my, int n) const { return &blocks_[((static_cast<uint64_t>(my) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mx)) * 6 + static_cast<uint64_t>(n)) * 64]; }

// The overlap smoothing of an I picture: the vertical edges of the whole picture first, then the horizontal ones.
void Core::overlap_filter_i() {
  const bool adv = seq_.profile == kAdvanced;
  auto flag = [&](int mx, int my) { return plane_overflags_[static_cast<uint64_t>(my) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mx)] != 0; };
  auto all = [&](int mx, int my, int i, bool horizontal) {
    if (pic_.pq >= 9) return true;
    if (!adv) return false;
    if (pic_.condover == kCondOverAll) return true;
    if (pic_.condover != kCondOverSelect) return false;
    if (!flag(mx, my)) return false;
    return horizontal ? ((i & 5) == 1 || flag(mx - 1, my)) : ((i & 2) != 0 || flag(mx, my - 1));
  };
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx)
      for (int i = 0; i < 6; ++i) {
        if (mx == 0 && (i & 5) != 1) continue;
        if (!all(mx, my, i, true)) continue;
        int16_t* cur = mb_block(mx, my, i);
        int16_t* left = nullptr;
        switch (i) {
          case 0: left = mb_block(mx - 1, my, 1); break;
          case 1: left = mb_block(mx, my, 0); break;
          case 2: left = mb_block(mx - 1, my, 3); break;
          case 3: left = mb_block(mx, my, 2); break;
          default: left = mb_block(mx - 1, my, i); break;
        }
        h_overlap(left, cur);
      }
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx)
      for (int i = 0; i < 6; ++i) {
        const bool slice_first = my == 0 || slice_start_row_[static_cast<uint64_t>(my)] != 0;
        if (slice_first && !(i & 2)) continue;
        if (!all(mx, my, i, false)) continue;
        int16_t* cur = mb_block(mx, my, i);
        int16_t* top = nullptr;
        switch (i) {
          case 0: top = mb_block(mx, my - 1, 2); break;
          case 1: top = mb_block(mx, my - 1, 3); break;
          case 2: top = mb_block(mx, my, 0); break;
          case 3: top = mb_block(mx, my, 1); break;
          default: top = mb_block(mx, my - 1, i); break;
        }
        v_overlap(top, cur);
      }
}

// The same for the intra macroblocks of a P picture (only edges between intra blocks).
void Core::overlap_filter_p() {
  auto intra = [&](int mx, int my, int n) { return ((intra_mask_[static_cast<uint64_t>(my) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mx)] >> n) & 1) != 0; };
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx)
      for (int i = 0; i < 6; ++i) {
        if (mx == 0 && (i & 5) != 1) continue;
        int lmx = mx, ln = i;
        switch (i) {
          case 0: lmx = mx - 1; ln = 1; break;
          case 1: ln = 0; break;
          case 2: lmx = mx - 1; ln = 3; break;
          case 3: ln = 2; break;
          default: lmx = mx - 1; break;
        }
        if (!intra(mx, my, i) || !intra(lmx, my, ln)) continue;
        h_overlap(mb_block(lmx, my, ln), mb_block(mx, my, i));
      }
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx)
      for (int i = 0; i < 6; ++i) {
        const bool slice_first = my == 0 || slice_start_row_[static_cast<uint64_t>(my)] != 0;
        if (slice_first && !(i & 2)) continue;
        int tmy = my, tn = i;
        switch (i) {
          case 0: tmy = my - 1; tn = 2; break;
          case 1: tmy = my - 1; tn = 3; break;
          case 2: tn = 0; break;
          case 3: tn = 1; break;
          default: tmy = my - 1; break;
        }
        if (!intra(mx, my, i) || !intra(mx, tmy, tn)) continue;
        v_overlap(mb_block(mx, tmy, tn), mb_block(mx, my, i));
      }
}

void Core::put_intra_blocks(bool signed_put) {
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      const uint64_t mb_pos = static_cast<uint64_t>(my) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mx);
      for (int n = 0; n < 6; ++n) {
        if (!((intra_mask_[mb_pos] >> n) & 1)) continue;
        int plane, stride;
        uint64_t offset;
        const int sx = mb_x_, sy = mb_y_;
        mb_x_ = mx;
        mb_y_ = my;
        block_target(n, &plane, &offset, &stride);
        mb_x_ = sx;
        mb_y_ = sy;
        put_block_clamped(ByteView(cur_->plane[plane].data(), cur_->plane[plane].size()), offset, stride, mb_block(mx, my, n), signed_put);
      }
    }
}

// ---- the in-loop filter --------------------------------------------------------------------------------------------------------------

namespace {
// 8.6.2 the filter of one line of 4 pixel pairs' edge: returns whether the other three lines of the segment are filtered.
bool filter_line(ByteView p, uint64_t at, int64_t step, int pq) {
  auto px = [&](int k) -> int { return p[static_cast<uint64_t>(static_cast<int64_t>(at) + step * k)]; };
  int a0 = (2 * (px(-2) - px(1)) - 5 * (px(-1) - px(0)) + 4) >> 3;
  const int a0_sign = a0 >> 31;
  a0 = (a0 ^ a0_sign) - a0_sign;
  if (a0 < pq) {
    const int a1 = iabs((2 * (px(-4) - px(-1)) - 5 * (px(-3) - px(-2)) + 4) >> 3);
    const int a2 = iabs((2 * (px(0) - px(3)) - 5 * (px(1) - px(2)) + 4) >> 3);
    if (a1 < a0 || a2 < a0) {
      int clip = px(-1) - px(0);
      const int clip_sign = clip >> 31;
      clip = ((clip ^ clip_sign) - clip_sign) >> 1;
      if (clip) {
        const int a3 = a1 < a2 ? a1 : a2;
        int d = (5 * (a0 - a3)) >> 3;
        if (a0_sign ^ clip_sign) {
          d = d < clip ? d : clip;
          d = (d ^ clip_sign) - clip_sign;
          p[static_cast<uint64_t>(static_cast<int64_t>(at) - step)] = static_cast<uint8_t>(clip255(px(-1) - d));
          p[at] = static_cast<uint8_t>(clip255(px(0) + d));
        }
        return true;
      }
    }
  }
  return false;
}

// Filters `len` pixels along an edge: `along` is the distance between the pixels of the edge, `across` between the pixels perpendicular to it.
void filter_edge(ByteView p, uint64_t at, int64_t along, int64_t across, int len, int pq) {
  for (int i = 0; i < len; i += 4) {
    const uint64_t base = static_cast<uint64_t>(static_cast<int64_t>(at) + along * i);
    const bool filt3 = filter_line(p, static_cast<uint64_t>(static_cast<int64_t>(base) + along * 2), across, pq);
    if (filt3) {
      filter_line(p, base, across, pq);
      filter_line(p, static_cast<uint64_t>(static_cast<int64_t>(base) + along), across, pq);
      filter_line(p, static_cast<uint64_t>(static_cast<int64_t>(base) + along * 3), across, pq);
    }
  }
}
}  // namespace

// The in-loop filter of I and B pictures: every 8x8 block edge inside the picture (and slice).
void Core::loop_filter_i() {
  const int pq = pic_.pq;
  ByteView planes[3] = {ByteView(cur_->plane[0].data(), cur_->plane[0].size()), ByteView(cur_->plane[1].data(), cur_->plane[1].size()), ByteView(cur_->plane[2].data(), cur_->plane[2].size())};
  const int ls = dst_stride_[0], cs = dst_stride_[1];
  // The edges between rows (the vertical filter) of the whole picture first, then the edges between columns.
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      const bool top = my == 0 || slice_start_row_[static_cast<uint64_t>(my)] != 0;
      const uint64_t lo = dst_off_[0] + static_cast<uint64_t>(my * 16) * static_cast<uint64_t>(ls) + static_cast<uint64_t>(mx * 16);
      const uint64_t co = dst_off_[1] + static_cast<uint64_t>(my * 8) * static_cast<uint64_t>(cs) + static_cast<uint64_t>(mx * 8);
      if (!top) {
        filter_edge(planes[0], lo, 1, ls, 16, pq);
        filter_edge(planes[1], co, 1, cs, 8, pq);
        filter_edge(planes[2], co, 1, cs, 8, pq);
      }
      filter_edge(planes[0], lo + static_cast<uint64_t>(8 * ls), 1, ls, 16, pq);
    }
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      const uint64_t lo = dst_off_[0] + static_cast<uint64_t>(my * 16) * static_cast<uint64_t>(ls) + static_cast<uint64_t>(mx * 16);
      const uint64_t co = dst_off_[1] + static_cast<uint64_t>(my * 8) * static_cast<uint64_t>(cs) + static_cast<uint64_t>(mx * 8);
      if (mx > 0) {
        filter_edge(planes[0], lo, ls, 1, 16, pq);
        filter_edge(planes[1], co, cs, 1, 8, pq);
        filter_edge(planes[2], co, cs, 1, 8, pq);
      }
      filter_edge(planes[0], lo + 8, ls, 1, 16, pq);
    }
}

// The in-loop filter of P pictures: block edges where either side is intra, the vectors differ or a coefficient is coded, and the edges inside blocks with 4x4 parts.
void Core::loop_filter_p() {
  const int pq = pic_.pq;
  ByteView planes[3] = {ByteView(cur_->plane[0].data(), cur_->plane[0].size()), ByteView(cur_->plane[1].data(), cur_->plane[1].size()), ByteView(cur_->plane[2].data(), cur_->plane[2].size())};
  const int bw = 2 * mb_w_;
  auto mb_index = [&](int mx, int my) { return static_cast<uint64_t>(my) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mx); };
  auto blk_mv = [&](int x, int y, int comp) -> int { return mv_[(static_cast<uint64_t>(y) * static_cast<uint64_t>(bw) + static_cast<uint64_t>(x)) * 2 + static_cast<uint64_t>(comp)]; };
  // (a field picture: vectors that refer to different fields differ too)
  auto blk_f = [&](int x, int y) -> int { return field_mode_ ? mv_f_[0][static_cast<uint64_t>(y) * static_cast<uint64_t>(bw) + static_cast<uint64_t>(x)] : 0; };
  auto mb_f = [&](uint64_t m) -> int { return field_mode_ ? uvmv_f_[m] : 0; };
  // Where block n of macroblock (mx, my) starts in its plane, and the plane's stride.
  auto where = [&](int mx, int my, int n, int* plane, int* stride) -> uint64_t {
    if (n < 4) {
      *plane = 0;
      *stride = dst_stride_[0];
      return dst_off_[0] + static_cast<uint64_t>(my * 16 + (n >> 1) * 8) * static_cast<uint64_t>(*stride) + static_cast<uint64_t>(mx * 16 + (n & 1) * 8);
    }
    *plane = n - 3;
    *stride = dst_stride_[*plane];
    return dst_off_[*plane] + static_cast<uint64_t>(my * 8) * static_cast<uint64_t>(*stride) + static_cast<uint64_t>(mx * 8);
  };
  // The edges at the bottom of the blocks (and the 4x4 edges inside them), the whole picture first.
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      const uint64_t m = mb_index(mx, my);
      const bool last_row_of_slice = my == mb_rows_ - 1 || slice_start_row_[static_cast<uint64_t>(my + 1)] != 0;
      for (int n = 0; n < 6; ++n) {
        int plane, stride;
        const uint64_t dst = where(mx, my, n, &plane, &stride);
        ByteView p = planes[plane];
        const uint32_t top_cbp = cbp_blocks_[m] >> (n * 4);
        if (!last_row_of_slice || n < 2) {
          uint32_t bottom_cbp;
          bool bottom_intra;
          bool differ;
          const bool top_intra = (intra_mask_[m] >> n) & 1;
          if (n > 3) {
            const uint64_t b = mb_index(mx, my + 1);
            bottom_intra = (intra_mask_[b] >> n) & 1;
            bottom_cbp = cbp_blocks_[b] >> (n * 4);
            differ = uvmv_[m * 2] != uvmv_[b * 2] || uvmv_[m * 2 + 1] != uvmv_[b * 2 + 1] || mb_f(m) != mb_f(b);
          } else if (n < 2) {
            bottom_intra = (intra_mask_[m] >> (n + 2)) & 1;
            bottom_cbp = cbp_blocks_[m] >> ((n + 2) * 4);
            const int x = 2 * mx + (n & 1), y = 2 * my;
            differ = blk_mv(x, y, 0) != blk_mv(x, y + 1, 0) || blk_mv(x, y, 1) != blk_mv(x, y + 1, 1) || blk_f(x, y) != blk_f(x, y + 1);
          } else {
            const uint64_t b = mb_index(mx, my + 1);
            bottom_intra = (intra_mask_[b] >> (n - 2)) & 1;
            bottom_cbp = cbp_blocks_[b] >> ((n - 2) * 4);
            const int x = 2 * mx + (n & 1), y = 2 * my + 1;
            differ = blk_mv(x, y, 0) != blk_mv(x, y + 1, 0) || blk_mv(x, y, 1) != blk_mv(x, y + 1, 1) || blk_f(x, y) != blk_f(x, y + 1);
          }
          const uint64_t edge = dst + static_cast<uint64_t>(8) * static_cast<uint64_t>(stride);
          if (top_intra || bottom_intra || differ) {
            filter_edge(p, edge, 1, stride, 8, pq);
          } else {
            const uint32_t idx = (top_cbp | (bottom_cbp >> 2)) & 3;
            if (idx & 1) filter_edge(p, edge + 4, 1, stride, 4, pq);
            if (idx & 2) filter_edge(p, edge, 1, stride, 4, pq);
          }
        }
        const uint32_t tt = (ttblk_[m] >> (n * 4)) & 0xF;
        if (tt == kTt4x4 || tt == kTt8x4) {
          if (top_cbp & 5) filter_edge(p, dst + static_cast<uint64_t>(4) * static_cast<uint64_t>(stride) + 4, 1, stride, 4, pq);
          if (top_cbp & 10) filter_edge(p, dst + static_cast<uint64_t>(4) * static_cast<uint64_t>(stride), 1, stride, 4, pq);
        }
      }
    }
  // Then the edges at the right of the blocks (and the 4x4 edges inside them).
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      const uint64_t m = mb_index(mx, my);
      const bool last_column = mx == mb_w_ - 1;
      for (int n = 0; n < 6; ++n) {
        int plane, stride;
        const uint64_t dst = where(mx, my, n, &plane, &stride);
        ByteView p = planes[plane];
        const uint32_t left_cbp = cbp_blocks_[m] >> (n * 4);
        if (!last_column || !(n & 5)) {
          uint32_t right_cbp;
          bool right_intra;
          bool differ;
          const bool left_intra = (intra_mask_[m] >> n) & 1;
          if (n > 3) {
            const uint64_t r = mb_index(mx + 1, my);
            right_intra = (intra_mask_[r] >> n) & 1;
            right_cbp = cbp_blocks_[r] >> (n * 4);
            differ = uvmv_[m * 2] != uvmv_[r * 2] || uvmv_[m * 2 + 1] != uvmv_[r * 2 + 1] || mb_f(m) != mb_f(r);
          } else if (n & 1) {
            const uint64_t r = mb_index(mx + 1, my);
            right_intra = (intra_mask_[r] >> (n - 1)) & 1;
            right_cbp = cbp_blocks_[r] >> ((n - 1) * 4);
            const int x = 2 * mx + 1, y = 2 * my + (n >> 1);
            differ = blk_mv(x, y, 0) != blk_mv(x + 1, y, 0) || blk_mv(x, y, 1) != blk_mv(x + 1, y, 1) || blk_f(x, y) != blk_f(x + 1, y);
          } else {
            right_intra = (intra_mask_[m] >> (n + 1)) & 1;
            right_cbp = cbp_blocks_[m] >> ((n + 1) * 4);
            const int x = 2 * mx, y = 2 * my + (n >> 1);
            differ = blk_mv(x, y, 0) != blk_mv(x + 1, y, 0) || blk_mv(x, y, 1) != blk_mv(x + 1, y, 1) || blk_f(x, y) != blk_f(x + 1, y);
          }
          const uint64_t edge = dst + 8;
          if (left_intra || right_intra || differ) {
            filter_edge(p, edge, stride, 1, 8, pq);
          } else {
            const uint32_t idx = (left_cbp | (right_cbp >> 1)) & 5;
            if (idx & 1) filter_edge(p, edge + static_cast<uint64_t>(4) * static_cast<uint64_t>(stride), stride, 1, 4, pq);
            if (idx & 4) filter_edge(p, edge, stride, 1, 4, pq);
          }
        }
        const uint32_t tt = (ttblk_[m] >> (n * 4)) & 0xF;
        if (tt == kTt4x4 || tt == kTt4x8) {
          if (left_cbp & 3) filter_edge(p, dst + static_cast<uint64_t>(4) * static_cast<uint64_t>(stride) + 4, stride, 1, 4, pq);
          if (left_cbp & 12) filter_edge(p, dst + 4, stride, 1, 4, pq);
        }
      }
    }
}

// What follows the macroblocks of a picture: overlap smoothing, the intra samples, the in-loop filter.
void Core::finish_intra_and_filters() {
  const bool adv = seq_.profile == kAdvanced;
  switch (pic_.type) {
    case kPictureI:
    case kPictureBI: {
      const bool overlap = seq_.overlap && (adv ? (pic_.pq >= 9 || pic_.condover != kCondOverNone) : pic_.pq >= 9);
      if (overlap) {
        if (frame_interlace()) overlap_filter_frame(true);
        else overlap_filter_i();
      }
      put_intra_blocks(adv || (seq_.overlap && pic_.pq >= 9));
      if (seq_.loop_filter) {
        if (frame_interlace()) loop_filter_i_frame();
        else loop_filter_i();
      }
      break;
    }
    case kPictureP:
      if (seq_.overlap && pic_.pq >= 9) {
        if (frame_interlace()) overlap_filter_frame(false);
        else overlap_filter_p();
      }
      put_intra_blocks(true);
      if (seq_.loop_filter) {
        if (frame_interlace()) loop_filter_inter_frame();
        else loop_filter_p();
      }
      break;
    default:
      if (seq_.loop_filter) {
        if (field_mode_) loop_filter_b_field();
        else if (frame_interlace()) loop_filter_inter_frame();
        else loop_filter_i();
      }
      break;
  }
}

// The in-loop filter of B field pictures: every 8x8 block edge inside the field (and slice), and the edges inside the blocks with 4x4, 8x4 or 4x8 parts that are
// coded.
void Core::loop_filter_b_field() {
  const int pq = pic_.pq;
  ByteView planes[3] = {dst_plane(0), dst_plane(1), dst_plane(2)};
  auto where = [&](int mx, int my, int n, int* plane, int* stride) -> uint64_t {
    if (n < 4) {
      *plane = 0;
      *stride = dst_stride_[0];
      return dst_off_[0] + static_cast<uint64_t>(my * 16 + (n >> 1) * 8) * static_cast<uint64_t>(*stride) + static_cast<uint64_t>(mx * 16 + (n & 1) * 8);
    }
    *plane = n - 3;
    *stride = dst_stride_[*plane];
    return dst_off_[*plane] + static_cast<uint64_t>(my * 8) * static_cast<uint64_t>(*stride) + static_cast<uint64_t>(mx * 8);
  };
  // The edges at the bottom of the blocks (and inside them), the whole field first; then the edges at their right.
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      const uint64_t m = static_cast<uint64_t>(my) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mx);
      const bool last_row_of_slice = my == mb_rows_ - 1 || slice_start_row_[static_cast<uint64_t>(my + 1)] != 0;
      for (int n = 0; n < 6; ++n) {
        int plane, stride;
        const uint64_t dst = where(mx, my, n, &plane, &stride);
        const uint64_t st = static_cast<uint64_t>(stride);
        if (!last_row_of_slice || n < 2) filter_edge(planes[plane], dst + 8 * st, 1, stride, 8, pq);
        const uint32_t cbp = cbp_blocks_[m] >> (n * 4);
        const uint32_t tt = (ttblk_[m] >> (n * 4)) & 0xF;
        if (tt == kTt4x4 || tt == kTt8x4) {
          const uint32_t idx = (cbp | (cbp >> 2)) & 3;
          if (idx & 1) filter_edge(planes[plane], dst + 4 * st + 4, 1, stride, 4, pq);
          if (idx & 2) filter_edge(planes[plane], dst + 4 * st, 1, stride, 4, pq);
        }
      }
    }
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      const uint64_t m = static_cast<uint64_t>(my) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mx);
      const bool last_column = mx == mb_w_ - 1;
      for (int n = 0; n < 6; ++n) {
        int plane, stride;
        const uint64_t dst = where(mx, my, n, &plane, &stride);
        const uint64_t st = static_cast<uint64_t>(stride);
        if (!last_column || !(n & 5)) filter_edge(planes[plane], dst + 8, stride, 1, 8, pq);
        const uint32_t cbp = cbp_blocks_[m] >> (n * 4);
        const uint32_t tt = (ttblk_[m] >> (n * 4)) & 0xF;
        if (tt == kTt4x4 || tt == kTt4x8) {
          const uint32_t idx = (cbp | (cbp >> 1)) & 5;
          if (idx & 1) filter_edge(planes[plane], dst + 4 * st + 4, stride, 1, 4, pq);
          if (idx & 4) filter_edge(planes[plane], dst + 4, stride, 1, 4, pq);
        }
      }
    }
}

// ---- interlaced frame pictures: overlap smoothing and the in-loop filter -------------------------------------------------------------------

// The overlap smoothing of an interlaced frame picture: only the vertical edges (the edges between rows would mix the fields), line by line of the frame - a
// macroblock with FIELDTX has its lines in its blocks field by field - with the rounding alternating by the line's parity. In an I picture the edges CONDOVER
// and OVERFLAGS select, in a P picture the edges between intra macroblocks.
void Core::overlap_filter_frame(bool intra_picture) {
  const bool adv = seq_.profile == kAdvanced;
  auto mb_index = [&](int mx, int my) { return static_cast<uint64_t>(my) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mx); };
  auto flag = [&](int mx, int my) { return plane_overflags_[mb_index(mx, my)] != 0; };
  // Whether the edge left of the macroblock (`internal`: the one inside it) is smoothed.
  auto smoothed = [&](int mx, int my, bool internal, int luma_bit) -> bool {
    if (intra_picture) {
      if (pic_.pq >= 9) return true;
      if (!adv) return false;
      if (pic_.condover == kCondOverAll) return true;
      if (pic_.condover != kCondOverSelect || !flag(mx, my)) return false;
      return internal || flag(mx - 1, my);
    }
    const uint8_t cur = intra_mask_[mb_index(mx, my)];
    if (internal) return (cur & 1) && (cur & 2);
    return ((cur >> luma_bit) & 1) && ((intra_mask_[mb_index(mx - 1, my)] >> (luma_bit < 4 ? luma_bit + 1 : luma_bit)) & 1);
  };
  // Line r (0..15), column c (0..15) of the macroblock's luma samples, wherever its blocks keep it.
  auto luma = [&](int mx, int my, int r, int c) -> int16_t& {
    const bool fields = mb_fieldtx(mx, my);
    const int n = (c >> 3) + 2 * (fields ? (r & 1) : (r >> 3));
    const int row = fields ? (r >> 1) : (r & 7);
    return blocks_[(mb_index(mx, my) * 6 + static_cast<uint64_t>(n)) * 64 + static_cast<uint64_t>(row * 8 + (c & 7))];
  };
  auto chroma = [&](int mx, int my, int n, int r, int c) -> int16_t& {
    return blocks_[(mb_index(mx, my) * 6 + static_cast<uint64_t>(n)) * 64 + static_cast<uint64_t>(r * 8 + c)];
  };
  // The smoothing of one line across an edge: the two samples on each side.
  auto smooth = [](int16_t& a, int16_t& b, int16_t& c, int16_t& d, int r) {
    const int rnd1 = (r & 1) ? 3 : 4, rnd2 = 7 - rnd1;
    const int va = a, vb = b, vc = c, vd = d;
    const int d1 = va - vd, d2 = va - vd + vb - vc;
    a = s16(((va * 8) - d1 + rnd1) >> 3);
    b = s16(((vb * 8) - d2 + rnd2) >> 3);
    c = s16(((vc * 8) + d2 + rnd1) >> 3);
    d = s16(((vd * 8) + d1 + rnd2) >> 3);
  };
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      if (mx > 0) {
        for (int r = 0; r < 16; ++r) {
          // (the reference decoder decides the upper and the lower half of the lines by the blocks 0 and 2: the same for a whole intra macroblock)
          if (!smoothed(mx, my, false, r < 8 ? 0 : 2)) continue;
          smooth(luma(mx - 1, my, r, 14), luma(mx - 1, my, r, 15), luma(mx, my, r, 0), luma(mx, my, r, 1), r);
        }
        for (int n = 4; n < 6; ++n) {
          if (!smoothed(mx, my, false, n)) continue;
          for (int r = 0; r < 8; ++r) smooth(chroma(mx - 1, my, n, r, 6), chroma(mx - 1, my, n, r, 7), chroma(mx, my, n, r, 0), chroma(mx, my, n, r, 1), r);
        }
      }
      if (smoothed(mx, my, true, 0))
        for (int r = 0; r < 16; ++r) smooth(luma(mx, my, r, 6), luma(mx, my, r, 7), luma(mx, my, r, 8), luma(mx, my, r, 9), r);
    }
}

// The in-loop filter of interlaced frame I and BI pictures: every block edge, field by field (each filter takes the lines of one parity); a macroblock with
// FIELDTX has no edge between its upper and lower blocks. The edges between rows of the whole picture first, then those between columns.
void Core::loop_filter_i_frame() {
  const int pq = pic_.pq;
  ByteView planes[3] = {dst_plane(0), dst_plane(1), dst_plane(2)};
  const int64_t ls = dst_stride_[0], cs = dst_stride_[1];
  const uint64_t uls = static_cast<uint64_t>(ls), ucs = static_cast<uint64_t>(cs);
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      const bool top = my == 0 || slice_start_row_[static_cast<uint64_t>(my)] != 0;
      const uint64_t lo = dst_off_[0] + static_cast<uint64_t>(my * 16) * uls + static_cast<uint64_t>(mx * 16);
      const uint64_t co = dst_off_[1] + static_cast<uint64_t>(my * 8) * ucs + static_cast<uint64_t>(mx * 8);
      if (!top)
        for (int f = 0; f < 2; ++f) filter_edge(planes[0], lo + static_cast<uint64_t>(f) * uls, 1, 2 * ls, 16, pq);
      if (!mb_fieldtx(mx, my))
        for (int f = 0; f < 2; ++f) filter_edge(planes[0], lo + static_cast<uint64_t>(8 + f) * uls, 1, 2 * ls, 16, pq);
      if (!top)
        for (int c = 1; c < 3; ++c)
          for (int f = 0; f < 2; ++f) filter_edge(planes[c], co + static_cast<uint64_t>(f) * ucs, 1, 2 * cs, 8, pq);
    }
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      const uint64_t lo = dst_off_[0] + static_cast<uint64_t>(my * 16) * uls + static_cast<uint64_t>(mx * 16);
      const uint64_t co = dst_off_[1] + static_cast<uint64_t>(my * 8) * ucs + static_cast<uint64_t>(mx * 8);
      if (mx > 0)
        for (int f = 0; f < 2; ++f) filter_edge(planes[0], lo + static_cast<uint64_t>(f) * uls, 2 * ls, 1, 8, pq);
      for (int f = 0; f < 2; ++f) filter_edge(planes[0], lo + static_cast<uint64_t>(f) * uls + 8, 2 * ls, 1, 8, pq);
      if (mx > 0)
        for (int c = 1; c < 3; ++c)
          for (int f = 0; f < 2; ++f) filter_edge(planes[c], co + static_cast<uint64_t>(f) * ucs, 2 * cs, 1, 4, pq);
    }
}

// The in-loop filter of interlaced frame P and B pictures, as the reference decoder has it: field by field, every edge at the bottom and the right of the blocks
// (not the picture's or the slice's last ones) whatever the blocks are, and the edges inside the blocks that have 8x4, 4x8 or 4x4 transforms - except the
// horizontal ones inside the upper blocks of a slice's first row and of the lower blocks of its last row (not FIELDTX macroblocks). The edges between rows of the
// whole picture first, then those between columns.
void Core::loop_filter_inter_frame() {
  const int pq = pic_.pq;
  ByteView planes[3] = {dst_plane(0), dst_plane(1), dst_plane(2)};
  const int64_t ls = dst_stride_[0], cs = dst_stride_[1];
  const uint64_t uls = static_cast<uint64_t>(ls), ucs = static_cast<uint64_t>(cs);
  auto tt_of = [&](uint64_t m, int n) { return static_cast<int>((ttblk_[m] >> (n * 4)) & 0xF); };
  // A filter of `len` samples across rows (the edge above line `row` of the block at `at`), field by field.
  auto rows = [&](ByteView p, uint64_t at, int row, int64_t stride, int len) { filter_edge(p, at + static_cast<uint64_t>(row) * static_cast<uint64_t>(stride), 1, 2 * stride, len, pq); };
  auto cols = [&](ByteView p, uint64_t at, int64_t stride, int len) { filter_edge(p, at, 2 * stride, 1, len, pq); };
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      const uint64_t m = static_cast<uint64_t>(my) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mx);
      const bool top = my == 0 || slice_start_row_[static_cast<uint64_t>(my)] != 0;
      const bool bottom = my == mb_rows_ - 1 || slice_start_row_[static_cast<uint64_t>(my + 1)] != 0;
      const bool fields = mb_fieldtx(mx, my);
      const uint64_t lo = dst_off_[0] + static_cast<uint64_t>(my * 16) * uls + static_cast<uint64_t>(mx * 16);
      const uint64_t co = dst_off_[1] + static_cast<uint64_t>(my * 8) * ucs + static_cast<uint64_t>(mx * 8);
      for (int n = 0; n < 4; ++n) {
        const uint64_t dst = lo + static_cast<uint64_t>((n & 2) * 4) * uls + static_cast<uint64_t>((n & 1) * 8);
        const int tt = tt_of(m, n);
        const bool split = tt == kTt4x4 || tt == kTt8x4;
        if (fields) {
          const int base = n < 2 ? 0 : -7;   // (the lower blocks are the bottom field: their lines start at line 1)
          if (split) rows(planes[0], dst, base + 8, ls, 8);
          if (!bottom) rows(planes[0], dst, base + 16, ls, 8);
        } else if (n < 2) {
          if (!top && split) {
            rows(planes[0], dst, 4, ls, 8);
            rows(planes[0], dst, 5, ls, 8);
          }
          rows(planes[0], dst, 8, ls, 8);
          rows(planes[0], dst, 9, ls, 8);
        } else if (!bottom) {
          if (split) {
            rows(planes[0], dst, 4, ls, 8);
            rows(planes[0], dst, 5, ls, 8);
          }
          rows(planes[0], dst, 8, ls, 8);
          rows(planes[0], dst, 9, ls, 8);
        }
      }
      for (int n = 4; n < 6; ++n) {
        if (bottom) continue;
        const int tt = tt_of(m, n);
        if (!top && (tt == kTt4x4 || tt == kTt8x4)) {
          rows(planes[n - 3], co, 4, cs, 8);
          rows(planes[n - 3], co, 5, cs, 8);
        }
        rows(planes[n - 3], co, 8, cs, 8);
        rows(planes[n - 3], co, 9, cs, 8);
      }
    }
  for (int my = 0; my < mb_rows_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      const uint64_t m = static_cast<uint64_t>(my) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mx);
      const bool right = mx == mb_w_ - 1;
      const bool fields = mb_fieldtx(mx, my);
      const uint64_t lo = dst_off_[0] + static_cast<uint64_t>(my * 16) * uls + static_cast<uint64_t>(mx * 16);
      const uint64_t co = dst_off_[1] + static_cast<uint64_t>(my * 8) * ucs + static_cast<uint64_t>(mx * 8);
      for (int n = 0; n < 4; ++n) {
        const int tt = tt_of(m, n);
        const bool split = tt == kTt4x4 || tt == kTt4x8;
        if (fields) {
          // (block n's lines: the top field's for blocks 0 and 1, the bottom field's for 2 and 3)
          const uint64_t dst = lo + static_cast<uint64_t>(n > 1 ? 1 : 0) * uls + static_cast<uint64_t>((n & 1) * 8);
          if (split) cols(planes[0], dst + 4, ls, 8);
          if (!right || !(n & 1)) cols(planes[0], dst + 8, ls, 8);
        } else {
          const uint64_t dst = lo + static_cast<uint64_t>((n & 2) * 4) * uls + static_cast<uint64_t>((n & 1) * 8);
          if (split) {
            cols(planes[0], dst + 4, ls, 4);
            cols(planes[0], dst + uls + 4, ls, 4);
          }
          if (!right || !(n & 5)) {
            cols(planes[0], dst + 8, ls, 4);
            cols(planes[0], dst + uls + 8, ls, 4);
          }
        }
      }
      for (int n = 4; n < 6; ++n) {
        const int tt = tt_of(m, n);
        if (tt == kTt4x4 || tt == kTt4x8) {
          cols(planes[n - 3], co + 4, cs, 4);
          cols(planes[n - 3], co + ucs + 4, cs, 4);
        }
        if (!right) {
          cols(planes[n - 3], co + 8, cs, 4);
          cols(planes[n - 3], co + ucs + 8, cs, 4);
        }
      }
    }
}

}  // namespace vc1
}  // namespace hardware
