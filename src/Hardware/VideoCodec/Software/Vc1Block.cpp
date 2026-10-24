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


// VC-1 decoding, part 2: macroblocks, the coefficients of the blocks, DC and AC prediction and motion vector prediction (SMPTE 421M clauses 7 and 8).
#include "Hardware/VideoCodec/Software/Vc1Core.hpp"

namespace hardware {
namespace vc1 {

namespace {
#include "Hardware/VideoCodec/Software/Vc1Tables.inc"

constexpr uint8_t kOffsetTable[9] = {0, 1, 3, 7, 15, 31, 63, 127, 255};
constexpr uint8_t kSizeTable[6] = {0, 2, 3, 4, 5, 8};
constexpr uint16_t kDcPred[32] = {0xFFFF, 1024, 512, 341, 256, 205, 171, 146, 128, 114, 102, 93, 85, 79, 73, 68, 64, 60, 57, 54, 51, 49, 47, 45, 43, 41, 39, 38, 37, 35, 34, 33};

int iabs(int v) { return v < 0 ? -v : v; }
int mid_pred(int a, int b, int c) {
  const int lo = a < b ? a : b, hi = a < b ? b : a;
  return c < lo ? lo : c > hi ? hi : c;
}
// (value * scale * dq + 0x20000) >> 18 in 32-bit unsigned arithmetic, as the reference decoders do it.
int scale_pred(int value, int scale, int dq) {
  const uint32_t product = static_cast<uint32_t>(value) * static_cast<uint32_t>(scale) * static_cast<uint32_t>(dq) + 0x20000u;
  return static_cast<int32_t>(product) >> 18;
}
int16_t s16(int v) { return static_cast<int16_t>(v); }
}  // namespace

// ---- helpers -------------------------------------------------------------------------------------------------------------------------

namespace {
// Where block n of the macroblock (mx, my) is: its plane (0 luma, 1 and 2 chroma), position and the width of the plane in blocks.
struct BlockPos {
  int plane, x, y, wrap;
};
BlockPos block_pos(int mx, int my, int mb_w, int n) {
  if (n < 4) return {0, 2 * mx + (n & 1), 2 * my + (n >> 1), 2 * mb_w};
  return {n - 3, mx, my, mb_w};
}
}  // namespace

// ---- coefficients --------------------------------------------------------------------------------------------------------------------

// 8.1.3.4: one run/level pair. Returns false on a code that does not exist.
bool Core::decode_ac_coeff(BitReader& br, int* last, int* skip, int* value, int codingset) {
  const int size = kAcSize[codingset];
  int index = vlc_ac_[codingset].decode(br);
  if (index < 0) return false;
  const int base = codingset * 185;
  int run, level, lst, sign;
  if (index != size - 1) {
    run = kAcRun[base + index];
    level = kAcLevel[base + index];
    lst = index >= kAcLastIndex[codingset];
    sign = br.bit();
  } else {
    const int escape = br.bit() ? 0 : 2 - br.bit();   // decode210
    if (escape != 2) {
      index = vlc_ac_[codingset].decode(br);
      if (index < 0 || index >= size - 1) return false;
      run = kAcRun[base + index];
      level = kAcLevel[base + index];
      lst = index >= kAcLastIndex[codingset];
      if (escape == 0) {
        level += lst ? kAcLastDeltaLevel[codingset * 44 + run] : kAcDeltaLevel[codingset * 31 + run];
      } else {
        run += (lst ? kAcLastDeltaRun[codingset * 10 + level] : kAcDeltaRun[codingset * 57 + level]) + 1;
      }
      sign = br.bit();
    } else {
      lst = br.bit();
      if (esc3_level_length_ == 0) {
        if (pic_.pq < 8 || pic_.dquantfrm) {   // table 59
          esc3_level_length_ = static_cast<int>(br.get(3));
          if (!esc3_level_length_) esc3_level_length_ = static_cast<int>(br.get(2)) + 8;
        } else {   // table 60
          esc3_level_length_ = br.unary(1, 6) + 2;
        }
        esc3_run_length_ = 3 + static_cast<int>(br.get(2));
      }
      run = static_cast<int>(br.get(esc3_run_length_));
      sign = br.bit();
      level = static_cast<int>(br.get(esc3_level_length_));
    }
  }
  *last = lst;
  *skip = run;
  *value = (level ^ -sign) + sign;
  return !br.error();
}

// 8.1.1.7 MQUANT: the quantizer of a macroblock; negative when it is not the picture's (no half step then).
bool Core::decode_mquant(BitReader& br, int* mquant) {
  if (!pic_.dquantfrm) return true;
  int edges = 0;
  if (pic_.dqprofile == kDqAllMbs) {
    if (pic_.dqbilevel) {
      *mquant = br.bit() ? -pic_.altpq : pic_.pq;
    } else {
      const int mqdiff = static_cast<int>(br.get(3));
      *mquant = mqdiff != 7 ? -pic_.pq - mqdiff : -static_cast<int>(br.get(5));
    }
  }
  if (pic_.dqprofile == kDqSingleEdge) edges = 1 << pic_.dqsbedge;
  else if (pic_.dqprofile == kDqDoubleEdges) edges = (3 << pic_.dqsbedge) % 15;
  else if (pic_.dqprofile == kDqFourEdges) edges = 15;
  if ((edges & 1) && !mb_x_) *mquant = -pic_.altpq;
  if ((edges & 2) && !mb_y_) *mquant = -pic_.altpq;
  if ((edges & 4) && mb_x_ == mb_w_ - 1) *mquant = -pic_.altpq;
  if ((edges & 8) && mb_y_ == mb_h_ - 1) *mquant = -pic_.altpq;
  if (!*mquant || *mquant > 31 || *mquant < -31) *mquant = 1;
  return !br.error();
}

// ---- neighbours: dc, ac, coded flags ---------------------------------------------------------------------------------------------------

namespace {
int16_t dc_get(const Buf<int16_t>* dc, const BlockPos& p, int dx, int dy) {
  const int x = p.x + dx, y = p.y + dy;
  if (x < 0 || y < 0 || x >= p.wrap) return 0;
  const uint64_t i = static_cast<uint64_t>(y) * static_cast<uint64_t>(p.wrap) + static_cast<uint64_t>(x);
  return i < dc[p.plane].size() ? dc[p.plane][i] : 0;
}
}  // namespace

// 8.1.1.6 DC prediction for I pictures of Simple and Main profile: the predicted value and the direction (1: from the left).
int Core::pred_dc_i(int n, int16_t** dc_val, int* dir) {
  const BlockPos p = block_pos(mb_x_, mb_y_, mb_w_, n);
  int c = dc_get(dc_, p, -1, 0), b = dc_get(dc_, p, -1, -1), a = dc_get(dc_, p, 0, -1);
  const int scale = y_dc_scale_;
  const int edge = (pic_.pq < 9 || !seq_.overlap) ? kDcPred[scale] : 0;
  if (first_slice_line_ && n != 2 && n != 3) b = a = static_cast<int16_t>(edge);
  if (mb_x_ == 0 && n != 1 && n != 3) b = c = static_cast<int16_t>(edge);
  int pred;
  if (iabs(a - b) <= iabs(b - c)) {
    pred = c;
    *dir = 1;
  } else {
    pred = a;
    *dir = 0;
  }
  *dc_val = &dc_[p.plane][static_cast<uint64_t>(p.y) * static_cast<uint64_t>(p.wrap) + static_cast<uint64_t>(p.x)];
  return pred;
}

// 8.1.1.6 DC prediction with the quantizer scaling of the neighbours (P and B pictures, Advanced profile I pictures).
int Core::pred_dc(int n, int quant, bool a_avail, bool c_avail, int16_t** dc_val, int* dir) {
  const BlockPos p = block_pos(mb_x_, mb_y_, mb_w_, n);
  const uint64_t mb_pos = static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_);
  *dc_val = &dc_[p.plane][static_cast<uint64_t>(p.y) * static_cast<uint64_t>(p.wrap) + static_cast<uint64_t>(p.x)];
  const int q1 = iabs(qscale_[mb_pos]);
  (void)quant;
  const int dq_index = kDcScale[q1] - 1;
  if (dq_index < 0) return 0;
  int c = dc_get(dc_, p, -1, 0), b = dc_get(dc_, p, -1, -1), a = dc_get(dc_, p, 0, -1);
  const uint64_t stride = static_cast<uint64_t>(mb_w_);
  if (c_avail && n != 1 && n != 3) {
    const int q2 = iabs(qscale_[mb_pos - 1]);
    if (q2 && q2 != q1) c = scale_pred(c, kDcScale[q2], kDqScale[dq_index]);
  }
  if (a_avail && n != 2 && n != 3) {
    const int q2 = iabs(qscale_[mb_pos - stride]);
    if (q2 && q2 != q1) a = scale_pred(a, kDcScale[q2], kDqScale[dq_index]);
  }
  if (a_avail && c_avail && n != 3) {
    uint64_t off = mb_pos;
    if (n != 1) off--;
    if (n != 2) off -= stride;
    const int q2 = iabs(qscale_[off]);
    if (q2 && q2 != q1) b = scale_pred(b, kDcScale[q2], kDqScale[dq_index]);
  }
  int pred;
  if (c_avail && (!a_avail || iabs(a - b) <= iabs(b - c))) {
    pred = c;
    *dir = 1;
  } else if (a_avail) {
    pred = a;
    *dir = 0;
  } else {
    pred = 0;
    *dir = 1;
  }
  return pred;
}

// 8.1.1.4 the coded block flag of a luma block of an I picture is predicted from its neighbours.
int Core::coded_block_pred(int n, int diff) {
  const BlockPos p = block_pos(mb_x_, mb_y_, mb_w_, n);
  auto get = [&](int dx, int dy) -> int {
    const int x = p.x + dx, y = p.y + dy;
    if (x < 0 || y < 0) return 0;
    return coded_[static_cast<uint64_t>(y) * static_cast<uint64_t>(p.wrap) + static_cast<uint64_t>(x)];
  };
  const int a = get(-1, 0), b = get(-1, -1), c = get(0, -1);
  const int pred = b == c ? a : c;
  coded_[static_cast<uint64_t>(p.y) * static_cast<uint64_t>(p.wrap) + static_cast<uint64_t>(p.x)] = static_cast<uint8_t>(pred ^ diff);
  return pred ^ diff;
}

// ---- intra blocks --------------------------------------------------------------------------------------------------------------------

namespace {
// The AC prediction values of block `p` plus a neighbour offset: 16 values (the left set, then the top set); a neighbour outside the picture has zeros.
int16_t* ac_block(Buf<int16_t>* ac, const BlockPos& p, int dx, int dy, int16_t* zeros) {
  const int x = p.x + dx, y = p.y + dy;
  if (x < 0 || y < 0 || x >= p.wrap) return zeros;
  const uint64_t i = (static_cast<uint64_t>(y) * static_cast<uint64_t>(p.wrap) + static_cast<uint64_t>(x)) * 16;
  if (i + 16 > ac[p.plane].size()) return zeros;
  return &ac[p.plane][i];
}
}  // namespace

// 8.1.1.5 an intra block of an I picture, Simple and Main profile.
bool Core::decode_i_block(BitReader& br, int16_t* block, int n, bool coded, int codingset) {
  int dir = 0;
  int dcdiff = vlc_dc_[pic_.dc_table_index][n >= 4 ? 1 : 0].decode(br);
  if (dcdiff < 0) return false;
  if (dcdiff) {
    const int m = (pic_.pq == 1 || pic_.pq == 2) ? 3 - pic_.pq : 0;
    if (dcdiff == 119) {
      dcdiff = static_cast<int>(br.get(8 + m));
    } else if (m) {
      dcdiff = (dcdiff << m) + static_cast<int>(br.get(m)) - ((1 << m) - 1);
    }
    if (br.bit()) dcdiff = -dcdiff;
  }
  int16_t* dc_val = nullptr;
  dcdiff += pred_dc_i(n, &dc_val, &dir);
  *dc_val = s16(dcdiff);
  block[0] = s16(dcdiff * y_dc_scale_);

  const BlockPos p = block_pos(mb_x_, mb_y_, mb_w_, n);
  int16_t zeros[16] = {};
  int16_t* ac_val2 = ac_block(ac_, p, 0, 0, zeros);
  int16_t* ac_val = dir ? ac_block(ac_, p, -1, 0, zeros) : ac_block(ac_, p, 0, -1, zeros);
  const int scale = pic_.pq * 2 + (pic_.halfpq ? 1 : 0);
  const int left_sh = 0, top_sh = 3;
  if (coded) {
    int last = 0, skip, value;
    const uint8_t* zz = ac_pred_ ? (!dir ? zz_8x8_[2] : zz_8x8_[3]) : zz_8x8_[1];
    for (int i = 1; !last; ++i) {
      if (!decode_ac_coeff(br, &last, &skip, &value, codingset)) return false;
      i += skip;
      if (i > 63) break;
      block[zz[i]] = s16(value);
    }
    if (ac_pred_) {
      int sh;
      const int16_t* pv = ac_val;
      if (dir) {
        sh = left_sh;
      } else {
        sh = top_sh;
        pv = ac_val + 8;
      }
      for (int k = 1; k < 8; ++k) block[k << sh] = s16(block[k << sh] + pv[k]);
    }
    for (int k = 1; k < 8; ++k) {
      ac_val2[k] = block[k << left_sh];
      ac_val2[k + 8] = block[k << top_sh];
    }
    for (int k = 1; k < 64; ++k)
      if (block[k]) {
        block[k] = s16(block[k] * scale);
        if (!pic_.pquantizer) block[k] = s16(block[k] + (block[k] < 0 ? -pic_.pq : pic_.pq));
      }
  } else {
    for (int k = 0; k < 16; ++k) ac_val2[k] = 0;
    if (ac_pred_) {
      int sh;
      int16_t* src = ac_val;
      int16_t* dst = ac_val2;
      if (dir) {
        sh = left_sh;
      } else {
        sh = top_sh;
        src = ac_val + 8;
        dst = ac_val2 + 8;
      }
      for (int k = 0; k < 8; ++k) dst[k] = src[k];
      for (int k = 1; k < 8; ++k) {
        block[k << sh] = s16(src[k] * scale);
        if (!pic_.pquantizer && block[k << sh]) block[k << sh] = s16(block[k << sh] + (block[k << sh] < 0 ? -pic_.pq : pic_.pq));
      }
    }
  }
  return !br.error();
}

// 8.1.1.5 an intra block of an I picture, Advanced profile (quantizers per macroblock, predictors scaled between them).
bool Core::decode_i_block_adv(BitReader& br, int16_t* block, int n, bool coded, int codingset, int mquant) {
  int dir = 0;
  const bool a_avail = a_avail_ != 0, c_avail = c_avail_ != 0;
  bool use_pred = ac_pred_;
  const uint64_t mb_pos = static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_);
  const int quant = iabs(mquant);
  int dcdiff = vlc_dc_[pic_.dc_table_index][n >= 4 ? 1 : 0].decode(br);
  if (dcdiff < 0) return false;
  if (dcdiff) {
    const int m = (quant == 1 || quant == 2) ? 3 - quant : 0;
    if (dcdiff == 119) {
      dcdiff = static_cast<int>(br.get(8 + m));
    } else if (m) {
      dcdiff = (dcdiff << m) + static_cast<int>(br.get(m)) - ((1 << m) - 1);
    }
    if (br.bit()) dcdiff = -dcdiff;
  }
  int16_t* dc_val = nullptr;
  dcdiff += pred_dc(n, quant, a_avail, c_avail, &dc_val, &dir);
  *dc_val = s16(dcdiff);
  block[0] = s16(dcdiff * y_dc_scale_);
  if (!a_avail && !c_avail) use_pred = false;
  const int scale = quant * 2 + (mquant < 0 ? 0 : (pic_.halfpq ? 1 : 0));
  const BlockPos p = block_pos(mb_x_, mb_y_, mb_w_, n);
  int16_t zeros[16] = {};
  int16_t* ac_val2 = ac_block(ac_, p, 0, 0, zeros);
  int16_t* ac_val = dir ? ac_block(ac_, p, -1, 0, zeros) : ac_block(ac_, p, 0, -1, zeros);
  int q1 = qscale_[mb_pos], q2 = 0;
  if (n == 3) {
    q2 = q1;
  } else if (dir) {
    if (n == 1) q2 = q1;
    else if (c_avail && mb_pos) q2 = qscale_[mb_pos - 1];
  } else {
    if (n == 2) q2 = q1;
    else if (a_avail && mb_pos >= static_cast<uint64_t>(mb_w_)) q2 = qscale_[mb_pos - static_cast<uint64_t>(mb_w_)];
  }
  const int left_sh = 0, top_sh = 3;
  const int halfpq = pic_.halfpq ? 1 : 0;
  if (coded) {
    int last = 0, skip, value;
    const uint8_t* zz;
    if (ac_pred_) zz = !dir ? zz_8x8_[2] : zz_8x8_[3];
    else zz = zz_8x8_[1];
    for (int i = 1; !last; ++i) {
      if (!decode_ac_coeff(br, &last, &skip, &value, codingset)) return false;
      i += skip;
      if (i > 63) break;
      block[zz[i]] = s16(value);
    }
    if (use_pred) {
      int sh;
      const int16_t* pv = ac_val;
      if (dir) {
        sh = left_sh;
      } else {
        sh = top_sh;
        pv = ac_val + 8;
      }
      q1 = iabs(q1) * 2 + (q1 < 0 ? 0 : halfpq) - 1;
      if (q1 < 1) return false;
      if (q2) q2 = iabs(q2) * 2 + (q2 < 0 ? 0 : halfpq) - 1;
      if (q2 && q1 != q2) {
        for (int k = 1; k < 8; ++k) block[k << sh] = s16(block[k << sh] + scale_pred(pv[k], q2, kDqScale[q1 - 1]));
      } else {
        for (int k = 1; k < 8; ++k) block[k << sh] = s16(block[k << sh] + pv[k]);
      }
    }
    for (int k = 1; k < 8; ++k) {
      ac_val2[k] = block[k << left_sh];
      ac_val2[k + 8] = block[k << top_sh];
    }
    for (int k = 1; k < 64; ++k)
      if (block[k]) {
        block[k] = s16(block[k] * scale);
        if (!pic_.pquantizer) block[k] = s16(block[k] + (block[k] < 0 ? -quant : quant));
      }
  } else {
    for (int k = 0; k < 16; ++k) ac_val2[k] = 0;
    if (use_pred) {
      int sh;
      int16_t* src = ac_val;
      int16_t* dst = ac_val2;
      if (dir) {
        sh = left_sh;
      } else {
        sh = top_sh;
        src = ac_val + 8;
        dst = ac_val2 + 8;
      }
      for (int k = 0; k < 8; ++k) dst[k] = src[k];
      q1 = iabs(q1) * 2 + (q1 < 0 ? 0 : halfpq) - 1;
      if (q1 < 1) return false;
      if (q2) q2 = iabs(q2) * 2 + (q2 < 0 ? 0 : halfpq) - 1;
      if (q2 && q1 != q2) {
        for (int k = 1; k < 8; ++k) dst[k] = s16(scale_pred(dst[k], q2, kDqScale[q1 - 1]));
      }
      for (int k = 1; k < 8; ++k) {
        block[k << sh] = s16(dst[k] * scale);
        if (!pic_.pquantizer && block[k << sh]) block[k << sh] = s16(block[k << sh] + (block[k << sh] < 0 ? -quant : quant));
      }
    }
  }
  return !br.error();
}

