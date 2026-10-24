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
// A reference plane with its real size: samples outside repeat the edge ones.
struct Ref {
  ConstByteView data;
  int stride = 0, width = 0, height = 0;
  const uint8_t* lut = nullptr;   // intensity compensation of the reference samples
  int at(int x, int y) const {
    x = x < 0 ? 0 : x >= width ? width - 1 : x;
    y = y < 0 ? 0 : y >= height ? height - 1 : y;
    const int v = data[static_cast<uint64_t>(y) * static_cast<uint64_t>(stride) + static_cast<uint64_t>(x)];
    return lut ? lut[v] : v;
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

// 8.3.4.? the intensity compensation tables: luma and chroma, built from LUMSCALE and LUMSHIFT.
void make_ic_luts(int lumscale, int lumshift, uint8_t* luty, uint8_t* lutuv) {
  int scale, shift;
  if (!lumscale) {
    scale = -64;
    shift = (255 - lumshift * 2) * 64;
    if (lumshift > 31) shift += 128 << 6;
  } else {
    scale = lumscale + 32;
    shift = lumshift > 31 ? (lumshift - 64) * 64 : lumshift << 6;
  }
  for (int i = 0; i < 256; ++i) {
    luty[i] = static_cast<uint8_t>(clip255((scale * i + shift + 32) >> 6));
    lutuv[i] = static_cast<uint8_t>(clip255((scale * (i - 128) + 128 * 64 + 32) >> 6));
  }
}
}  // namespace

const Picture* Core::reference_for(int dir) const {
  if (pic_.type == kPictureP) return anchor_[1];
  return dir == 0 ? anchor_[0] : anchor_[1];
}

// Builds the reference views of a picture, with the intensity compensation tables of the picture header when they apply.
void Core::make_refs(const Picture* ref, bool use_ic, void* luma, void* cb, void* cr) const {
  (void)use_ic;
  Ref* y = static_cast<Ref*>(luma);
  Ref* u = static_cast<Ref*>(cb);
  Ref* v = static_cast<Ref*>(cr);
  y->data = ConstByteView(ref->plane[0].data(), ref->plane[0].size());
  y->stride = ref->stride[0];
  y->width = ref_w_;
  y->height = ref_h_;
  u->data = ConstByteView(ref->plane[1].data(), ref->plane[1].size());
  u->stride = ref->stride[1];
  u->width = ref_w_ >> 1;
  u->height = ref_h_ >> 1;
  *v = *u;
  v->data = ConstByteView(ref->plane[2].data(), ref->plane[2].size());
  v->stride = ref->stride[2];
}

// The chroma vector of a macroblock from the luma one (8.3.4.5): the quarter sample vector halved, a three quarter fraction rounded up.
static void chroma_vector(int mx, int my, int* uvmx, int* uvmy) {
  *uvmx = (mx + ((mx & 3) == 3)) >> 1;
  *uvmy = (my + ((my & 3) == 3)) >> 1;
}

// One direction of a 1MV macroblock (also the prediction of the first direction of a bidirectional one).
void Core::mc_1mv(int dir) {
  const Picture* ref = reference_for(dir);
  if (!ref) return;
  const uint64_t mb_pos = static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_);
  const int mx = mv_cur_[dir][0][0], my = mv_cur_[dir][0][1];
  if (pic_.type == kPictureP) {
    cur_->direct_mv[mb_pos * 2] = s16(mx);
    cur_->direct_mv[mb_pos * 2 + 1] = s16(my);
  }
  int uvmx, uvmy;
  chroma_vector(mx, my, &uvmx, &uvmy);
  uvmv_[mb_pos * 2] = s16(uvmx);
  uvmv_[mb_pos * 2 + 1] = s16(uvmy);
  if (seq_.fastuvmc) {
    uvmx = uvmx + ((uvmx < 0) ? (uvmx & 1) : -(uvmx & 1));
    uvmy = uvmy + ((uvmy < 0) ? (uvmy & 1) : -(uvmy & 1));
  }
  predict_mb(ref, dir == 0 && ic_use_, mx, my, uvmx, uvmy, false);
}

