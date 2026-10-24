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

#include "Hardware/VideoCodec/Software/Av1Wedge.hpp"
#include "Hardware/VideoCodec/Software/Av1Tables.inc"

// Port of dav1d src/wedge.c (BSD-2-Clause, Copyright (c) VideoLAN and dav1d authors), see Av1Wedge.hpp.

namespace hardware {
namespace av1 {

namespace {

enum WedgeDirectionType {
  WEDGE_HORIZONTAL = 0,
  WEDGE_VERTICAL = 1,
  WEDGE_OBLIQUE27 = 2,
  WEDGE_OBLIQUE63 = 3,
  WEDGE_OBLIQUE117 = 4,
  WEDGE_OBLIQUE153 = 5,
};

struct WedgeCode {
  uint8_t direction;
  uint8_t x_offset;
  uint8_t y_offset;
};

constexpr WedgeCode kCodebookHgtw[16] = {
    {WEDGE_OBLIQUE27, 4, 4},  {WEDGE_OBLIQUE63, 4, 4},  {WEDGE_OBLIQUE117, 4, 4}, {WEDGE_OBLIQUE153, 4, 4},
    {WEDGE_HORIZONTAL, 4, 2}, {WEDGE_HORIZONTAL, 4, 4}, {WEDGE_HORIZONTAL, 4, 6}, {WEDGE_VERTICAL, 4, 4},
    {WEDGE_OBLIQUE27, 4, 2},  {WEDGE_OBLIQUE27, 4, 6},  {WEDGE_OBLIQUE153, 4, 2}, {WEDGE_OBLIQUE153, 4, 6},
    {WEDGE_OBLIQUE63, 2, 4},  {WEDGE_OBLIQUE63, 6, 4},  {WEDGE_OBLIQUE117, 2, 4}, {WEDGE_OBLIQUE117, 6, 4},
};

constexpr WedgeCode kCodebookHltw[16] = {
    {WEDGE_OBLIQUE27, 4, 4}, {WEDGE_OBLIQUE63, 4, 4},  {WEDGE_OBLIQUE117, 4, 4}, {WEDGE_OBLIQUE153, 4, 4},
    {WEDGE_VERTICAL, 2, 4},  {WEDGE_VERTICAL, 4, 4},   {WEDGE_VERTICAL, 6, 4},   {WEDGE_HORIZONTAL, 4, 4},
    {WEDGE_OBLIQUE27, 4, 2}, {WEDGE_OBLIQUE27, 4, 6},  {WEDGE_OBLIQUE153, 4, 2}, {WEDGE_OBLIQUE153, 4, 6},
    {WEDGE_OBLIQUE63, 2, 4}, {WEDGE_OBLIQUE63, 6, 4},  {WEDGE_OBLIQUE117, 2, 4}, {WEDGE_OBLIQUE117, 6, 4},
};

constexpr WedgeCode kCodebookHeqw[16] = {
    {WEDGE_OBLIQUE27, 4, 4},  {WEDGE_OBLIQUE63, 4, 4},  {WEDGE_OBLIQUE117, 4, 4}, {WEDGE_OBLIQUE153, 4, 4},
    {WEDGE_HORIZONTAL, 4, 2}, {WEDGE_HORIZONTAL, 4, 6}, {WEDGE_VERTICAL, 2, 4},   {WEDGE_VERTICAL, 6, 4},
    {WEDGE_OBLIQUE27, 4, 2},  {WEDGE_OBLIQUE27, 4, 6},  {WEDGE_OBLIQUE153, 4, 2}, {WEDGE_OBLIQUE153, 4, 6},
    {WEDGE_OBLIQUE63, 2, 4},  {WEDGE_OBLIQUE63, 6, 4},  {WEDGE_OBLIQUE117, 2, 4}, {WEDGE_OBLIQUE117, 6, 4},
};

constexpr int kMasterSize = 64 * 64;

int imax(int a, int b) { return a > b ? a : b; }
int imin(int a, int b) { return a < b ? a : b; }

void insert_border(ByteView dst, ConstByteView src, int ctr) {
  if (ctr > 4) dst.first(ctr - 4).fill(0);
  dst.subview(imax(ctr, 4) - 4, imin(64 - ctr, 8)).copy_from(src.subview(imax(4 - ctr, 0), imin(64 - ctr, 8)));
  if (ctr < 64 - 4) dst.subview(ctr + 4, 64 - 4 - ctr).fill(64);
}

void transpose(ByteView dst, ConstByteView src) {
  for (int y = 0, y_off = 0; y < 64; y++, y_off += 64)
    for (int x = 0, x_off = 0; x < 64; x++, x_off += 64) dst[x_off + y] = src[y_off + x];
}

void hflip(ByteView dst, ConstByteView src) {
  for (int y = 0, y_off = 0; y < 64; y++, y_off += 64)
    for (int x = 0; x < 64; x++) dst[y_off + 64 - 1 - x] = src[y_off + x];
}

void copy2d(ByteView dst, ConstByteView src, int sign, int w, int h, int x_off, int y_off) {
  src = src.subview(y_off * 64 + x_off);
  for (int y = 0; y < h; y++) {
    const ConstByteView s = src.subview(y * 64, w);
    const ByteView d = dst.subview(y * w, w);
    if (sign) {
      for (int x = 0; x < w; x++) d[x] = static_cast<uint8_t>(64 - s[x]);
    } else {
      d.copy_from(s);
    }
  }
}

void init_chroma(ByteView chroma, ConstByteView luma, int sign, int w, int h, int ss_ver) {
  int luma_off = 0, chroma_off = 0;
  for (int y = 0; y < h; y += 1 + ss_ver) {
    for (int x = 0; x < w; x += 2) {
      int sum = luma[luma_off + x] + luma[luma_off + x + 1] + 1;
      if (ss_ver) sum += luma[luma_off + w + x] + luma[luma_off + w + x + 1] + 1;
      chroma[chroma_off + (x >> 1)] = static_cast<uint8_t>((sum - sign) >> (1 + ss_ver));
    }
    luma_off += w << ss_ver;
    chroma_off += w >> 1;
  }
}

constexpr uint8_t kIiWeights1d[32] = {
    60, 52, 45, 39, 34, 30, 26, 22, 19, 17, 15, 13, 11, 10, 8, 7, 6, 6, 5, 4, 4, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 1,
};

void build_nondc_ii_masks(ByteView mask_v, int w, int h, int step) {
  const ByteView mask_h = mask_v.subview(w * h, w * h);
  const ByteView mask_sm = mask_v.subview(2 * w * h, w * h);
  for (int y = 0, off = 0; y < h; y++, off += w) {
    mask_v.subview(off, w).fill(kIiWeights1d[y * step]);
    for (int x = 0; x < w; x++) {
      mask_sm[off + x] = kIiWeights1d[imin(x, y) * step];
      mask_h[off + x] = kIiWeights1d[x * step];
    }
  }
}

// One wedge block size: width, height, codebook, sign bits and the index in the table (block size - BS_32x32).
struct WedgeSize {
  int w, h;
  const WedgeCode* codebook;
  unsigned signs;
  int bs;
};
constexpr WedgeSize kWedgeSizes[9] = {
    {32, 32, kCodebookHeqw, 0x7bfb, BS_32x32 - BS_32x32}, {32, 16, kCodebookHltw, 0x7beb, BS_32x16 - BS_32x32},
    {32, 8, kCodebookHltw, 0x6beb, BS_32x8 - BS_32x32},   {16, 32, kCodebookHgtw, 0x7beb, BS_16x32 - BS_32x32},
    {16, 16, kCodebookHeqw, 0x7bfb, BS_16x16 - BS_32x32}, {16, 8, kCodebookHltw, 0x7beb, BS_16x8 - BS_32x32},
    {8, 32, kCodebookHgtw, 0x7aeb, BS_8x32 - BS_32x32},   {8, 16, kCodebookHgtw, 0x7beb, BS_8x16 - BS_32x32},
    {8, 8, kCodebookHeqw, 0x7bfb, BS_8x8 - BS_32x32},
};

// The non-DC inter-intra mask sets: width, height and the step into the 1-D weights.
struct IiSize {
  int w, h, step;
};
constexpr IiSize kIiSizes[9] = {{32, 32, 1}, {16, 32, 1}, {16, 16, 2}, {8, 32, 1}, {8, 16, 2}, {8, 8, 4}, {4, 16, 2}, {4, 8, 4}, {4, 4, 8}};

// Which of those sets serve a block size in each of the layouts (444, 422, 420): the dimensions of the stored mask.
struct IiAssign {
  int bs;
  int dims[3][2];
};
constexpr IiAssign kIiAssign[7] = {
    {BS_32x32, {{32, 32}, {16, 32}, {16, 16}}}, {BS_32x16, {{32, 32}, {16, 16}, {16, 16}}}, {BS_16x32, {{16, 32}, {8, 32}, {8, 16}}},
    {BS_16x16, {{16, 16}, {8, 16}, {8, 8}}},    {BS_16x8, {{16, 16}, {8, 8}, {8, 8}}},      {BS_8x16, {{8, 16}, {4, 16}, {4, 8}}},
    {BS_8x8, {{8, 8}, {4, 8}, {4, 4}}},
};

constexpr uint32_t kWedge444Total = 16 * (32 * 32 + 32 * 16 + 32 * 8 + 16 * 32 + 16 * 16 + 16 * 8 + 8 * 32 + 8 * 16 + 8 * 8);
constexpr uint32_t kWedge422Total = kWedge444Total;
constexpr uint32_t kWedge420Total = kWedge444Total / 2;
constexpr uint32_t kIiDcSize = 32 * 32;
constexpr uint32_t kIiNonDcTotal = 3 * (32 * 32 + 16 * 32 + 16 * 16 + 8 * 32 + 8 * 16 + 8 * 8 + 4 * 16 + 4 * 8 + 4 * 4);
constexpr uint32_t kPoolSize = kWedge444Total + kWedge422Total + kWedge420Total + kIiDcSize + kIiNonDcTotal;

}  // namespace

bool WedgeMasks::init() {
  if (ready_) return true;
  pool_.clear();
  if (!pool_.put_zeros(kPoolSize)) return false;
  const ByteView pool = pool_.writable_view();

  uint8_t master_storage[6][kMasterSize];
  ByteView master[6];
  for (int i = 0; i < 6; i++) master[i] = ByteView(master_storage[i]);

  // create master templates
  static constexpr uint8_t kBorderOdd[8] = {1, 2, 6, 18, 37, 53, 60, 63};
  static constexpr uint8_t kBorderEven[8] = {1, 4, 11, 27, 46, 58, 62, 63};
  static constexpr uint8_t kBorderVert[8] = {0, 2, 7, 21, 43, 57, 62, 64};
  for (int y = 0, off = 0; y < 64; y++, off += 64) insert_border(master[WEDGE_VERTICAL].subview(off, 64), ConstByteView(kBorderVert), 32);
  for (int y = 0, off = 0, ctr = 48; y < 64; y += 2, off += 128, ctr--) {
    insert_border(master[WEDGE_OBLIQUE63].subview(off, 64), ConstByteView(kBorderEven), ctr);
    insert_border(master[WEDGE_OBLIQUE63].subview(off + 64, 64), ConstByteView(kBorderOdd), ctr - 1);
  }
  transpose(master[WEDGE_OBLIQUE27], master[WEDGE_OBLIQUE63]);
  transpose(master[WEDGE_HORIZONTAL], master[WEDGE_VERTICAL]);
  hflip(master[WEDGE_OBLIQUE117], master[WEDGE_OBLIQUE63]);
  hflip(master[WEDGE_OBLIQUE153], master[WEDGE_OBLIQUE27]);

  uint32_t base444 = 0;
  uint32_t base422 = kWedge444Total;
  uint32_t base420 = kWedge444Total + kWedge422Total;
  for (const WedgeSize& size : kWedgeSizes) {
    const int w = size.w, h = size.h;
    const uint32_t n_stride_444 = static_cast<uint32_t>(w * h);
    const uint32_t n_stride_422 = n_stride_444 >> 1;
    const uint32_t n_stride_420 = n_stride_444 >> 2;
    const uint32_t sign_stride_422 = 16 * n_stride_422;
    const uint32_t sign_stride_420 = 16 * n_stride_420;
    unsigned signs = size.signs;

    // assign pointer offsets in lookup table
    for (int n = 0; n < 16; n++) {
      const WedgeCode& code = size.codebook[n];
      const unsigned sign = signs & 1;
      const uint32_t o444 = base444 + n * n_stride_444;
      const uint32_t o422 = base422 + n * n_stride_422;
      const uint32_t o420 = base420 + n * n_stride_420;
      const ByteView mask444 = pool.subview(o444, n_stride_444);

      copy2d(mask444, master[code.direction], static_cast<int>(sign), w, h, 32 - (w * code.x_offset >> 3), 32 - (h * code.y_offset >> 3));

      // not using !sign is intentional here, since 444 does not require any rounding since no chroma subsampling is applied.
      offsets_[0][size.bs].wedge[0][n] = offsets_[0][size.bs].wedge[1][n] = o444;

      const uint32_t o422_0 = o422 + sign * sign_stride_422;
      const uint32_t o422_1 = o422 + (sign ^ 1) * sign_stride_422;
      const uint32_t o420_0 = o420 + sign * sign_stride_420;
      const uint32_t o420_1 = o420 + (sign ^ 1) * sign_stride_420;
      init_chroma(pool.subview(o422_0, n_stride_422), mask444, 0, w, h, 0);
      init_chroma(pool.subview(o422_1, n_stride_422), mask444, 1, w, h, 0);
      init_chroma(pool.subview(o420_0, n_stride_420), mask444, 0, w, h, 1);
      init_chroma(pool.subview(o420_1, n_stride_420), mask444, 1, w, h, 1);
      offsets_[1][size.bs].wedge[0][n] = o422_0;
      offsets_[1][size.bs].wedge[1][n] = o422_1;
      offsets_[2][size.bs].wedge[0][n] = o420_0;
      offsets_[2][size.bs].wedge[1][n] = o420_1;

      signs >>= 1;
    }
    base444 += 16 * n_stride_444;
    base422 += 2 * sign_stride_422;
    base420 += 2 * sign_stride_420;
  }

  const uint32_t ii_dc = kWedge444Total + kWedge422Total + kWedge420Total;
  pool.subview(ii_dc, kIiDcSize).fill(32);
  for (auto& per_layout : offsets_) {
    for (const IiAssign& a : kIiAssign) per_layout[a.bs - BS_32x32].ii[II_DC_PRED] = ii_dc;
  }

  uint32_t ii_nondc_offsets[9];
  uint32_t cursor = ii_dc + kIiDcSize;
  for (int i = 0; i < 9; i++) {
    const IiSize& s = kIiSizes[i];
    ii_nondc_offsets[i] = cursor;
    build_nondc_ii_masks(pool.subview(cursor, 3 * s.w * s.h), s.w, s.h, s.step);
    cursor += 3 * s.w * s.h;
  }
  if (cursor != kPoolSize) return false;

  for (int p = 0; p < 3; p++) {
    for (const IiAssign& a : kIiAssign) {
      for (int layout = 0; layout < 3; layout++) {
        const int w = a.dims[layout][0], h = a.dims[layout][1];
        for (int i = 0; i < 9; i++) {
          if (kIiSizes[i].w == w && kIiSizes[i].h == h)
            offsets_[layout][a.bs - BS_32x32].ii[p + 1] = ii_nondc_offsets[i] + p * w * h;
        }
      }
    }
  }
  ready_ = true;
  return true;
}

ConstByteView WedgeMasks::at(uint32_t offset, int layout, BlockSize bs) const {
  if (offset == kNone) return ConstByteView();
  const int ss_hor = layout != 0, ss_ver = layout == 2;
  const uint32_t size = static_cast<uint32_t>((k_block_dimensions[bs][0] * 4 >> ss_hor) * (k_block_dimensions[bs][1] * 4 >> ss_ver));
  ConstByteView out;
  if (!pool_.view().try_subview(offset, size, &out)) return ConstByteView();
  return out;
}

ConstByteView WedgeMasks::wedge_mask(int layout, BlockSize bs, int sign, int wedge_idx) const {
  if (!ready_ || layout < 0 || layout > 2 || bs < BS_32x32 || bs > BS_8x8 || sign < 0 || sign > 1 || wedge_idx < 0 || wedge_idx > 15)
    return ConstByteView();
  return at(offsets_[layout][bs - BS_32x32].wedge[sign][wedge_idx], layout, bs);
}

ConstByteView WedgeMasks::ii_mask(int layout, BlockSize bs, InterIntraType interintra_type, int interintra_mode, int wedge_idx) const {
  if (!ready_ || layout < 0 || layout > 2 || bs < BS_32x32 || bs > BS_8x8) return ConstByteView();
  if (interintra_type == INTER_INTRA_BLEND) {
    if (interintra_mode < 0 || interintra_mode >= N_INTER_INTRA_PRED_MODES) return ConstByteView();
    return at(offsets_[layout][bs - BS_32x32].ii[interintra_mode], layout, bs);
  }
  if (wedge_idx < 0 || wedge_idx > 15) return ConstByteView();
  return at(offsets_[layout][bs - BS_32x32].wedge[0][wedge_idx], layout, bs);
}

}  // namespace av1
}  // namespace hardware