// 8.1.1.5 an intra block in a P or B picture.
bool Core::decode_intra_block(BitReader& br, int16_t* block, int n, bool coded, int mquant, int codingset) {
  int dir = 0;
  const bool a_avail = a_avail_ != 0, c_avail = c_avail_ != 0;
  bool use_pred = ac_pred_;
  const uint64_t mb_pos = static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_);
  for (int k = 0; k < 64; ++k) block[k] = 0;
  int quant = iabs(mquant);
  quant = quant > 31 ? 31 : quant;
  y_dc_scale_ = kDcScale[quant];
  int dcdiff = vlc_dc_[pic_.dc_table_index][n >= 4 ? 1 : 0].decode(br);
  if (dcdiff < 0) return false;
  if (dcdiff) {
    const int m = (quant == 1 || quant == 2) ? 3 - quant : 0;
    if (dcdiff == 119) {
      dcdiff = static_cast<int>(br.get(8 + m));
    } else if (m) {
      dcdiff = (dcdiff << m) + static_cast<int>(br.get(m)) - ((1 << m) - 1);
    }
    if (br.bit()) dcdiff = -dcdiff;
  }
  int16_t* dc_val = nullptr;
  dcdiff += pred_dc(n, quant, a_avail, c_avail, &dc_val, &dir);
  *dc_val = s16(dcdiff);
  block[0] = s16(dcdiff * y_dc_scale_);
  if (!a_avail) dir = 1;
  if (!c_avail) dir = 0;
  if (!a_avail && !c_avail) use_pred = false;
  const BlockPos p = block_pos(mb_x_, mb_y_, mb_w_, n);
  int16_t zeros[16] = {};
  int16_t* ac_val2 = ac_block(ac_, p, 0, 0, zeros);
  int16_t* ac_val = dir ? ac_block(ac_, p, -1, 0, zeros) : ac_block(ac_, p, 0, -1, zeros);
  const int halfpq = pic_.halfpq ? 1 : 0;
  const int scale = quant * 2 + (mquant < 0 ? 0 : halfpq);
  int q1 = qscale_[mb_pos], q2 = 0;
  if (dir && c_avail && mb_pos) q2 = qscale_[mb_pos - 1];
  if (!dir && a_avail && mb_pos >= static_cast<uint64_t>(mb_w_)) q2 = qscale_[mb_pos - static_cast<uint64_t>(mb_w_)];
  if (dir && n == 1) q2 = q1;
  if (!dir && n == 2) q2 = q1;
  if (n == 3) q2 = q1;
  const int left_sh = 0, top_sh = 3;
  if (coded) {
    int last = 0, skip, value;
    for (int i = 1; !last; ++i) {
      if (!decode_ac_coeff(br, &last, &skip, &value, codingset)) return false;
      i += skip;
      if (i > 63) break;
      block[zz_8x8_[0][i]] = s16(value);
    }
    if (use_pred) {
      q1 = iabs(q1) * 2 + (q1 < 0 ? 0 : halfpq) - 1;
      if (q1 < 1) return false;
      if (q2) q2 = iabs(q2) * 2 + (q2 < 0 ? 0 : halfpq) - 1;
      const int16_t* pv = dir ? ac_val : ac_val + 8;
      const int sh = dir ? left_sh : top_sh;
      if (q2 && q1 != q2) {
        for (int k = 1; k < 8; ++k) block[k << sh] = s16(block[k << sh] + scale_pred(pv[k], q2, kDqScale[q1 - 1]));
      } else {
        for (int k = 1; k < 8; ++k) block[k << sh] = s16(block[k << sh] + pv[k]);
      }
    }
    for (int k = 1; k < 8; ++k) {
      ac_val2[k] = block[k << left_sh];
      ac_val2[k + 8] = block[k << top_sh];
    }
    for (int k = 1; k < 64; ++k)
      if (block[k]) {
        block[k] = s16(block[k] * scale);
        if (!pic_.pquantizer) block[k] = s16(block[k] + (block[k] < 0 ? -quant : quant));
      }
  } else {
    for (int k = 0; k < 16; ++k) ac_val2[k] = 0;
    if (dir) {
      if (use_pred) {
        for (int k = 0; k < 8; ++k) ac_val2[k] = ac_val[k];
        q1 = iabs(q1) * 2 + (q1 < 0 ? 0 : halfpq) - 1;
        if (q1 < 1) return false;
        if (q2) q2 = iabs(q2) * 2 + (q2 < 0 ? 0 : halfpq) - 1;
        if (q2 && q1 != q2)
          for (int k = 1; k < 8; ++k) ac_val2[k] = s16(scale_pred(ac_val2[k], q2, kDqScale[q1 - 1]));
      }
    } else {
      if (use_pred) {
        for (int k = 0; k < 8; ++k) ac_val2[8 + k] = ac_val[8 + k];
        q1 = iabs(q1) * 2 + (q1 < 0 ? 0 : halfpq) - 1;
        if (q1 < 1) return false;
        if (q2) q2 = iabs(q2) * 2 + (q2 < 0 ? 0 : halfpq) - 1;
        if (q2 && q1 != q2)
          for (int k = 1; k < 8; ++k) ac_val2[k + 8] = s16(scale_pred(ac_val2[k + 8], q2, kDqScale[q1 - 1]));
      }
    }
    if (use_pred) {
      const int sh = dir ? left_sh : top_sh;
      const int16_t* src = dir ? ac_val2 : ac_val2 + 8;
      for (int k = 1; k < 8; ++k) {
        block[k << sh] = s16(src[k] * scale);
        if (!pic_.pquantizer && block[k << sh]) block[k << sh] = s16(block[k << sh] + (block[k << sh] < 0 ? -quant : quant));
      }
    }
  }
  return !br.error();
}