// The prediction of the whole macroblock (luma 16x16 and chroma 8x8) from `ref`; the vectors are the luma one and the chroma one, both already adjusted.
void Core::predict_mb(const Picture* ref, bool use_ic, int mx, int my, int uvmx, int uvmy, bool avg) {
  Ref y, u, v;
  make_refs(ref, use_ic, &y, &u, &v);
  uint8_t luty[256], lutuv[256];
  if (use_ic) {
    make_ic_luts(ic_scale_, ic_shift_, luty, lutuv);
    y.lut = luty;
    u.lut = v.lut = lutuv;
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
    src_y = clip_int(src_y, -18, seq_.height + 1);
    uvsrc_y = clip_int(uvsrc_y, -8, seq_.height >> 1);
  }
  ByteView dy(cur_->plane[0].data(), cur_->plane[0].size()), du(cur_->plane[1].data(), cur_->plane[1].size()), dv(cur_->plane[2].data(), cur_->plane[2].size());
  const uint64_t loff = static_cast<uint64_t>(mb_y_ * 16) * static_cast<uint64_t>(cur_->stride[0]) + static_cast<uint64_t>(mb_x_ * 16);
  const uint64_t coff = static_cast<uint64_t>(mb_y_ * 8) * static_cast<uint64_t>(cur_->stride[1]) + static_cast<uint64_t>(mb_x_ * 8);
  const int rnd = pic_.rnd;
  if (pic_.mspel) {
    mspel_mc(dy, loff, cur_->stride[0], y, src_x, src_y, mx & 3, my & 3, rnd, 16, 16, avg);
  } else {
    hpel_mc(dy, loff, cur_->stride[0], y, src_x, src_y, (my & 2) | ((mx & 2) >> 1), rnd, 16, 16, avg);
  }
  const int fx = (uvmx & 3) << 1, fy = (uvmy & 3) << 1;
  chroma_mc(du, coff, cur_->stride[1], u, uvsrc_x, uvsrc_y, fx, fy, rnd, 8, avg);
  chroma_mc(dv, coff, cur_->stride[2], v, uvsrc_x, uvsrc_y, fx, fy, rnd, 8, avg);
}

// The luma block n of a 4MV macroblock.
void Core::mc_4mv_luma(int n) {
  const Picture* ref = reference_for(0);
  if (!ref) return;
  const int mx = mv_cur_[0][n][0], my = mv_cur_[0][n][1];
  Ref y, u, v;
  make_refs(ref, false, &y, &u, &v);
  uint8_t luty[256], lutuv[256];
  const bool use_ic = ic_use_;
  if (use_ic) {
    make_ic_luts(ic_scale_, ic_shift_, luty, lutuv);
    y.lut = luty;
  }
  int src_x = mb_x_ * 16 + (n & 1) * 8 + (mx >> 2);
  int src_y = mb_y_ * 16 + (n & 2) * 4 + (my >> 2);
  if (seq_.profile != kAdvanced) {
    src_x = clip_int(src_x, -16, mb_w_ * 16);
    src_y = clip_int(src_y, -16, mb_h_ * 16);
  } else {
    src_x = clip_int(src_x, -17, seq_.width);
    src_y = clip_int(src_y, -18, seq_.height + 1);
  }
  ByteView dy(cur_->plane[0].data(), cur_->plane[0].size());
  const uint64_t off = static_cast<uint64_t>(mb_y_ * 16 + (n >> 1) * 8) * static_cast<uint64_t>(cur_->stride[0]) + static_cast<uint64_t>(mb_x_ * 16 + (n & 1) * 8);
  if (pic_.mspel) mspel_mc(dy, off, cur_->stride[0], y, src_x, src_y, mx & 3, my & 3, pic_.rnd, 8, 8, false);
  else hpel_mc(dy, off, cur_->stride[0], y, src_x, src_y, (my & 2) | ((mx & 2) >> 1), pic_.rnd, 8, 8, false);
}

namespace {
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
}  // namespace

// The chroma blocks of a 4MV macroblock: one vector derived from the luma vectors of the blocks that are not intra.
void Core::mc_4mv_chroma() {
  const Picture* ref = reference_for(0);
  const uint64_t mb_pos = static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_);
  if (!ref) return;
  static const uint8_t popcount4[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
  static const uint8_t index2[16] = {0, 0, 0, 0x01, 0, 0x02, 0x12, 0, 0, 0x03, 0x13, 0, 0x23, 0, 0, 0};
  const int idx = (((intra_mask_[mb_pos] >> 0) & 1) ^ 1) | ((((intra_mask_[mb_pos] >> 1) & 1) ^ 1) << 1) | ((((intra_mask_[mb_pos] >> 2) & 1) ^ 1) << 2) | ((((intra_mask_[mb_pos] >> 3) & 1) ^ 1) << 3);
  const int valid = popcount4[idx];
  int tx, ty;
  const int(*mv)[2] = nullptr;
  int mvs[4][2];
  for (int k = 0; k < 4; ++k) {
    mvs[k][0] = mv_cur_[0][k][0];
    mvs[k][1] = mv_cur_[0][k][1];
  }
  mv = mvs;
  switch (valid) {
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
      cur_->direct_mv[mb_pos * 2] = cur_->direct_mv[mb_pos * 2 + 1] = 0;
      uvmv_[mb_pos * 2] = uvmv_[mb_pos * 2 + 1] = 0;
      return;   // no chroma prediction for intra blocks
  }
  cur_->direct_mv[mb_pos * 2] = s16(tx);
  cur_->direct_mv[mb_pos * 2 + 1] = s16(ty);
  int uvmx, uvmy;
  chroma_vector(tx, ty, &uvmx, &uvmy);
  uvmv_[mb_pos * 2] = s16(uvmx);
  uvmv_[mb_pos * 2 + 1] = s16(uvmy);
  if (seq_.fastuvmc) {
    uvmx = uvmx + ((uvmx < 0) ? (uvmx & 1) : -(uvmx & 1));
    uvmy = uvmy + ((uvmy < 0) ? (uvmy & 1) : -(uvmy & 1));
  }
  Ref y, u, v;
  make_refs(ref, false, &y, &u, &v);
  uint8_t luty[256], lutuv[256];
  if (ic_use_) {
    make_ic_luts(ic_scale_, ic_shift_, luty, lutuv);
    u.lut = v.lut = lutuv;
  }
  int uvsrc_x = mb_x_ * 8 + (uvmx >> 2), uvsrc_y = mb_y_ * 8 + (uvmy >> 2);
  if (seq_.profile != kAdvanced) {
    uvsrc_x = clip_int(uvsrc_x, -8, mb_w_ * 8);
    uvsrc_y = clip_int(uvsrc_y, -8, mb_h_ * 8);
  } else {
    uvsrc_x = clip_int(uvsrc_x, -8, seq_.width >> 1);
    uvsrc_y = clip_int(uvsrc_y, -8, seq_.height >> 1);
  }
  ByteView du(cur_->plane[1].data(), cur_->plane[1].size()), dv(cur_->plane[2].data(), cur_->plane[2].size());
  const uint64_t coff = static_cast<uint64_t>(mb_y_ * 8) * static_cast<uint64_t>(cur_->stride[1]) + static_cast<uint64_t>(mb_x_ * 8);
  const int fx = (uvmx & 3) << 1, fy = (uvmy & 3) << 1;
  chroma_mc(du, coff, cur_->stride[1], u, uvsrc_x, uvsrc_y, fx, fy, pic_.rnd, 8, false);
  chroma_mc(dv, coff, cur_->stride[2], v, uvsrc_x, uvsrc_y, fx, fy, pic_.rnd, 8, false);
}