// ---- motion vectors ------------------------------------------------------------------------------------------------------------------

// 8.3.5.2 MVDATA: the differential motion vector, whether the macroblock has coefficients and whether it is intra.
bool Core::read_mvdata(BitReader& br, int* dmv_x, int* dmv_y, bool* has_coeffs, bool* intra) {
  int sym = vlc_mv_diff_[pic_.mv_table_index].decode(br);
  if (sym < 0) return false;
  int index = 1 + sym;
  if (index > 36) {
    *has_coeffs = true;
    index -= 37;
  } else {
    *has_coeffs = false;
  }
  *intra = false;
  const int qs = pic_.quarter_sample ? 1 : 0;
  if (!index) {
    *dmv_x = *dmv_y = 0;
  } else if (index == 35) {
    *dmv_x = static_cast<int>(br.get(pic_.k_x - 1 + qs));
    *dmv_y = static_cast<int>(br.get(pic_.k_y - 1 + qs));
  } else if (index == 36) {
    *dmv_x = *dmv_y = 0;
    *intra = true;
  } else {
    int index1 = index % 6;
    *dmv_x = kOffsetTable[index1];
    int val = kSizeTable[index1] - ((!qs && index1 == 5) ? 1 : 0);
    if (val > 0) {
      val = static_cast<int>(br.get(val));
      const int sign = 0 - (val & 1);
      *dmv_x = (sign ^ ((val >> 1) + *dmv_x)) - sign;
    }
    index1 = index / 6;
    *dmv_y = kOffsetTable[index1];
    val = kSizeTable[index1] - ((!qs && index1 == 5) ? 1 : 0);
    if (val > 0) {
      val = static_cast<int>(br.get(val));
      const int sign = 0 - (val & 1);
      *dmv_y = (sign ^ ((val >> 1) + *dmv_y)) - sign;
    }
  }
  return !br.error();
}

// 8.3.5.3 motion vector prediction of a P picture macroblock (n: the block, mv1: the vector covers the whole macroblock).
void Core::pred_mv(BitReader& br, int n, int dmv_x, int dmv_y, bool mv1, int r_x, int r_y) {
  const int bw = 2 * mb_w_;
  const int bx = 2 * mb_x_ + (n & 1), by = 2 * mb_y_ + (n >> 1);
  auto mv_at = [&](int x, int y) -> int16_t* { return &mv_[(static_cast<uint64_t>(y) * static_cast<uint64_t>(bw) + static_cast<uint64_t>(x)) * 2]; };
  if (!pic_.quarter_sample) {
    dmv_x *= 2;
    dmv_y *= 2;
  }
  if (mb_intra_) {
    int16_t* mv = mv_at(bx, by);
    mv[0] = mv[1] = 0;
    mv_cur_[0][n][0] = mv_cur_[0][n][1] = 0;
    if (mv1) {
      for (int k = 1; k < 4; ++k) {
        int16_t* m = mv_at(2 * mb_x_ + (k & 1), 2 * mb_y_ + (k >> 1));
        m[0] = m[1] = 0;
      }
      uvmv_[(static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_)) * 2] = 0;
      uvmv_[(static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_)) * 2 + 1] = 0;
    }
    return;
  }
  bool a_valid = !first_slice_line_ || n == 2 || n == 3;
  bool b_valid = a_valid;
  const bool c_valid = mb_x_ != 0 || n == 1 || n == 3;
  int off = 0;
  if (mv1) {
    off = mb_x_ == mb_w_ - 1 ? -1 : 2;
    b_valid = b_valid && mb_w_ > 1;
  } else {
    switch (n) {
      case 0: off = mb_x_ ? -1 : 1; break;
      case 1: off = mb_x_ == mb_w_ - 1 ? -1 : 1; break;
      case 2: off = 1; break;
      default: off = -1; break;
    }
  }
  int pa[2] = {0, 0}, pb[2] = {0, 0}, pc[2] = {0, 0};
  int valid = 0;
  if (a_valid) {
    const int16_t* m = mv_at(bx, by - 1);
    pa[0] = m[0];
    pa[1] = m[1];
    ++valid;
  }
  if (b_valid) {
    const int16_t* m = mv_at(bx + off, by - 1);
    pb[0] = m[0];
    pb[1] = m[1];
    ++valid;
  }
  if (c_valid) {
    const int16_t* m = mv_at(bx - 1, by);
    pc[0] = m[0];
    pc[1] = m[1];
    ++valid;
  }
  int px, py;
  if (a_valid) {
    px = pa[0];
    py = pa[1];
  } else if (c_valid) {
    px = pc[0];
    py = pc[1];
  } else if (b_valid) {
    px = pb[0];
    py = pb[1];
  } else {
    px = py = 0;
  }
  if (valid > 1) {
    px = mid_pred(pa[0], pb[0], pc[0]);
    py = mid_pred(pa[1], pb[1], pc[1]);
  }
  {   // pull the vector back into the picture (8.3.5.3.4)
    const int mvlim = mv1 ? -60 : -28;
    const int qx = (mb_x_ << 6) + ((n == 1 || n == 3) ? 32 : 0);
    const int qy = (mb_y_ << 6) + ((n == 2 || n == 3) ? 32 : 0);
    const int X = (mb_w_ << 6) - 4, Y = (mb_h_ << 6) - 4;
    if (qx + px < mvlim) px = mvlim - qx;
    if (qy + py < mvlim) py = mvlim - qy;
    if (qx + px > X) px = X - qx;
    if (qy + py > Y) py = Y - qy;
  }
  if (a_valid && c_valid) {   // hybrid prediction (8.3.5.3.5)
    auto intra_at = [&](int x, int y) {
      const uint64_t mb = static_cast<uint64_t>(y >> 1) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(x >> 1);
      return (intra_mask_[mb] >> (((y & 1) << 1) | (x & 1))) & 1;
    };
    int sum;
    if (intra_at(bx, by - 1)) sum = iabs(px) + iabs(py);
    else sum = iabs(px - pa[0]) + iabs(py - pa[1]);
    if (sum > 32) {
      if (br.bit()) {
        px = pa[0];
        py = pa[1];
      } else {
        px = pc[0];
        py = pc[1];
      }
    } else {
      if (intra_at(bx - 1, by)) sum = iabs(px) + iabs(py);
      else sum = iabs(px - pc[0]) + iabs(py - pc[1]);
      if (sum > 32) {
        if (br.bit()) {
          px = pa[0];
          py = pa[1];
        } else {
          px = pc[0];
          py = pc[1];
        }
      }
    }
  }
  const int mx = ((px + dmv_x + r_x) & ((r_x << 1) - 1)) - r_x;
  const int my = ((py + dmv_y + r_y) & ((r_y << 1) - 1)) - r_y;
  mv_cur_[0][n][0] = s16(mx);
  mv_cur_[0][n][1] = s16(my);
  int16_t* m = mv_at(bx, by);
  m[0] = s16(mx);
  m[1] = s16(my);
  if (mv1) {
    for (int k = 1; k < 4; ++k) {
      int16_t* d = mv_at(2 * mb_x_ + (k & 1), 2 * mb_y_ + (k >> 1));
      d[0] = m[0];
      d[1] = m[1];
    }
  }
}