// The backward prediction of an interpolated (or direct) B macroblock, averaged with the forward one already in the picture.
void Core::interp_mc() {
  const Picture* ref = anchor_[1];
  if (!ref) return;
  const int mx = mv_cur_[1][0][0], my = mv_cur_[1][0][1];
  int uvmx, uvmy;
  chroma_vector(mx, my, &uvmx, &uvmy);
  if (seq_.fastuvmc) {   // (the reference decoders round this one away from zero, unlike the single direction case)
    uvmx = uvmx + ((uvmx < 0) ? -(uvmx & 1) : (uvmx & 1));
    uvmy = uvmy + ((uvmy < 0) ? -(uvmy & 1) : (uvmy & 1));
  }
  predict_mb(ref, false, mx, my, uvmx, uvmy, true);
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
  for (int my = 0; my < mb_h_; ++my)
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
  for (int my = 0; my < mb_h_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx)
      for (int i = 0; i < 6; ++i) {
        const bool slice_first = slice_start_row_[static_cast<uint64_t>(my)] != 0;
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
  for (int my = 0; my < mb_h_; ++my)
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
  for (int my = 0; my < mb_h_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx)
      for (int i = 0; i < 6; ++i) {
        const bool slice_first = slice_start_row_[static_cast<uint64_t>(my)] != 0;
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
  for (int my = 0; my < mb_h_; ++my)
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
  const int ls = cur_->stride[0], cs = cur_->stride[1];
  // The edges between rows (the vertical filter) of the whole picture first, then the edges between columns.
  for (int my = 0; my < mb_h_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      const bool top = slice_start_row_[static_cast<uint64_t>(my)] != 0;
      const uint64_t lo = static_cast<uint64_t>(my * 16) * static_cast<uint64_t>(ls) + static_cast<uint64_t>(mx * 16);
      const uint64_t co = static_cast<uint64_t>(my * 8) * static_cast<uint64_t>(cs) + static_cast<uint64_t>(mx * 8);
      if (!top) {
        filter_edge(planes[0], lo, 1, ls, 16, pq);
        filter_edge(planes[1], co, 1, cs, 8, pq);
        filter_edge(planes[2], co, 1, cs, 8, pq);
      }
      filter_edge(planes[0], lo + static_cast<uint64_t>(8 * ls), 1, ls, 16, pq);
    }
  for (int my = 0; my < mb_h_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      const uint64_t lo = static_cast<uint64_t>(my * 16) * static_cast<uint64_t>(ls) + static_cast<uint64_t>(mx * 16);
      const uint64_t co = static_cast<uint64_t>(my * 8) * static_cast<uint64_t>(cs) + static_cast<uint64_t>(mx * 8);
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
  // Where block n of macroblock (mx, my) starts in its plane, and the plane's stride.
  auto where = [&](int mx, int my, int n, int* plane, int* stride) -> uint64_t {
    if (n < 4) {
      *plane = 0;
      *stride = cur_->stride[0];
      return static_cast<uint64_t>(my * 16 + (n >> 1) * 8) * static_cast<uint64_t>(*stride) + static_cast<uint64_t>(mx * 16 + (n & 1) * 8);
    }
    *plane = n - 3;
    *stride = cur_->stride[1];
    return static_cast<uint64_t>(my * 8) * static_cast<uint64_t>(*stride) + static_cast<uint64_t>(mx * 8);
  };
  // The edges at the bottom of the blocks (and the 4x4 edges inside them), the whole picture first.
  for (int my = 0; my < mb_h_; ++my)
    for (int mx = 0; mx < mb_w_; ++mx) {
      const uint64_t m = mb_index(mx, my);
      const bool last_row_of_slice = my == mb_h_ - 1 || slice_start_row_[static_cast<uint64_t>(my + 1)] != 0;
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
            differ = uvmv_[m * 2] != uvmv_[b * 2] || uvmv_[m * 2 + 1] != uvmv_[b * 2 + 1];
          } else if (n < 2) {
            bottom_intra = (intra_mask_[m] >> (n + 2)) & 1;
            bottom_cbp = cbp_blocks_[m] >> ((n + 2) * 4);
            const int x = 2 * mx + (n & 1), y = 2 * my;
            differ = blk_mv(x, y, 0) != blk_mv(x, y + 1, 0) || blk_mv(x, y, 1) != blk_mv(x, y + 1, 1);
          } else {
            const uint64_t b = mb_index(mx, my + 1);
            bottom_intra = (intra_mask_[b] >> (n - 2)) & 1;
            bottom_cbp = cbp_blocks_[b] >> ((n - 2) * 4);
            const int x = 2 * mx + (n & 1), y = 2 * my + 1;
            differ = blk_mv(x, y, 0) != blk_mv(x, y + 1, 0) || blk_mv(x, y, 1) != blk_mv(x, y + 1, 1);
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
  for (int my = 0; my < mb_h_; ++my)
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
            differ = uvmv_[m * 2] != uvmv_[r * 2] || uvmv_[m * 2 + 1] != uvmv_[r * 2 + 1];
          } else if (n & 1) {
            const uint64_t r = mb_index(mx + 1, my);
            right_intra = (intra_mask_[r] >> (n - 1)) & 1;
            right_cbp = cbp_blocks_[r] >> ((n - 1) * 4);
            const int x = 2 * mx + 1, y = 2 * my + (n >> 1);
            differ = blk_mv(x, y, 0) != blk_mv(x + 1, y, 0) || blk_mv(x, y, 1) != blk_mv(x + 1, y, 1);
          } else {
            right_intra = (intra_mask_[m] >> (n + 1)) & 1;
            right_cbp = cbp_blocks_[m] >> ((n + 1) * 4);
            const int x = 2 * mx, y = 2 * my + (n >> 1);
            differ = blk_mv(x, y, 0) != blk_mv(x + 1, y, 0) || blk_mv(x, y, 1) != blk_mv(x + 1, y, 1);
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
      if (overlap) overlap_filter_i();
      put_intra_blocks(adv || (seq_.overlap && pic_.pq >= 9));
      if (seq_.loop_filter) loop_filter_i();
      break;
    }
    case kPictureP:
      if (seq_.overlap && pic_.pq >= 9) overlap_filter_p();
      put_intra_blocks(true);
      if (seq_.loop_filter) loop_filter_p();
      break;
    default:
      if (seq_.loop_filter) loop_filter_i();
      break;
  }
}

}  // namespace vc1
}  // namespace hardware