namespace {
// 8.4.5.3 scaling of the co-located vector by BFRACTION.
int scale_mv(int value, int bfrac, int inv, bool quarter) {
  int n = bfrac;
  if (inv) n -= 256;
  if (!quarter) return 2 * ((value * n + 255) >> 9);
  return (value * n + 128) >> 8;
}
int clip_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
}  // namespace

// 8.4.5 motion vector prediction of a B picture macroblock; the vectors of both directions are left in mv_cur_[0][0] and mv_cur_[1][0].
void Core::pred_b_mv(int dmv_x[2], int dmv_y[2], bool direct, int mvtype) {
  const int r_x = pic_.range_x, r_y = pic_.range_y;
  const int bw = 2 * mb_w_;
  const int bx = 2 * mb_x_, by = 2 * mb_y_;
  auto mv_at = [&](Buf<int16_t>* arr, int x, int y) -> int16_t* { return &(*arr)[(static_cast<uint64_t>(y) * static_cast<uint64_t>(bw) + static_cast<uint64_t>(x)) * 2]; };
  if (!pic_.quarter_sample) {
    dmv_x[0] *= 2;
    dmv_y[0] *= 2;
    dmv_x[1] *= 2;
    dmv_y[1] *= 2;
  }
  if (mb_intra_) {
    mv_at(&mv_, bx, by)[0] = mv_at(&mv_, bx, by)[1] = 0;
    mv_at(&mv_back_, bx, by)[0] = mv_at(&mv_back_, bx, by)[1] = 0;
    return;
  }
  const uint64_t mb_pos = static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_);
  const Picture* next = anchor_[1];
  const int nx = next ? next->direct_mv[mb_pos * 2] : 0, ny = next ? next->direct_mv[mb_pos * 2 + 1] : 0;
  const bool qs = pic_.quarter_sample;
  int mvf[2] = {scale_mv(nx, pic_.bfraction, 0, qs), scale_mv(ny, pic_.bfraction, 0, qs)};
  int mvb[2] = {scale_mv(nx, pic_.bfraction, 1, qs), scale_mv(ny, pic_.bfraction, 1, qs)};
  mvf[0] = clip_int(mvf[0], -60 - (mb_x_ << 6), (mb_w_ << 6) - 4 - (mb_x_ << 6));
  mvf[1] = clip_int(mvf[1], -60 - (mb_y_ << 6), (mb_h_ << 6) - 4 - (mb_y_ << 6));
  mvb[0] = clip_int(mvb[0], -60 - (mb_x_ << 6), (mb_w_ << 6) - 4 - (mb_x_ << 6));
  mvb[1] = clip_int(mvb[1], -60 - (mb_y_ << 6), (mb_h_ << 6) - 4 - (mb_y_ << 6));
  auto store = [&]() {
    int16_t* f = mv_at(&mv_, bx, by);
    int16_t* b = mv_at(&mv_back_, bx, by);
    f[0] = s16(mvf[0]);
    f[1] = s16(mvf[1]);
    b[0] = s16(mvb[0]);
    b[1] = s16(mvb[1]);
    mv_cur_[0][0][0] = s16(mvf[0]);
    mv_cur_[0][0][1] = s16(mvf[1]);
    mv_cur_[1][0][0] = s16(mvb[0]);
    mv_cur_[1][0][1] = s16(mvb[1]);
  };
  if (direct) {
    store();
    return;
  }
  const int sh = seq_.profile < kAdvanced ? 5 : 6;
  for (int dir = 0; dir < 2; ++dir) {
    if (!(dir == 0 ? (mvtype == kBmvForward || mvtype == kBmvInterpolated) : (mvtype == kBmvBackward || mvtype == kBmvInterpolated))) continue;
    Buf<int16_t>* arr = dir == 0 ? &mv_ : &mv_back_;
    int cx = 0, cy = 0, ax = 0, ay = 0, ex = 0, ey = 0;
    if (mb_x_) {
      const int16_t* c = mv_at(arr, bx - 2, by);
      cx = c[0];
      cy = c[1];
    }
    if (!first_slice_line_) {
      const int16_t* a = mv_at(arr, bx, by - 2);
      ax = a[0];
      ay = a[1];
      const int off = mb_x_ == mb_w_ - 1 ? -2 : 2;
      const int16_t* b = mv_at(arr, bx + off, by - 2);
      ex = b[0];
      ey = b[1];
    }
    int px, py;
    if (!first_slice_line_) {
      if (mb_w_ == 1) {
        px = ax;
        py = ay;
      } else {
        px = mid_pred(ax, ex, cx);
        py = mid_pred(ay, ey, cy);
      }
    } else if (mb_x_) {
      px = cx;
      py = cy;
    } else {
      px = py = 0;
    }
    {
      const int mvlim = 4 - (1 << sh);
      const int qx = mb_x_ << sh, qy = mb_y_ << sh;
      const int X = (mb_w_ << sh) - 4, Y = (mb_h_ << sh) - 4;
      if (qx + px < mvlim) px = mvlim - qx;
      if (qy + py < mvlim) py = mvlim - qy;
      if (qx + px > X) px = X - qx;
      if (qy + py > Y) py = Y - qy;
    }
    const int vx = ((px + dmv_x[dir] + r_x) & ((r_x << 1) - 1)) - r_x;
    const int vy = ((py + dmv_y[dir] + r_y) & ((r_y << 1) - 1)) - r_y;
    if (dir == 0) {
      mvf[0] = vx;
      mvf[1] = vy;
    } else {
      mvb[0] = vx;
      mvb[1] = vy;
    }
  }
  store();
}

// ---- inter blocks --------------------------------------------------------------------------------------------------------------------

// 8.1.1.7 the residual of an inter block: transform type, sub-block pattern, coefficients; adds it to the prediction in `plane` at `offset`. Returns the pattern of the
// coded sub-blocks (4 bits, a set bit: coded) or -1.
int Core::decode_p_block(BitReader& br, int16_t* block, int n, int mquant, int ttmb, bool first_block, ByteView plane, uint64_t offset, int stride, uint32_t* ttmb_out) {
  (void)n;
  for (int k = 0; k < 64; ++k) block[k] = 0;
  const int quant = iabs(mquant);
  int ttblk = ttmb & 7;
  int subblkpat = 0;
  int pat = 0;
  if (ttmb == -1) {
    const int sym = vlc_ttblk_[pic_.tt_index].decode(br);
    if (sym < 0) return -1;
    ttblk = kTtblkToTt[pic_.tt_index * 8 + sym];
  }
  if (ttblk == kTt4x4) {
    const int sym = vlc_subblkpat_[pic_.tt_index].decode(br);
    if (sym < 0) return -1;
    subblkpat = ~(sym + 1);
  }
  if ((ttblk != kTt8x8 && ttblk != kTt4x4) && ((pic_.ttmbf || (ttmb != -1 && (ttmb & 8) && !first_block)) || (!seq_.res_rtm_flag && !first_block))) {
    subblkpat = br.decode012();
    if (subblkpat) subblkpat ^= 3;
    if (ttblk == kTt8x4Top || ttblk == kTt8x4Bottom) ttblk = kTt8x4;
    if (ttblk == kTt4x8Right || ttblk == kTt4x8Left) ttblk = kTt4x8;
  }
  const int scale = quant * 2 + (mquant < 0 ? 0 : (pic_.halfpq ? 1 : 0));
  if (ttblk == kTt8x4Top || ttblk == kTt8x4Bottom) {
    subblkpat = 2 - (ttblk == kTt8x4Top);
    ttblk = kTt8x4;
  }
  if (ttblk == kTt4x8Right || ttblk == kTt4x8Left) {
    subblkpat = 2 - (ttblk == kTt4x8Left);
    ttblk = kTt4x8;
  }
  auto dequant = [&](int idx, int value) {
    block[idx] = s16(value * scale);
    if (!pic_.pquantizer) block[idx] = s16(block[idx] + (block[idx] < 0 ? -quant : quant));
  };
  int last, skip, value;
  switch (ttblk) {
    case kTt8x8: {
      pat = 0xF;
      int i = 0;
      last = 0;
      while (!last) {
        if (!decode_ac_coeff(br, &last, &skip, &value, codingset2_)) return -1;
        i += skip;
        if (i > 63) break;
        dequant(zz_8x8_[0][i++], value);
      }
      inv_trans_8x8(block);
      add_block_clamped(plane, offset, stride, block);
      break;
    }
    case kTt4x4: {
      pat = ~subblkpat & 0xF;
      for (int j = 0; j < 4; ++j) {
        last = subblkpat & (1 << (3 - j));
        int i = 0;
        const int off = (j & 1) * 4 + (j & 2) * 16;
        while (!last) {
          if (!decode_ac_coeff(br, &last, &skip, &value, codingset2_)) return -1;
          i += skip;
          if (i > 15) break;
          dequant(kScan4x4[i++] + off, value);
        }
        if (!(subblkpat & (1 << (3 - j)))) inv_trans_4x4_add(plane, offset + static_cast<uint64_t>((j & 1) * 4) + static_cast<uint64_t>((j & 2) * 2) * static_cast<uint64_t>(stride), stride, block + off);
      }
      break;
    }
    case kTt8x4: {
      pat = ~((subblkpat & 2) * 6 + (subblkpat & 1) * 3) & 0xF;
      for (int j = 0; j < 2; ++j) {
        last = subblkpat & (1 << (1 - j));
        int i = 0;
        const int off = j * 32;
        while (!last) {
          if (!decode_ac_coeff(br, &last, &skip, &value, codingset2_)) return -1;
          i += skip;
          if (i > 31) break;
          dequant(zz_8x4_[i++] + off, value);
        }
        if (!(subblkpat & (1 << (1 - j)))) inv_trans_8x4_add(plane, offset + static_cast<uint64_t>(j * 4) * static_cast<uint64_t>(stride), stride, block + off);
      }
      break;
    }
    case kTt4x8: {
      pat = ~(subblkpat * 5) & 0xF;
      for (int j = 0; j < 2; ++j) {
        last = subblkpat & (1 << (1 - j));
        int i = 0;
        const int off = j * 4;
        while (!last) {
          if (!decode_ac_coeff(br, &last, &skip, &value, codingset2_)) return -1;
          i += skip;
          if (i > 31) break;
          dequant(zz_4x8_[i++] + off, value);
        }
        if (!(subblkpat & (1 << (1 - j)))) inv_trans_4x8_add(plane, offset + static_cast<uint64_t>(j * 4), stride, block + off);
      }
      break;
    }
    default:
      return -1;
  }
  if (ttmb_out) *ttmb_out |= static_cast<uint32_t>(ttblk) << (n * 4);
  return pat;
}

// ---- macroblocks ---------------------------------------------------------------------------------------------------------------------

// The positions of the six blocks of the current macroblock in the planes of the picture: block n -> plane, byte offset, stride.
void Core::block_target(int n, int* plane, uint64_t* offset, int* stride) const {
  if (n < 4) {
    *plane = 0;
    *stride = cur_->stride[0];
    *offset = static_cast<uint64_t>(mb_y_ * 16 + (n >> 1) * 8) * static_cast<uint64_t>(*stride) + static_cast<uint64_t>(mb_x_ * 16 + (n & 1) * 8);
  } else {
    *plane = n - 3;
    *stride = cur_->stride[1];
    *offset = static_cast<uint64_t>(mb_y_ * 8) * static_cast<uint64_t>(*stride) + static_cast<uint64_t>(mb_x_ * 8);
  }
}

void Core::select_coding_sets(bool intra_picture) {
  const int ac = intra_picture ? pic_.y_ac_table_index : pic_.c_ac_table_index;
  switch (ac) {
    case 0: codingset_ = pic_.pqindex <= 8 ? kCsHighRateIntra : kCsLowMotIntra; break;
    case 1: codingset_ = kCsHighMotIntra; break;
    default: codingset_ = kCsMidRateIntra; break;
  }
  switch (pic_.c_ac_table_index) {
    case 0: codingset2_ = pic_.pqindex <= 8 ? kCsHighRateInter : kCsLowMotInter; break;
    case 1: codingset2_ = kCsHighMotInter; break;
    default: codingset2_ = kCsMidRateInter; break;
  }
}

// Stores the intra block n of the current macroblock (its samples after the inverse transform) until the overlap filter and the put step.
void Core::keep_intra_block(int n, const int16_t* block) {
  const uint64_t mb_pos = static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_);
  for (int k = 0; k < 64; ++k) blocks_[(mb_pos * 6 + static_cast<uint64_t>(n)) * 64 + static_cast<uint64_t>(k)] = block[k];
}

bool Core::decode_slice_blocks(BitReader& br, int start_mb_y, int end_mb_y) {
  start_mb_y_ = start_mb_y;
  end_mb_y_ = end_mb_y;
  esc3_level_length_ = 0;
  const bool intra_picture = pic_.type == kPictureI || pic_.type == kPictureBI;
  select_coding_sets(intra_picture);
  const bool adv = seq_.profile == kAdvanced;
  if (intra_picture && adv && start_mb_y > 0) {
    // The coded block flags of the row above the slice do not predict.
    const int bw = 2 * mb_w_;
    for (int x = 0; x < bw; ++x) coded_[static_cast<uint64_t>(2 * start_mb_y - 1) * static_cast<uint64_t>(bw) + static_cast<uint64_t>(x)] = 0;
  }
  if (intra_picture && !adv) y_dc_scale_ = kDcScale[pic_.pq];
  for (mb_y_ = start_mb_y; mb_y_ < end_mb_y; ++mb_y_) {
    first_slice_line_ = mb_y_ == start_mb_y;
    slice_start_row_[static_cast<uint64_t>(mb_y_)] = first_slice_line_ ? 1 : 0;
    for (mb_x_ = 0; mb_x_ < mb_w_; ++mb_x_) {
      bool ok;
      if (intra_picture) ok = adv ? decode_i_mb_adv(br) : decode_i_mb(br);
      else if (pic_.type == kPictureP) ok = decode_p_mb(br);
      else ok = decode_b_mb(br);
      if (!ok || br.overrun()) return false;
    }
  }
  return true;
}

bool Core::decode_i_mb(BitReader& br) {
  const uint64_t mb_pos = static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_);
  qscale_[mb_pos] = static_cast<int8_t>(pic_.pq);
  intra_mask_[mb_pos] = 0x3F;
  cur_->direct_mv[mb_pos * 2] = cur_->direct_mv[mb_pos * 2 + 1] = 0;
  int cbp = vlc_intra_cbpcy_.decode(br);
  if (cbp < 0) return false;
  ac_pred_ = br.bit() != 0;
  for (int k = 0; k < 6; ++k) {
    int val = (cbp >> (5 - k)) & 1;
    if (k < 4) val = coded_block_pred(k, val);
    cbp |= val << (5 - k);
    for (int i = 0; i < 64; ++i) cur_blk_[k][i] = 0;
    if (!decode_i_block(br, cur_blk_[k], k, val != 0, k < 4 ? codingset_ : codingset2_)) return false;
    inv_trans_8x8(cur_blk_[k]);
    keep_intra_block(k, cur_blk_[k]);
  }
  return true;
}

bool Core::decode_i_mb_adv(BitReader& br) {
  const uint64_t mb_pos = static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_);
  intra_mask_[mb_pos] = 0x3F;
  cur_->direct_mv[mb_pos * 2] = cur_->direct_mv[mb_pos * 2 + 1] = 0;
  int mquant = pic_.pq;
  int cbp = vlc_intra_cbpcy_.decode(br);
  if (cbp < 0) return false;
  ac_pred_ = pic_.acpred_raw ? br.bit() != 0 : plane_acpred_[mb_pos] != 0;
  if (pic_.condover == kCondOverSelect && pic_.overflags_raw) plane_overflags_[mb_pos] = static_cast<uint8_t>(br.bit());
  if (!decode_mquant(br, &mquant)) return false;
  qscale_[mb_pos] = static_cast<int8_t>(mquant);
  y_dc_scale_ = kDcScale[iabs(mquant)];
  for (int k = 0; k < 6; ++k) {
    int val = (cbp >> (5 - k)) & 1;
    if (k < 4) val = coded_block_pred(k, val);
    cbp |= val << (5 - k);
    a_avail_ = !first_slice_line_ || k == 2 || k == 3;
    c_avail_ = mb_x_ != 0 || k == 1 || k == 3;
    for (int i = 0; i < 64; ++i) cur_blk_[k][i] = 0;
    if (!decode_i_block_adv(br, cur_blk_[k], k, val != 0, k < 4 ? codingset_ : codingset2_, mquant)) return false;
    inv_trans_8x8(cur_blk_[k]);
    keep_intra_block(k, cur_blk_[k]);
  }
  return true;
}

bool Core::decode_p_mb(BitReader& br) {
  const uint64_t mb_pos = static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_);
  int mquant = pic_.pq;
  int ttmb = pic_.ttfrm;
  bool first_block = true;
  uint32_t block_cbp = 0, block_tt = 0;
  const int fourmv = pic_.mv_type_raw ? br.bit() : plane_mvtype_[mb_pos];
  const int skipped = pic_.skip_raw ? br.bit() : plane_skip_[mb_pos];
  bool has_coeffs = true;
  int dmv_x = 0, dmv_y = 0, cbp = 0;
  intra_mask_[mb_pos] = 0;
  auto block_intra = [&](int bx, int by) { return ((intra_mask_[static_cast<uint64_t>(by >> 1) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(bx >> 1)] >> (((by & 1) << 1) | (bx & 1))) & 1) != 0; };
  // Whether the blocks above and to the left of block i are intra (they predict then).
  auto set_avail = [&](int i) {
    a_avail_ = c_avail_ = 0;
    if (i < 4) {
      const int bx = 2 * mb_x_ + (i & 1), by = 2 * mb_y_ + (i >> 1);
      if (i == 2 || i == 3 || !first_slice_line_) a_avail_ = block_intra(bx, by - 1);
      if (i == 1 || i == 3 || mb_x_) c_avail_ = block_intra(bx - 1, by);
    } else {
      if (!first_slice_line_) a_avail_ = (intra_mask_[mb_pos - static_cast<uint64_t>(mb_w_)] >> i) & 1;
      if (mb_x_) c_avail_ = (intra_mask_[mb_pos - 1] >> i) & 1;
    }
  };
  auto clear_dc = [&]() {
    for (int i = 0; i < 6; ++i) {
      const BlockPos p = block_pos(mb_x_, mb_y_, mb_w_, i);
      dc_[p.plane][static_cast<uint64_t>(p.y) * static_cast<uint64_t>(p.wrap) + static_cast<uint64_t>(p.x)] = 0;
    }
  };
  if (!fourmv) {
    if (!skipped) {
      if (!read_mvdata(br, &dmv_x, &dmv_y, &has_coeffs, &mb_intra_)) return false;
      if (mb_intra_) cur_->direct_mv[mb_pos * 2] = cur_->direct_mv[mb_pos * 2 + 1] = 0;
      pred_mv(br, 0, dmv_x, dmv_y, true, pic_.range_x, pic_.range_y);
      if (mb_intra_ && !has_coeffs) {
        if (!decode_mquant(br, &mquant)) return false;
        ac_pred_ = br.bit() != 0;
        cbp = 0;
      } else if (has_coeffs) {
        if (mb_intra_) ac_pred_ = br.bit() != 0;
        cbp = vlc_cbpcy_[pic_.cbptab].decode(br);
        if (cbp < 0) return false;
        if (!decode_mquant(br, &mquant)) return false;
      } else {
        mquant = pic_.pq;
        cbp = 0;
      }
      qscale_[mb_pos] = static_cast<int8_t>(mquant);
      if (!pic_.ttmbf && !mb_intra_ && has_coeffs) {
        ttmb = vlc_ttmb_[pic_.tt_index].decode(br);
        if (ttmb < 0) return false;
      }
      if (!mb_intra_) mc_1mv(0);
      clear_dc();
      for (int i = 0; i < 6; ++i) {
        const int val = (cbp >> (5 - i)) & 1;
        if (mb_intra_) intra_mask_[mb_pos] = static_cast<uint8_t>(intra_mask_[mb_pos] | (1 << i));
        if (mb_intra_) {
          set_avail(i);
          for (int k = 0; k < 64; ++k) cur_blk_[i][k] = 0;
          if (!decode_intra_block(br, cur_blk_[i], i, val != 0, mquant, i >= 4 ? codingset2_ : codingset_)) return false;
          inv_trans_8x8(cur_blk_[i]);
          keep_intra_block(i, cur_blk_[i]);
          block_cbp |= 0xFu << (i << 2);
        } else if (val) {
          int plane;
          uint64_t offset;
          int stride;
          block_target(i, &plane, &offset, &stride);
          const int pat = decode_p_block(br, cur_blk_[i], i, mquant, ttmb, first_block, ByteView(cur_->plane[plane].data(), cur_->plane[plane].size()), offset, stride, &block_tt);
          if (pat < 0) return false;
          block_cbp |= static_cast<uint32_t>(pat) << (i << 2);
          if (!pic_.ttmbf && ttmb < 8) ttmb = -1;
          first_block = false;
        }
      }
    } else {   // skipped
      mb_intra_ = false;
      clear_dc();
      qscale_[mb_pos] = 0;
      pred_mv(br, 0, 0, 0, true, pic_.range_x, pic_.range_y);
      mc_1mv(0);
    }
  } else {   // 4MV
    if (!skipped) {
      bool is_intra[6], is_coded[6];
      int intra_count = 0;
      bool coded_inter = false;
      cbp = vlc_cbpcy_[pic_.cbptab].decode(br);
      if (cbp < 0) return false;
      clear_dc();
      for (int i = 0; i < 6; ++i) {
        const int val = (cbp >> (5 - i)) & 1;
        mb_intra_ = false;
        if (i < 4) {
          dmv_x = dmv_y = 0;
          bool coeffs = false;
          if (val && !read_mvdata(br, &dmv_x, &dmv_y, &coeffs, &mb_intra_)) return false;
          pred_mv(br, i, dmv_x, dmv_y, false, pic_.range_x, pic_.range_y);
          if (!mb_intra_) mc_4mv_luma(i);
          intra_count += mb_intra_ ? 1 : 0;
          is_intra[i] = mb_intra_;
          is_coded[i] = coeffs;
        } else {
          is_intra[i] = intra_count >= 3;
          is_coded[i] = val != 0;
        }
        if (i == 4) mc_4mv_chroma();
        if (is_intra[i]) intra_mask_[mb_pos] = static_cast<uint8_t>(intra_mask_[mb_pos] | (1 << i));
        if (!coded_inter) coded_inter = !is_intra[i] && is_coded[i];
      }
      if (intra_count || coded_inter) {
        if (!decode_mquant(br, &mquant)) return false;
        qscale_[mb_pos] = static_cast<int8_t>(mquant);
        bool intrapred = false;
        for (int i = 0; i < 6 && !intrapred; ++i) {
          if (!is_intra[i]) continue;
          set_avail(i);
          if (a_avail_ || c_avail_) intrapred = true;
        }
        ac_pred_ = intrapred ? br.bit() != 0 : false;
        if (!pic_.ttmbf && coded_inter) {
          ttmb = vlc_ttmb_[pic_.tt_index].decode(br);
          if (ttmb < 0) return false;
        }
        for (int i = 0; i < 6; ++i) {
          mb_intra_ = is_intra[i];
          if (is_intra[i]) {
            set_avail(i);
            for (int k = 0; k < 64; ++k) cur_blk_[i][k] = 0;
            if (!decode_intra_block(br, cur_blk_[i], i, is_coded[i], mquant, i >= 4 ? codingset2_ : codingset_)) return false;
            inv_trans_8x8(cur_blk_[i]);
            keep_intra_block(i, cur_blk_[i]);
            block_cbp |= 0xFu << (i << 2);
          } else if (is_coded[i]) {
            int plane;
            uint64_t offset;
            int stride;
            block_target(i, &plane, &offset, &stride);
            const int pat = decode_p_block(br, cur_blk_[i], i, mquant, ttmb, first_block, ByteView(cur_->plane[plane].data(), cur_->plane[plane].size()), offset, stride, &block_tt);
            if (pat < 0) return false;
            block_cbp |= static_cast<uint32_t>(pat) << (i << 2);
            if (!pic_.ttmbf && ttmb < 8) ttmb = -1;
            first_block = false;
          }
        }
      }
    } else {   // skipped 4MV
      mb_intra_ = false;
      clear_dc();
      qscale_[mb_pos] = 0;
      for (int i = 0; i < 4; ++i) {
        pred_mv(br, i, 0, 0, false, pic_.range_x, pic_.range_y);
        mc_4mv_luma(i);
      }
      mc_4mv_chroma();
    }
  }
  cbp_blocks_[mb_pos] = block_cbp;
  ttblk_[mb_pos] = block_tt;
  return !br.error();
}

bool Core::decode_b_mb(BitReader& br) {
  const uint64_t mb_pos = static_cast<uint64_t>(mb_y_) * static_cast<uint64_t>(mb_w_) + static_cast<uint64_t>(mb_x_);
  int mquant = pic_.pq;
  int ttmb = pic_.ttfrm;
  bool first_block = true;
  mb_intra_ = false;
  int cbp = 0, dmv_x[2] = {0, 0}, dmv_y[2] = {0, 0};
  bool has_coeffs = false;
  int bmvtype = kBmvBackward;
  const bool direct = pic_.direct_raw ? br.bit() != 0 : plane_direct_[mb_pos] != 0;
  const bool skipped = pic_.skip_raw ? br.bit() != 0 : plane_skip_[mb_pos] != 0;
  intra_mask_[mb_pos] = 0;
  for (int i = 0; i < 6; ++i) {
    const BlockPos p = block_pos(mb_x_, mb_y_, mb_w_, i);
    dc_[p.plane][static_cast<uint64_t>(p.y) * static_cast<uint64_t>(p.wrap) + static_cast<uint64_t>(p.x)] = 0;
  }
  qscale_[mb_pos] = 0;
  if (!direct) {
    if (!skipped) {
      if (!read_mvdata(br, &dmv_x[0], &dmv_y[0], &has_coeffs, &mb_intra_)) return false;
      dmv_x[1] = dmv_x[0];
      dmv_y[1] = dmv_y[0];
    }
    if (skipped || !mb_intra_) {
      bmvtype = br.decode012();
      switch (bmvtype) {
        case 0: bmvtype = pic_.bfraction >= 128 ? kBmvBackward : kBmvForward; break;
        case 1: bmvtype = pic_.bfraction >= 128 ? kBmvForward : kBmvBackward; break;
        default:
          bmvtype = kBmvInterpolated;
          dmv_x[0] = dmv_y[0] = 0;
          break;
      }
    }
  }
  if (mb_intra_) intra_mask_[mb_pos] = 0x3F;
  if (skipped) {
    if (direct) bmvtype = kBmvInterpolated;
    pred_b_mv(dmv_x, dmv_y, direct, bmvtype);
    b_mc(direct, bmvtype);
    return true;
  }
  if (direct) {
    cbp = vlc_cbpcy_[pic_.cbptab].decode(br);
    if (cbp < 0) return false;
    if (!decode_mquant(br, &mquant)) return false;
    mb_intra_ = false;
    qscale_[mb_pos] = static_cast<int8_t>(mquant);
    if (!pic_.ttmbf) {
      ttmb = vlc_ttmb_[pic_.tt_index].decode(br);
      if (ttmb < 0) return false;
    }
    dmv_x[0] = dmv_y[0] = dmv_x[1] = dmv_y[1] = 0;
    pred_b_mv(dmv_x, dmv_y, direct, bmvtype);
    b_mc(direct, bmvtype);
  } else {
    if (!has_coeffs && !mb_intra_) {   // no coded blocks: effectively skipped
      pred_b_mv(dmv_x, dmv_y, direct, bmvtype);
      b_mc(direct, bmvtype);
      return true;
    }
    if (mb_intra_ && !has_coeffs) {
      if (!decode_mquant(br, &mquant)) return false;
      qscale_[mb_pos] = static_cast<int8_t>(mquant);
      ac_pred_ = br.bit() != 0;
      cbp = 0;
      pred_b_mv(dmv_x, dmv_y, direct, bmvtype);
    } else {
      if (bmvtype == kBmvInterpolated) {
        bool dummy_intra = false;
        if (!read_mvdata(br, &dmv_x[0], &dmv_y[0], &has_coeffs, &dummy_intra)) return false;
        if (!has_coeffs) {   // an interpolated block without residual
          pred_b_mv(dmv_x, dmv_y, direct, bmvtype);
          b_mc(direct, bmvtype);
          return true;
        }
      }
      pred_b_mv(dmv_x, dmv_y, direct, bmvtype);
      if (!mb_intra_) b_mc(direct, bmvtype);
      if (mb_intra_) ac_pred_ = br.bit() != 0;
      cbp = vlc_cbpcy_[pic_.cbptab].decode(br);
      if (cbp < 0) return false;
      if (!decode_mquant(br, &mquant)) return false;
      qscale_[mb_pos] = static_cast<int8_t>(mquant);
      if (!pic_.ttmbf && !mb_intra_ && has_coeffs) {
        ttmb = vlc_ttmb_[pic_.tt_index].decode(br);
        if (ttmb < 0) return false;
      }
    }
  }
  for (int i = 0; i < 6; ++i) {
    const int val = (cbp >> (5 - i)) & 1;
    int plane;
    uint64_t offset;
    int stride;
    block_target(i, &plane, &offset, &stride);
    ByteView pv(cur_->plane[plane].data(), cur_->plane[plane].size());
    if (mb_intra_) {
      a_avail_ = c_avail_ = 0;
      auto flag = [&](uint64_t mb, int bit) { return (intra_mask_[mb] >> bit) & 1; };
      if (i < 4) {
        if (i == 2 || i == 3) a_avail_ = 1;
        else if (!first_slice_line_) a_avail_ = flag(mb_pos - static_cast<uint64_t>(mb_w_), i + 2);
        if (i == 1 || i == 3) c_avail_ = 1;
        else if (mb_x_) c_avail_ = flag(mb_pos - 1, i + 1);
      } else {
        if (!first_slice_line_) a_avail_ = flag(mb_pos - static_cast<uint64_t>(mb_w_), i);
        if (mb_x_) c_avail_ = flag(mb_pos - 1, i);
      }
      for (int k = 0; k < 64; ++k) cur_blk_[i][k] = 0;
      if (!decode_intra_block(br, cur_blk_[i], i, val != 0, mquant, i >= 4 ? codingset2_ : codingset_)) return false;
      inv_trans_8x8(cur_blk_[i]);
      put_block_clamped(pv, offset, stride, cur_blk_[i], true);
    } else if (val) {
      const int pat = decode_p_block(br, cur_blk_[i], i, mquant, ttmb, first_block, pv, offset, stride, nullptr);
      if (pat < 0) return false;
      if (!pic_.ttmbf && ttmb < 8) ttmb = -1;
      first_block = false;
    }
  }
  return !br.error();
}

}  // namespace vc1
}  // namespace hardware
