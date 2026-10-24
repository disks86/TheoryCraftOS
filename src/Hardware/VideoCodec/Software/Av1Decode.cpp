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

#include "Hardware/VideoCodec/Software/Av1Core.hpp"
#include "Hardware/VideoCodec/Software/Av1Warp.hpp"

// Block, partition, tile and superblock syntax of the AV1 decoder: a port of dav1d's src/decode.c (BSD-2-Clause, Copyright (c)
// 2018-2019 VideoLAN and dav1d authors, Two Orioles LLC) and the context helpers of its env.h. Single threaded, single pass.

namespace hardware {
namespace av1 {
namespace {

#include "Hardware/VideoCodec/Software/Av1Tables.inc"

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int iclip(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int apply_sign(int v, int s) { return s < 0 ? -v : v; }
inline unsigned uabs(int v) { return v < 0 ? 0u - static_cast<unsigned>(v) : static_cast<unsigned>(v); }
inline int ulog2(unsigned v) { return 31 ^ __builtin_clz(v); }

// dav1d's set_ctx / case_set: sets 1 << log2n entries of a context array starting at `off`.
template <class T, uint64_t N>
inline void set_ctx(T (&arr)[N], int off, int log2n, int value) {
  ArrayView<T>(arr).subview(static_cast<uint64_t>(off), uint64_t{1} << log2n).fill(static_cast<T>(value));
}

inline ArrayView<const uint8_t> block_dim(int bs) { return ArrayView<const uint8_t>(k_block_dimensions[bs]); }

// ---- contexts (env.h) -------------------------------------------------------------------------------------------------------------

int get_intra_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  if (have_left) {
    if (have_top) {
      const int ctx = l.intra[yb4] + a.intra[xb4];
      return ctx + (ctx == 2);
    }
    return l.intra[yb4] * 2;
  }
  return have_top ? a.intra[xb4] * 2 : 0;
}

int get_tx_ctx(const BlockContext& a, const BlockContext& l, const TxfmInfo& max_tx, int yb4, int xb4) {
  return (l.tx_intra[yb4] >= max_tx.lh) + (a.tx_intra[xb4] >= max_tx.lw);
}

int get_partition_ctx(const BlockContext& a, const BlockContext& l, int bl, int yb8, int xb8) {
  return ((a.partition[xb8] >> (4 - bl)) & 1) + (((l.partition[yb8] >> (4 - bl)) & 1) << 1);
}

unsigned gather_left_partition_prob(ArrayView<const uint16_t> in, int bl) {
  unsigned out = static_cast<unsigned>(in[PARTITION_H - 1] - in[PARTITION_H]);
  out += static_cast<unsigned>(in[PARTITION_SPLIT - 1] - in[PARTITION_T_LEFT_SPLIT]);
  if (bl != BL_128X128) out += static_cast<unsigned>(in[PARTITION_H4 - 1] - in[PARTITION_H4]);
  return out;
}

unsigned gather_top_partition_prob(ArrayView<const uint16_t> in, int bl) {
  unsigned out = static_cast<unsigned>(in[PARTITION_V - 1] - in[PARTITION_T_TOP_SPLIT]);
  out += in[PARTITION_T_LEFT_SPLIT - 1];
  if (bl != BL_128X128) out += static_cast<unsigned>(in[PARTITION_V4 - 1] - in[PARTITION_T_RIGHT_SPLIT]);
  return out;
}

int get_filter_ctx(const BlockContext& a, const BlockContext& l, int comp, int dir, int ref, int yb4, int xb4) {
  const int a_filter = (a.ref[0][xb4] == ref || a.ref[1][xb4] == ref) ? static_cast<int>(a.filter[dir][xb4]) : static_cast<int>(N_SWITCHABLE_FILTERS);
  const int l_filter = (l.ref[0][yb4] == ref || l.ref[1][yb4] == ref) ? static_cast<int>(l.filter[dir][yb4]) : static_cast<int>(N_SWITCHABLE_FILTERS);
  if (a_filter == l_filter) return comp * 4 + a_filter;
  if (a_filter == N_SWITCHABLE_FILTERS) return comp * 4 + l_filter;
  if (l_filter == N_SWITCHABLE_FILTERS) return comp * 4 + a_filter;
  return comp * 4 + N_SWITCHABLE_FILTERS;
}

int get_comp_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  if (have_top) {
    if (have_left) {
      if (a.comp_type[xb4]) {
        if (l.comp_type[yb4]) return 4;
        return 2 + (static_cast<unsigned>(l.ref[0][yb4]) >= 4u);
      } else if (l.comp_type[yb4]) {
        return 2 + (static_cast<unsigned>(a.ref[0][xb4]) >= 4u);
      }
      return (l.ref[0][yb4] >= 4) ^ (a.ref[0][xb4] >= 4);
    }
    return a.comp_type[xb4] ? 3 : a.ref[0][xb4] >= 4;
  } else if (have_left) {
    return l.comp_type[yb4] ? 3 : l.ref[0][yb4] >= 4;
  }
  return 1;
}

inline bool has_uni_comp(const BlockContext& edge, int off) { return (edge.ref[0][off] < 4) == (edge.ref[1][off] < 4); }

int get_comp_dir_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  if (have_top && have_left) {
    const int a_intra = a.intra[xb4], l_intra = l.intra[yb4];
    if (a_intra && l_intra) return 2;
    if (a_intra || l_intra) {
      const BlockContext& edge = a_intra ? l : a;
      const int off = a_intra ? yb4 : xb4;
      if (edge.comp_type[off] == COMP_INTER_NONE) return 2;
      return 1 + 2 * has_uni_comp(edge, off);
    }
    const int a_comp = a.comp_type[xb4] != COMP_INTER_NONE;
    const int l_comp = l.comp_type[yb4] != COMP_INTER_NONE;
    const int a_ref0 = a.ref[0][xb4], l_ref0 = l.ref[0][yb4];
    if (!a_comp && !l_comp) {
      return 1 + 2 * ((a_ref0 >= 4) == (l_ref0 >= 4));
    } else if (!a_comp || !l_comp) {
      const BlockContext& edge = a_comp ? a : l;
      const int off = a_comp ? xb4 : yb4;
      if (!has_uni_comp(edge, off)) return 1;
      return 3 + ((a_ref0 >= 4) == (l_ref0 >= 4));
    }
    const bool a_uni = has_uni_comp(a, xb4), l_uni = has_uni_comp(l, yb4);
    if (!a_uni && !l_uni) return 0;
    if (!a_uni || !l_uni) return 2;
    return 3 + ((a_ref0 == 4) == (l_ref0 == 4));
  } else if (have_top || have_left) {
    const BlockContext& edge = have_left ? l : a;
    const int off = have_left ? yb4 : xb4;
    if (edge.intra[off]) return 2;
    if (edge.comp_type[off] == COMP_INTER_NONE) return 2;
    return 4 * has_uni_comp(edge, off);
  }
  return 2;
}

int get_jnt_comp_ctx(int order_hint_n_bits, int poc, int ref0poc, int ref1poc, const BlockContext& a, const BlockContext& l, int yb4,
                     int xb4) {
  const int d0 = static_cast<int>(uabs(get_poc_diff(order_hint_n_bits, ref0poc, poc)));
  const int d1 = static_cast<int>(uabs(get_poc_diff(order_hint_n_bits, poc, ref1poc)));
  const int offset = d0 == d1;
  const int a_ctx = a.comp_type[xb4] >= COMP_INTER_AVG || a.ref[0][xb4] == 6;
  const int l_ctx = l.comp_type[yb4] >= COMP_INTER_AVG || l.ref[0][yb4] == 6;
  return 3 * offset + a_ctx + l_ctx;
}

int get_mask_comp_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4) {
  const int a_ctx = a.comp_type[xb4] >= COMP_INTER_SEG ? 1 : a.ref[0][xb4] == 6 ? 3 : 0;
  const int l_ctx = l.comp_type[yb4] >= COMP_INTER_SEG ? 1 : l.ref[0][yb4] == 6 ? 3 : 0;
  return imin(a_ctx + l_ctx, 5);
}

int get_ref_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {  // av1_get_ref_ctx
  int cnt[2] = {0, 0};
  if (have_top && !a.intra[xb4]) {
    cnt[a.ref[0][xb4] >= 4]++;
    if (a.comp_type[xb4]) cnt[a.ref[1][xb4] >= 4]++;
  }
  if (have_left && !l.intra[yb4]) {
    cnt[l.ref[0][yb4] >= 4]++;
    if (l.comp_type[yb4]) cnt[l.ref[1][yb4] >= 4]++;
  }
  return cnt[0] == cnt[1] ? 1 : cnt[0] < cnt[1] ? 0 : 2;
}

int get_fwd_ref_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  int cnt[4] = {0, 0, 0, 0};
  if (have_top && !a.intra[xb4]) {
    if (a.ref[0][xb4] < 4) cnt[a.ref[0][xb4]]++;
    if (a.comp_type[xb4] && a.ref[1][xb4] < 4) cnt[a.ref[1][xb4]]++;
  }
  if (have_left && !l.intra[yb4]) {
    if (l.ref[0][yb4] < 4) cnt[l.ref[0][yb4]]++;
    if (l.comp_type[yb4] && l.ref[1][yb4] < 4) cnt[l.ref[1][yb4]]++;
  }
  cnt[0] += cnt[1];
  cnt[2] += cnt[3];
  return cnt[0] == cnt[2] ? 1 : cnt[0] < cnt[2] ? 0 : 2;
}

int get_fwd_ref_1_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  int cnt[2] = {0, 0};
  if (have_top && !a.intra[xb4]) {
    if (a.ref[0][xb4] < 2) cnt[a.ref[0][xb4]]++;
    if (a.comp_type[xb4] && a.ref[1][xb4] < 2) cnt[a.ref[1][xb4]]++;
  }
  if (have_left && !l.intra[yb4]) {
    if (l.ref[0][yb4] < 2) cnt[l.ref[0][yb4]]++;
    if (l.comp_type[yb4] && l.ref[1][yb4] < 2) cnt[l.ref[1][yb4]]++;
  }
  return cnt[0] == cnt[1] ? 1 : cnt[0] < cnt[1] ? 0 : 2;
}

int get_fwd_ref_2_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  int cnt[2] = {0, 0};
  if (have_top && !a.intra[xb4]) {
    if ((static_cast<unsigned>(a.ref[0][xb4]) ^ 2u) < 2) cnt[a.ref[0][xb4] - 2]++;
    if (a.comp_type[xb4] && (static_cast<unsigned>(a.ref[1][xb4]) ^ 2u) < 2) cnt[a.ref[1][xb4] - 2]++;
  }
  if (have_left && !l.intra[yb4]) {
    if ((static_cast<unsigned>(l.ref[0][yb4]) ^ 2u) < 2) cnt[l.ref[0][yb4] - 2]++;
    if (l.comp_type[yb4] && (static_cast<unsigned>(l.ref[1][yb4]) ^ 2u) < 2) cnt[l.ref[1][yb4] - 2]++;
  }
  return cnt[0] == cnt[1] ? 1 : cnt[0] < cnt[1] ? 0 : 2;
}

int get_bwd_ref_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  int cnt[3] = {0, 0, 0};
  if (have_top && !a.intra[xb4]) {
    if (a.ref[0][xb4] >= 4) cnt[a.ref[0][xb4] - 4]++;
    if (a.comp_type[xb4] && a.ref[1][xb4] >= 4) cnt[a.ref[1][xb4] - 4]++;
  }
  if (have_left && !l.intra[yb4]) {
    if (l.ref[0][yb4] >= 4) cnt[l.ref[0][yb4] - 4]++;
    if (l.comp_type[yb4] && l.ref[1][yb4] >= 4) cnt[l.ref[1][yb4] - 4]++;
  }
  cnt[1] += cnt[0];
  return cnt[2] == cnt[1] ? 1 : cnt[1] < cnt[2] ? 0 : 2;
}

int get_bwd_ref_1_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  int cnt[3] = {0, 0, 0};
  if (have_top && !a.intra[xb4]) {
    if (a.ref[0][xb4] >= 4) cnt[a.ref[0][xb4] - 4]++;
    if (a.comp_type[xb4] && a.ref[1][xb4] >= 4) cnt[a.ref[1][xb4] - 4]++;
  }
  if (have_left && !l.intra[yb4]) {
    if (l.ref[0][yb4] >= 4) cnt[l.ref[0][yb4] - 4]++;
    if (l.comp_type[yb4] && l.ref[1][yb4] >= 4) cnt[l.ref[1][yb4] - 4]++;
  }
  return cnt[0] == cnt[1] ? 1 : cnt[0] < cnt[1] ? 0 : 2;
}

int get_uni_p1_ctx(const BlockContext& a, const BlockContext& l, int yb4, int xb4, bool have_top, bool have_left) {
  int cnt[3] = {0, 0, 0};
  if (have_top && !a.intra[xb4]) {
    if (a.ref[0][xb4] - 1u < 3) cnt[a.ref[0][xb4] - 1]++;
    if (a.comp_type[xb4] && a.ref[1][xb4] - 1u < 3) cnt[a.ref[1][xb4] - 1]++;
  }
  if (have_left && !l.intra[yb4]) {
    if (l.ref[0][yb4] - 1u < 3) cnt[l.ref[0][yb4] - 1]++;
    if (l.comp_type[yb4] && l.ref[1][yb4] - 1u < 3) cnt[l.ref[1][yb4] - 1]++;
  }
  cnt[1] += cnt[2];
  return cnt[0] == cnt[1] ? 1 : cnt[0] < cnt[1] ? 0 : 2;
}

int get_drl_context(ArrayView<const RefmvsCandidate> ref_mv_stack, int ref_idx) {
  if (ref_mv_stack[static_cast<uint64_t>(ref_idx)].weight >= 640) return ref_mv_stack[static_cast<uint64_t>(ref_idx) + 1].weight < 640;
  return ref_mv_stack[static_cast<uint64_t>(ref_idx) + 1].weight < 640 ? 2 : 0;
}

unsigned get_cur_frame_segid(int by, int bx, bool have_top, bool have_left, int& seg_ctx, ArrayView<const uint8_t> map, int stride) {
  const int64_t base = static_cast<int64_t>(bx) + static_cast<int64_t>(by) * stride;
  auto at = [&](int64_t delta) { return map[static_cast<uint64_t>(base + delta)]; };
  if (have_left && have_top) {
    const int l = at(-1);
    const int a = at(-stride);
    const int al = at(-(static_cast<int64_t>(stride) + 1));
    if (l == a && al == l)
      seg_ctx = 2;
    else if (l == a || al == l || a == al)
      seg_ctx = 1;
    else
      seg_ctx = 0;
    return a == al ? a : l;
  }
  seg_ctx = 0;
  return have_left ? at(-1) : have_top ? at(-stride) : 0;
}

void fix_int_mv_precision(Mv& mv) {
  mv.x = static_cast<int16_t>(static_cast<unsigned>(mv.x - (mv.x >> 15) + 3) & ~7u);
  mv.y = static_cast<int16_t>(static_cast<unsigned>(mv.y - (mv.y >> 15) + 3) & ~7u);
}

void fix_mv_precision(const FrameHeader& hdr, Mv& mv) {
  if (hdr.force_integer_mv) {
    fix_int_mv_precision(mv);
  } else if (!hdr.hp) {
    mv.x = static_cast<int16_t>(static_cast<unsigned>(mv.x - (mv.x >> 15)) & ~1u);
    mv.y = static_cast<int16_t>(static_cast<unsigned>(mv.y - (mv.y >> 15)) & ~1u);
  }
}

Mv get_gmv_2d(const WarpedMotionParams& gmv, int bx4, int by4, int bw4, int bh4, const FrameHeader& hdr) {
  Mv res;
  res.n = 0;
  switch (gmv.type) {
    case WM_TYPE_ROT_ZOOM:
    case WM_TYPE_AFFINE: {
      const uint32_t x = static_cast<uint32_t>(bx4 * 4 + bw4 * 2 - 1);
      const uint32_t y = static_cast<uint32_t>(by4 * 4 + bh4 * 2 - 1);
      const int xc = static_cast<int>((static_cast<uint32_t>(gmv.matrix[2]) - (1u << 16)) * x + static_cast<uint32_t>(gmv.matrix[3]) * y +
                                      static_cast<uint32_t>(gmv.matrix[0]));
      const int yc = static_cast<int>((static_cast<uint32_t>(gmv.matrix[5]) - (1u << 16)) * y + static_cast<uint32_t>(gmv.matrix[4]) * x +
                                      static_cast<uint32_t>(gmv.matrix[1]));
      const int shift = 16 - (3 - !hdr.hp);
      const unsigned round = (1u << shift) >> 1;
      res.y = static_cast<int16_t>(apply_sign(static_cast<int>(((uabs(yc) + round) >> shift) << !hdr.hp), yc));
      res.x = static_cast<int16_t>(apply_sign(static_cast<int>(((uabs(xc) + round) >> shift) << !hdr.hp), xc));
      if (hdr.force_integer_mv) fix_int_mv_precision(res);
      return res;
    }
    case WM_TYPE_TRANSLATION:
      res.y = static_cast<int16_t>(gmv.matrix[0] >> 13);
      res.x = static_cast<int16_t>(gmv.matrix[1] >> 13);
      if (hdr.force_integer_mv) fix_int_mv_precision(res);
      return res;
    case WM_TYPE_IDENTITY:
    default:
      return res;
  }
}

// ---- motion vector residuals ------------------------------------------------------------------------------------------------------

int read_mv_component_diff(Msac& msac, CdfMvComponent& mv_comp, int mv_prec) {
  const int sign = msac.bool_adapt(ArrayView<uint16_t>(mv_comp.sign));
  const int cl = msac.symbol_adapt16(ArrayView<uint16_t>(mv_comp.classes), 10);
  int up, fp = 3, hp = 1;
  if (!cl) {
    up = msac.bool_adapt(ArrayView<uint16_t>(mv_comp.class0));
    if (mv_prec >= 0) {  // !force_integer_mv
      fp = msac.symbol_adapt4(ArrayView<uint16_t>(mv_comp.class0_fp[up]), 3);
      if (mv_prec > 0) hp = msac.bool_adapt(ArrayView<uint16_t>(mv_comp.class0_hp));  // allow_high_precision_mv
    }
  } else {
    up = 1 << cl;
    for (int n = 0; n < cl; n++) up |= msac.bool_adapt(ArrayView<uint16_t>(mv_comp.classN[n])) << n;
    if (mv_prec >= 0) {
      fp = msac.symbol_adapt4(ArrayView<uint16_t>(mv_comp.classN_fp), 3);
      if (mv_prec > 0) hp = msac.bool_adapt(ArrayView<uint16_t>(mv_comp.classN_hp));
    }
  }
  const int diff = ((up << 3) | (fp << 1) | hp) + 1;
  return sign ? -diff : diff;
}

void read_mv_residual(TileState& ts, Mv& ref_mv, int mv_prec) {
  Msac& msac = ts.msac;
  const unsigned mv_joint = msac.symbol_adapt4(ArrayView<uint16_t>(ts.cdf.mv.joint), N_MV_JOINTS - 1);
  if (mv_joint & MV_JOINT_V) ref_mv.y = static_cast<int16_t>(ref_mv.y + read_mv_component_diff(msac, ts.cdf.mv.comp[0], mv_prec));
  if (mv_joint & MV_JOINT_H) ref_mv.x = static_cast<int16_t>(ref_mv.x + read_mv_component_diff(msac, ts.cdf.mv.comp[1], mv_prec));
}

int neg_deinterleave(int diff, int ref, int max) {
  if (!ref) return diff;
  if (ref >= (max - 1)) return max - diff - 1;
  if (2 * ref < max) {
    if (diff <= 2 * ref) {
      if (diff & 1) return ref + ((diff + 1) >> 1);
      return ref - (diff >> 1);
    }
    return diff;
  }
  if (diff <= 2 * (max - ref - 1)) {
    if (diff & 1) return ref + ((diff + 1) >> 1);
    return ref - (diff >> 1);
  }
  return max - (diff + 1);
}

// ---- warp: matching reference blocks and the local warp model ------------------------------------------------------------------------

void find_matching_ref(const FrameContext& f, const TaskContext& t, int intra_edge_flags, int bw4, int bh4, int w4, int h4, bool have_left,
                       bool have_top, int ref, uint64_t masks[2]) {
  const int base = (t.by & 31) + 5;
  auto row = [&](int dy) { return f.rf.row(t.rt, base + dy); };
  int count = 0;
  bool have_topleft = have_top && have_left;
  bool have_topright = imax(bw4, bh4) < 32 && have_top && t.bx + bw4 < t.ts->tiling.col_end && (intra_edge_flags & EDGE_I444_TOP_HAS_RIGHT);
  auto matches = [&](const RefmvsBlock& rp) { return rp.ref.ref[0] == ref + 1 && rp.ref.ref[1] == -1; };

  if (have_top) {
    const ArrayView<RefmvsBlock> above = row(-1);
    int x2 = t.bx;
    if (matches(above[static_cast<uint64_t>(x2)])) {
      masks[0] |= 1;
      count = 1;
    }
    int aw4 = k_block_dimensions[above[static_cast<uint64_t>(x2)].bs][0];
    if (aw4 >= bw4) {
      const int off = t.bx & (aw4 - 1);
      if (off) have_topleft = false;
      if (aw4 - off > bw4) have_topright = false;
    } else {
      unsigned mask = 1u << aw4;
      for (int x = aw4; x < w4; x += aw4) {
        x2 += aw4;
        if (matches(above[static_cast<uint64_t>(x2)])) {
          masks[0] |= mask;
          if (++count >= 8) return;
        }
        aw4 = k_block_dimensions[above[static_cast<uint64_t>(x2)].bs][0];
        mask <<= aw4;
      }
    }
  }
  if (have_left) {
    int dy = 0;
    if (matches(row(dy)[static_cast<uint64_t>(t.bx - 1)])) {
      masks[1] |= 1;
      if (++count >= 8) return;
    }
    int lh4 = k_block_dimensions[row(dy)[static_cast<uint64_t>(t.bx - 1)].bs][1];
    if (lh4 >= bh4) {
      if (t.by & (lh4 - 1)) have_topleft = false;
    } else {
      unsigned mask = 1u << lh4;
      for (int y = lh4; y < h4; y += lh4) {
        dy += lh4;
        if (matches(row(dy)[static_cast<uint64_t>(t.bx - 1)])) {
          masks[1] |= mask;
          if (++count >= 8) return;
        }
        lh4 = k_block_dimensions[row(dy)[static_cast<uint64_t>(t.bx - 1)].bs][1];
        mask <<= lh4;
      }
    }
  }
  if (have_topleft && matches(row(-1)[static_cast<uint64_t>(t.bx - 1)])) {
    masks[1] |= 1ULL << 32;
    if (++count >= 8) return;
  }
  if (have_topright && matches(row(-1)[static_cast<uint64_t>(t.bx + bw4)])) masks[0] |= 1ULL << 32;
}

void derive_warpmv(const FrameContext& f, const TaskContext& t, int bw4, int bh4, const uint64_t masks[2], Mv mv, WarpedMotionParams& wmp) {
  WarpSample pts[8];
  int np = 0;
  const int base = (t.by & 31) + 5;
  auto row = [&](int dy) { return f.rf.row(t.rt, base + dy); };
  auto add_sample = [&](int dx, int dy, int sx, int sy, const RefmvsBlock& rp) {
    const int rw = k_block_dimensions[rp.bs][0], rh = k_block_dimensions[rp.bs][1];
    WarpSample& s = pts[np];
    s.src[0] = 16 * (2 * dx + sx * rw) - 8;
    s.src[1] = 16 * (2 * dy + sy * rh) - 8;
    s.dst[0] = s.src[0] + rp.mv.mv[0].x;
    s.dst[1] = s.src[1] + rp.mv.mv[0].y;
    np++;
  };

  // use masks[] to find the projectable motion vectors in the edges
  if (static_cast<unsigned>(masks[0]) == 1 && !(masks[1] >> 32)) {
    const RefmvsBlock& rp = row(-1)[static_cast<uint64_t>(t.bx)];
    const int off = t.bx & (k_block_dimensions[rp.bs][0] - 1);
    add_sample(-off, 0, 1, -1, rp);
  } else {
    unsigned off = 0, xmask = static_cast<uint32_t>(masks[0]);
    while (np < 8 && xmask) {  // top
      const int tz = __builtin_ctz(xmask);
      off += static_cast<unsigned>(tz);
      xmask >>= tz;
      add_sample(static_cast<int>(off), 0, 1, -1, row(-1)[t.bx + off]);
      xmask &= ~1u;
    }
  }
  if (np < 8 && masks[1] == 1) {
    const RefmvsBlock& rl = row(0)[static_cast<uint64_t>(t.bx - 1)];
    const int off = t.by & (k_block_dimensions[rl.bs][1] - 1);
    add_sample(0, -off, -1, 1, row(-off)[static_cast<uint64_t>(t.bx - 1)]);
  } else {
    unsigned off = 0, ymask = static_cast<uint32_t>(masks[1]);
    while (np < 8 && ymask) {  // left
      const int tz = __builtin_ctz(ymask);
      off += static_cast<unsigned>(tz);
      ymask >>= tz;
      add_sample(0, static_cast<int>(off), -1, 1, row(static_cast<int>(off))[static_cast<uint64_t>(t.bx - 1)]);
      ymask &= ~1u;
    }
  }
  if (np < 8 && (masks[1] >> 32)) add_sample(0, 0, -1, -1, row(-1)[static_cast<uint64_t>(t.bx - 1)]);  // top/left
  if (np < 8 && (masks[0] >> 32)) add_sample(bw4, 0, 1, -1, row(-1)[static_cast<uint64_t>(t.bx + bw4)]);  // top/right
  if (np <= 0) {
    wmp.type = WM_TYPE_IDENTITY;
    return;
  }

  // select according to motion vector difference against a threshold
  int mvd[8], ret = 0;
  const int thresh = 4 * iclip(imax(bw4, bh4), 4, 28);
  for (int i = 0; i < np; i++) {
    mvd[i] = static_cast<int>(uabs(pts[i].dst[0] - pts[i].src[0] - mv.x) + uabs(pts[i].dst[1] - pts[i].src[1] - mv.y));
    if (mvd[i] > thresh)
      mvd[i] = -1;
    else
      ret++;
  }
  if (!ret) {
    ret = 1;
  } else {
    for (int i = 0, j = np - 1, k = 0; k < np - ret; k++, i++, j--) {
      while (mvd[i] != -1) i++;
      while (mvd[j] == -1) j--;
      if (i > j) break;
      mvd[i] = mvd[j];  // replace the discarded samples
      pts[i] = pts[j];
    }
  }

  if (!find_affine_int(ArrayView<const WarpSample>(pts, static_cast<uint64_t>(ret)), bw4, bh4, mv, &wmp, t.bx, t.by) && !get_shear_params(&wmp))
    wmp.type = WM_TYPE_AFFINE;
  else
    wmp.type = WM_TYPE_IDENTITY;
}

bool findoddzero(ArrayView<const uint8_t> buf, int len) {
  for (int n = 0; n < len; n++)
    if (!buf[static_cast<uint64_t>(n) * 2]) return true;
  return false;
}

// ---- palette indices -------------------------------------------------------------------------------------------------------------

// Resolve a decoded rank (color_idx) to a palette entry, given the ordered priority candidates p[0..prio_count-1] and the bitmask of values
// already used by those candidates. Ranks below prio_count select a priority candidate directly; higher ranks select the
// (rank - prio_count)th value (counting from 0) that is not present in mask, in ascending order.
int resolve_pal_color(int color_idx, const uint8_t* p, int prio_count, unsigned mask) {
  if (color_idx < prio_count) return p[color_idx];
  unsigned rank = static_cast<unsigned>(color_idx - prio_count) + (mask & 1);
  rank += ((mask >> 1) & 1) & (rank > 0);
  // Unset-bit positions minus their ranks, packed in 2-bit fields. Mask bits 0 and 1 are cleared and compensated in rank. At most three
  // priority colors keep each offset within 0..3.
  static const uint8_t free_offsets2[] = {
      0x00, 0x00, 0x50, 0x15, 0x40, 0x15, 0xa0, 0x0a, 0x00, 0x15, 0x90, 0x0a, 0x80, 0x0a, 0xf0, 0x03, 0x00, 0x14, 0x50, 0x0a,
      0x40, 0x0a, 0xe0, 0x03, 0x00, 0x0a, 0xd0, 0x03, 0xc0, 0x03, 0x00, 0x00, 0x00, 0x10, 0x50, 0x09, 0x40, 0x09, 0xa0, 0x03,
      0x00, 0x09, 0x90, 0x03, 0x80, 0x03, 0x00, 0x00, 0x00, 0x08, 0x50, 0x03, 0x40, 0x03, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x05, 0x40, 0x05, 0xa0, 0x02, 0x00, 0x05, 0x90, 0x02, 0x80, 0x02, 0x00, 0x00,
      0x00, 0x04, 0x50, 0x02, 0x40, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x01,
      0x40, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  const unsigned byte_idx = 2 * (mask >> 2) + (rank >> 2);
  return static_cast<int>(rank + ((ArrayView<const uint8_t>(free_offsets2)[byte_idx] >> (2 * (rank & 3))) & 3));
}

bool read_pal_indices(TaskContext& t, ArrayView<uint8_t> pal_idx, int pal_sz, int pl, int w4, int h4, int bw4, int bh4) {
  TileState& ts = *t.ts;
  const int stride = bw4 * 4;
  ArrayView<uint8_t> pal_tmp(t.scratch.pal_tmp);
  pal_tmp[0] = static_cast<uint8_t>(ts.msac.uniform(static_cast<unsigned>(pal_sz)));
  auto color_map_cdf = [&](int ctx) { return ArrayView<uint16_t>(ts.cdf.m.color_map[pl][pal_sz - 2][ctx]); };

  if (pal_sz == 2) {
    // Binary palette: ctx 1 is unreachable (needs 3 distinct neighbours), and resolve always toggles the single priority value.
    for (int i = 1; i < 4 * (w4 + h4) - 1; i++) {
      const int first = imin(i, w4 * 4 - 1);
      const int last = imax(0, i - h4 * 4 + 1);
      bool have_top = i > first;
      int64_t pos = first + static_cast<int64_t>(i - first) * stride;
      for (int j = first; j >= last; have_top = true, j--, pos += stride - 1) {
        const bool have_left = j > 0;
        int ctx, p0;
        if (!have_left) {
          ctx = 0;
          p0 = pal_tmp[static_cast<uint64_t>(pos - stride)];
        } else if (!have_top) {
          ctx = 0;
          p0 = pal_tmp[static_cast<uint64_t>(pos - 1)];
        } else {
          const int l = pal_tmp[static_cast<uint64_t>(pos - 1)], tp = pal_tmp[static_cast<uint64_t>(pos - stride)];
          const int tl = pal_tmp[static_cast<uint64_t>(pos - (stride + 1))];
          const int same_t_l = tp == l;
          const int same_t_tl = tp == tl;
          ctx = 2 + same_t_l + (same_t_l & same_t_tl);
          p0 = same_t_l ? tp : tl;
        }
        const int bit = static_cast<int>(ts.msac.bool_adapt(color_map_cdf(ctx)));
        pal_tmp[static_cast<uint64_t>((i - j) * static_cast<int64_t>(stride) + j)] = static_cast<uint8_t>(bit ^ p0);
      }
    }
    return pal_idx_finish(pal_idx, ConstByteView(pal_tmp), bw4 * 4, bh4 * 4, w4 * 4, h4 * 4);
  }

  for (int i = 1; i < 4 * (w4 + h4) - 1; i++) {
    // top/left-to-bottom/right diagonals ("wave-front")
    const int first = imin(i, w4 * 4 - 1);
    const int last = imax(0, i - h4 * 4 + 1);
    bool have_top = i > first;
    int64_t pos = first + static_cast<int64_t>(i - first) * stride;
    for (int j = first; j >= last; have_top = true, j--, pos += stride - 1) {
      const bool have_left = j > 0;
      uint8_t p[3] = {0, 0, 0};
      unsigned mask;
      int ctx, prio_count;
      if (!have_left) {
        ctx = 0;
        prio_count = 1;
        p[0] = pal_tmp[static_cast<uint64_t>(pos - stride)];
        mask = 1u << p[0];
      } else if (!have_top) {
        ctx = 0;
        prio_count = 1;
        p[0] = pal_tmp[static_cast<uint64_t>(pos - 1)];
        mask = 1u << p[0];
      } else {
        const int l = pal_tmp[static_cast<uint64_t>(pos - 1)], tp = pal_tmp[static_cast<uint64_t>(pos - stride)];
        const int tl = pal_tmp[static_cast<uint64_t>(pos - (stride + 1))];
        const bool same_t_l = tp == l;
        const bool same_t_tl = tp == tl;
        const bool same_l_tl = l == tl;
        if (same_t_l & same_t_tl) {
          ctx = 4;
          prio_count = 1;
          p[0] = static_cast<uint8_t>(tp);
          mask = 1u << tp;
        } else if (same_t_l) {
          ctx = 3;
          prio_count = 2;
          p[0] = static_cast<uint8_t>(tp);
          p[1] = static_cast<uint8_t>(tl);
          mask = (1u << tp) | (1u << tl);
        } else if (same_t_tl | same_l_tl) {
          ctx = 2;
          prio_count = 2;
          p[0] = static_cast<uint8_t>(tl);
          p[1] = static_cast<uint8_t>(same_t_tl ? l : tp);
          mask = (1u << tl) | (1u << p[1]);
        } else {
          ctx = 1;
          prio_count = 3;
          p[0] = static_cast<uint8_t>(imin(tp, l));
          p[1] = static_cast<uint8_t>(imax(tp, l));
          p[2] = static_cast<uint8_t>(tl);
          mask = (1u << tp) | (1u << l) | (1u << tl);
        }
      }
      const int color_idx = static_cast<int>(ts.msac.symbol_adapt8(color_map_cdf(ctx), static_cast<unsigned>(pal_sz - 1)));
      pal_tmp[static_cast<uint64_t>((i - j) * static_cast<int64_t>(stride) + j)] =
          static_cast<uint8_t>(resolve_pal_color(color_idx, p, prio_count, mask));
    }
  }
  return pal_idx_finish(pal_idx, ConstByteView(pal_tmp), bw4 * 4, bh4 * 4, w4 * 4, h4 * 4);
}

// ---- transform size syntax ---------------------------------------------------------------------------------------------------------

void read_tx_tree(TaskContext& t, int from, int depth, uint16_t* masks, int x_off, int y_off) {
  const FrameContext& f = *t.f;
  const int bx4 = t.bx & 31, by4 = t.by & 31;
  const TxfmInfo& t_dim = k_txfm_dimensions[from];
  const int txw = t_dim.lw, txh = t_dim.lh;
  bool is_split;

  if (depth < 2 && from > static_cast<int>(TX_4X4)) {
    const int cat = 2 * (TX_64X64 - t_dim.max) - depth;
    const int a = t.a->tx[bx4] < txw;
    const int l = t.l.tx[by4] < txh;
    is_split = t.ts->msac.bool_adapt(ArrayView<uint16_t>(t.ts->cdf.m.txpart[cat][a + l]));
    if (is_split) masks[depth] = static_cast<uint16_t>(masks[depth] | (1 << (y_off * 4 + x_off)));
  } else {
    is_split = false;
  }

  if (is_split && t_dim.max > TX_8X8) {
    const int sub = t_dim.sub;
    const TxfmInfo& sub_t_dim = k_txfm_dimensions[sub];
    const int txsw = sub_t_dim.w, txsh = sub_t_dim.h;

    read_tx_tree(t, sub, depth + 1, masks, x_off * 2 + 0, y_off * 2 + 0);
    t.bx += txsw;
    if (txw >= txh && t.bx < f.bw) read_tx_tree(t, sub, depth + 1, masks, x_off * 2 + 1, y_off * 2 + 0);
    t.bx -= txsw;
    t.by += txsh;
    if (txh >= txw && t.by < f.bh) {
      read_tx_tree(t, sub, depth + 1, masks, x_off * 2 + 0, y_off * 2 + 1);
      t.bx += txsw;
      if (txw >= txh && t.bx < f.bw) read_tx_tree(t, sub, depth + 1, masks, x_off * 2 + 1, y_off * 2 + 1);
      t.bx -= txsw;
    }
    t.by -= txsh;
  } else {
    set_ctx(t.a->tx, bx4, t_dim.lw, is_split ? TX_4X4 : txw);
    set_ctx(t.l.tx, by4, t_dim.lh, is_split ? TX_4X4 : txh);
  }
}

void read_vartx_tree(TaskContext& t, Av1Block& b, BlockSize bs, int bx4, int by4) {
  const FrameContext& f = *t.f;
  const FrameHeader& hdr = *f.frame_hdr;
  const ArrayView<const uint8_t> b_dim = block_dim(bs);
  const int bw4 = b_dim[0], bh4 = b_dim[1];

  // var-tx tree coding
  uint16_t tx_split[2] = {0, 0};
  b.max_ytx = k_max_txfm_size_for_bs[bs][0];
  if (!b.skip && (hdr.segmentation.lossless[b.seg_id] || b.max_ytx == TX_4X4)) {
    b.max_ytx = b.uvtx = TX_4X4;
    if (hdr.txfm_mode == TX_SWITCHABLE) {
      set_ctx(t.a->tx, bx4, b_dim[2], TX_4X4);
      set_ctx(t.l.tx, by4, b_dim[3], TX_4X4);
    }
  } else if (hdr.txfm_mode != TX_SWITCHABLE || b.skip) {
    if (hdr.txfm_mode == TX_SWITCHABLE) {
      set_ctx(t.a->tx, bx4, b_dim[2], b_dim[2 + 0]);
      set_ctx(t.l.tx, by4, b_dim[3], b_dim[2 + 1]);
    }
    b.uvtx = k_max_txfm_size_for_bs[bs][f.layout];
  } else {
    const TxfmInfo& ytx = k_txfm_dimensions[b.max_ytx];
    int y, x, y_off, x_off;
    for (y = 0, y_off = 0; y < bh4; y += ytx.h, y_off++) {
      for (x = 0, x_off = 0; x < bw4; x += ytx.w, x_off++) {
        read_tx_tree(t, b.max_ytx, 0, tx_split, x_off, y_off);
        // contexts are updated inside read_tx_tree()
        t.bx += ytx.w;
      }
      t.bx -= x;
      t.by += ytx.h;
    }
    t.by -= y;
    b.uvtx = k_max_txfm_size_for_bs[bs][f.layout];
  }
  b.tx_split0 = static_cast<uint8_t>(tx_split[0]);
  b.tx_split1 = tx_split[1];
}

unsigned get_prev_frame_segid(const FrameContext& f, int by, int bx, int w4, int h4, ArrayView<const uint8_t> ref_seg_map, int stride) {
  unsigned seg_id = 8;
  int64_t pos = static_cast<int64_t>(by) * stride + bx;
  do {
    for (int x = 0; x < w4; x++) seg_id = seg_id < ref_seg_map[static_cast<uint64_t>(pos + x)] ? seg_id : ref_seg_map[static_cast<uint64_t>(pos + x)];
    pos += stride;
  } while (--h4 > 0 && seg_id);
  return seg_id;
}

// ---- refmvs templates (splat_*_mv) --------------------------------------------------------------------------------------------------

void splat_oneref_mv(FrameContext& f, TaskContext& t, BlockSize bs, const Av1Block& b, int bw4, int bh4) {
  const int mode = b.inter_mode;
  RefmvsBlock tmpl{};
  tmpl.ref.ref[0] = static_cast<int8_t>(b.ref[0] + 1);
  tmpl.ref.ref[1] = b.interintra_type ? 0 : -1;
  tmpl.mv.mv[0] = b.mv[0];
  tmpl.bs = bs;
  tmpl.mf = static_cast<uint8_t>((mode == GLOBALMV && imin(bw4, bh4) >= 2) | ((mode == NEWMV) * 2));
  f.rf.splat_mv(t.rt, tmpl, t.by, t.bx, bw4, bh4);
}

void splat_intrabc_mv(FrameContext& f, TaskContext& t, BlockSize bs, const Av1Block& b, int bw4, int bh4) {
  RefmvsBlock tmpl{};
  tmpl.ref.ref[0] = 0;
  tmpl.ref.ref[1] = -1;
  tmpl.mv.mv[0] = b.mv[0];
  tmpl.bs = bs;
  tmpl.mf = 0;
  f.rf.splat_mv(t.rt, tmpl, t.by, t.bx, bw4, bh4);
}

void splat_tworef_mv(FrameContext& f, TaskContext& t, BlockSize bs, const Av1Block& b, int bw4, int bh4) {
  const int mode = b.inter_mode;
  RefmvsBlock tmpl{};
  tmpl.ref.ref[0] = static_cast<int8_t>(b.ref[0] + 1);
  tmpl.ref.ref[1] = static_cast<int8_t>(b.ref[1] + 1);
  tmpl.mv.mv[0] = b.mv[0];
  tmpl.mv.mv[1] = b.mv[1];
  tmpl.bs = bs;
  tmpl.mf = static_cast<uint8_t>((mode == GLOBALMV_GLOBALMV) | (!!((1 << mode) & 0xbc) * 2));
  f.rf.splat_mv(t.rt, tmpl, t.by, t.bx, bw4, bh4);
}

void splat_intraref(FrameContext& f, TaskContext& t, BlockSize bs, int bw4, int bh4) {
  RefmvsBlock tmpl{};
  tmpl.ref.ref[0] = 0;
  tmpl.ref.ref[1] = -1;
  tmpl.mv.mv[0].n = INVALID_MV;
  tmpl.bs = bs;
  tmpl.mf = 0;
  f.rf.splat_mv(t.rt, tmpl, t.by, t.bx, bw4, bh4);
}

void reset_context(BlockContext& ctx, bool keyframe) {
  ArrayView<uint8_t>(ctx.intra).fill(keyframe);
  ArrayView<uint8_t>(ctx.uvmode).fill(DC_PRED);
  if (keyframe) ArrayView<uint8_t>(ctx.mode).fill(DC_PRED);
  ArrayView<uint8_t>(ctx.partition).fill(0);
  ArrayView<uint8_t>(ctx.skip).fill(0);
  ArrayView<uint8_t>(ctx.skip_mode).fill(0);
  ArrayView<int8_t>(ctx.tx_intra).fill(-1);
  ArrayView<int8_t>(ctx.tx).fill(TX_64X64);
  if (!keyframe) {
    ArrayView<int8_t>(ctx.ref[0]).fill(-1);
    ArrayView<int8_t>(ctx.ref[1]).fill(-1);
    ArrayView<uint8_t>(ctx.comp_type).fill(0);
    ArrayView<uint8_t>(ctx.mode).fill(NEARESTMV);
  }
  ArrayView<uint8_t>(ctx.lcoef).fill(0x40);
  ArrayView<uint8_t>(ctx.ccoef[0]).fill(0x40);
  ArrayView<uint8_t>(ctx.ccoef[1]).fill(0x40);
  ArrayView<uint8_t>(ctx.filter[0]).fill(N_SWITCHABLE_FILTERS);
  ArrayView<uint8_t>(ctx.filter[1]).fill(N_SWITCHABLE_FILTERS);
  ArrayView<uint8_t>(ctx.seg_pred).fill(0);
  ArrayView<uint8_t>(ctx.pal_sz).fill(0);
}

}  // namespace

void reset_block_context(BlockContext& ctx, bool keyframe) { reset_context(ctx, keyframe); }

void init_quant_tables(const SequenceHeader& seq_hdr, const FrameHeader& frame_hdr, int qidx, DequantTables& dq) {
  for (int i = 0; i < (frame_hdr.segmentation.enabled ? 8 : 1); i++) {
    const int yac = frame_hdr.segmentation.enabled ? iclip(qidx + frame_hdr.segmentation.seg_data.d[i].delta_q, 0, 255) : qidx;
    const int ydc = iclip(yac + frame_hdr.quant.ydc_delta, 0, 255);
    const int uac = iclip(yac + frame_hdr.quant.uac_delta, 0, 255);
    const int udc = iclip(yac + frame_hdr.quant.udc_delta, 0, 255);
    const int vac = iclip(yac + frame_hdr.quant.vac_delta, 0, 255);
    const int vdc = iclip(yac + frame_hdr.quant.vdc_delta, 0, 255);
    const int hbd = seq_hdr.hbd;
    dq.q[i][0][0] = k_dq_tbl[hbd][ydc][0];
    dq.q[i][0][1] = k_dq_tbl[hbd][yac][1];
    dq.q[i][1][0] = k_dq_tbl[hbd][udc][0];
    dq.q[i][1][1] = k_dq_tbl[hbd][uac][1];
    dq.q[i][2][0] = k_dq_tbl[hbd][vdc][0];
    dq.q[i][2][1] = k_dq_tbl[hbd][vac][1];
  }
}

bool Core::decode_b(TaskContext& t, BlockLevel bl, BlockSize bs, BlockPartition bp, int intra_edge_flags) {
  TileState& ts = *t.ts;
  FrameContext& f = *t.f;
  const FrameHeader& hdr = *f.frame_hdr;
  const SequenceHeader& seq = *f.seq_hdr;
  Av1Block b{};
  const ArrayView<const uint8_t> b_dim = block_dim(bs);
  const int bx4 = t.bx & 31, by4 = t.by & 31;
  const int ss_ver = f.ss_ver;
  const int ss_hor = f.ss_hor;
  const int cbx4 = bx4 >> ss_hor, cby4 = by4 >> ss_ver;
  const int bw4 = b_dim[0], bh4 = b_dim[1];
  const int w4 = imin(bw4, f.bw - t.bx), h4 = imin(bh4, f.bh - t.by);
  const int cbw4 = (bw4 + ss_hor) >> ss_hor, cbh4 = (bh4 + ss_ver) >> ss_ver;
  const bool have_left = t.bx > ts.tiling.col_start;
  const bool have_top = t.by > ts.tiling.row_start;
  const bool has_chroma = f.layout != PIXEL_LAYOUT_I400 && (bw4 > ss_hor || (t.bx & 1)) && (bh4 > ss_ver || (t.by & 1));
  const bool inter_frame = is_inter_or_switch(hdr);
  Msac& msac = ts.msac;
  CdfModeContext& cdf = ts.cdf.m;
  auto bool_adapt = [&](uint16_t(&c)[2]) { return static_cast<int>(msac.bool_adapt(ArrayView<uint16_t>(c))); };

  const int cw4 = (w4 + ss_hor) >> ss_hor, ch4 = (h4 + ss_ver) >> ss_ver;

  b.bl = static_cast<uint8_t>(bl);
  b.bp = static_cast<uint8_t>(bp);
  b.bs = static_cast<uint8_t>(bs);

  const SegmentationData* seg = nullptr;

  // segment_id (if seg_feature for skip/ref/gmv is enabled)
  int seg_pred = 0;
  const ArrayView<const uint8_t> prev_segmap = f.prev_segmap ? ArrayView<const uint8_t>(f.prev_segmap->data.view()) : ArrayView<const uint8_t>();
  const ArrayView<const uint8_t> cur_segmap = f.cur_segmap ? ArrayView<const uint8_t>(f.cur_segmap->data.view()) : ArrayView<const uint8_t>();
  if (hdr.segmentation.enabled) {
    if (!hdr.segmentation.update_map) {
      if (f.prev_segmap) {
        const unsigned seg_id = get_prev_frame_segid(f, t.by, t.bx, w4, h4, prev_segmap, f.b4_stride);
        if (seg_id >= 8) return false;
        b.seg_id = static_cast<uint8_t>(seg_id);
      } else {
        b.seg_id = 0;
      }
      seg = &hdr.segmentation.seg_data.d[b.seg_id];
    } else if (hdr.segmentation.seg_data.preskip) {
      if (hdr.segmentation.temporal && (seg_pred = bool_adapt(cdf.seg_pred[t.a->seg_pred[bx4] + t.l.seg_pred[by4]]))) {
        // temporal predicted seg_id
        if (f.prev_segmap) {
          const unsigned seg_id = get_prev_frame_segid(f, t.by, t.bx, w4, h4, prev_segmap, f.b4_stride);
          if (seg_id >= 8) return false;
          b.seg_id = static_cast<uint8_t>(seg_id);
        } else {
          b.seg_id = 0;
        }
      } else {
        int seg_ctx;
        const unsigned pred_seg_id = get_cur_frame_segid(t.by, t.bx, have_top, have_left, seg_ctx, cur_segmap, f.b4_stride);
        const unsigned diff = msac.symbol_adapt8(ArrayView<uint16_t>(cdf.seg_id[seg_ctx]), MAX_SEGMENTS - 1);
        const unsigned last_active_seg_id = static_cast<unsigned>(hdr.segmentation.seg_data.last_active_segid);
        b.seg_id = static_cast<uint8_t>(neg_deinterleave(static_cast<int>(diff), static_cast<int>(pred_seg_id), static_cast<int>(last_active_seg_id) + 1));
        if (b.seg_id > last_active_seg_id) b.seg_id = 0;  // error?
        if (b.seg_id >= MAX_SEGMENTS) b.seg_id = 0;       // error?
      }
      seg = &hdr.segmentation.seg_data.d[b.seg_id];
    }
  } else {
    b.seg_id = 0;
  }

  // skip_mode
  if ((!seg || (!seg->globalmv && seg->ref == -1 && !seg->skip)) && hdr.skip_mode_enabled && imin(bw4, bh4) > 1) {
    const int smctx = t.a->skip_mode[bx4] + t.l.skip_mode[by4];
    b.skip_mode = static_cast<uint8_t>(bool_adapt(cdf.skip_mode[smctx]));
  } else {
    b.skip_mode = 0;
  }

  // skip
  if (b.skip_mode || (seg && seg->skip)) {
    b.skip = 1;
  } else {
    const int sctx = t.a->skip[bx4] + t.l.skip[by4];
    b.skip = static_cast<uint8_t>(bool_adapt(cdf.skip[sctx]));
  }

  // segment_id
  if (hdr.segmentation.enabled && hdr.segmentation.update_map && !hdr.segmentation.seg_data.preskip) {
    if (!b.skip && hdr.segmentation.temporal && (seg_pred = bool_adapt(cdf.seg_pred[t.a->seg_pred[bx4] + t.l.seg_pred[by4]]))) {
      // temporal predicted seg_id
      if (f.prev_segmap) {
        const unsigned seg_id = get_prev_frame_segid(f, t.by, t.bx, w4, h4, prev_segmap, f.b4_stride);
        if (seg_id >= 8) return false;
        b.seg_id = static_cast<uint8_t>(seg_id);
      } else {
        b.seg_id = 0;
      }
    } else {
      int seg_ctx;
      const unsigned pred_seg_id = get_cur_frame_segid(t.by, t.bx, have_top, have_left, seg_ctx, cur_segmap, f.b4_stride);
      if (b.skip) {
        b.seg_id = static_cast<uint8_t>(pred_seg_id);
      } else {
        const unsigned diff = msac.symbol_adapt8(ArrayView<uint16_t>(cdf.seg_id[seg_ctx]), MAX_SEGMENTS - 1);
        const unsigned last_active_seg_id = static_cast<unsigned>(hdr.segmentation.seg_data.last_active_segid);
        b.seg_id = static_cast<uint8_t>(neg_deinterleave(static_cast<int>(diff), static_cast<int>(pred_seg_id), static_cast<int>(last_active_seg_id) + 1));
        if (b.seg_id > last_active_seg_id) b.seg_id = 0;  // error?
      }
      if (b.seg_id >= MAX_SEGMENTS) b.seg_id = 0;  // error?
    }
    seg = &hdr.segmentation.seg_data.d[b.seg_id];
  }

  // cdef index
  if (!b.skip) {
    if (f.lfm.cdef_idx_unset(t.bx, t.by)) {
      const int v = static_cast<int>(msac.bools(hdr.cdef.n_bits));
      f.lfm.set_cdef_idx(t.bx, t.by, bs, v);
    }
  }

  // delta-q/lf
  if (!((t.bx | t.by) & (31 >> !seq.sb128))) {
    const int prev_qidx = ts.last_qidx;
    const bool have_delta_q = hdr.delta.q.present && (bs != (seq.sb128 ? BS_128x128 : BS_64x64) || !b.skip);

    if (have_delta_q) {
      int delta_q = static_cast<int>(msac.symbol_adapt4(ArrayView<uint16_t>(cdf.delta_q), 3));
      if (delta_q == 3) {
        const int n_bits = 1 + static_cast<int>(msac.bools(3));
        delta_q = static_cast<int>(msac.bools(static_cast<unsigned>(n_bits))) + 1 + (1 << n_bits);
      }
      if (delta_q) {
        if (msac.bool_equi()) delta_q = -delta_q;
        delta_q *= 1 << hdr.delta.q.res_log2;
      }
      ts.last_qidx = iclip(ts.last_qidx + delta_q, 1, 255);

      if (hdr.delta.lf.present) {
        const int n_lfs = hdr.delta.lf.multi ? f.layout != PIXEL_LAYOUT_I400 ? 4 : 2 : 1;
        for (int i = 0; i < n_lfs; i++) {
          int delta_lf = static_cast<int>(msac.symbol_adapt4(ArrayView<uint16_t>(cdf.delta_lf[i + hdr.delta.lf.multi]), 3));
          if (delta_lf == 3) {
            const int n_bits = 1 + static_cast<int>(msac.bools(3));
            delta_lf = static_cast<int>(msac.bools(static_cast<unsigned>(n_bits))) + 1 + (1 << n_bits);
          }
          if (delta_lf) {
            if (msac.bool_equi()) delta_lf = -delta_lf;
            delta_lf *= 1 << hdr.delta.lf.res_log2;
          }
          ts.last_delta_lf[i] = static_cast<int8_t>(iclip(ts.last_delta_lf[i] + delta_lf, -63, 63));
        }
      }
    }
    if (ts.last_qidx == hdr.quant.yac) {
      // assign frame-wide q values to this sb
      ts.dq = &f.dq;
    } else if (ts.last_qidx != prev_qidx) {
      // find sb-specific quant parameters
      init_quant_tables(seq, hdr, ts.last_qidx, ts.dqmem);
      ts.dq = &ts.dqmem;
    }
    f.lfm.set_delta_lf(ts.last_delta_lf);
  }

  if (b.skip_mode) {
    b.intra = 0;
  } else if (inter_frame) {
    if (seg && (seg->ref >= 0 || seg->globalmv)) {
      b.intra = !seg->ref;
    } else {
      const int ictx = get_intra_ctx(*t.a, t.l, by4, bx4, have_top, have_left);
      b.intra = !bool_adapt(cdf.intra[ictx]);
    }
  } else if (hdr.allow_intrabc) {
    b.intra = !bool_adapt(cdf.intrabc);
  } else {
    b.intra = 1;
  }

  // intra/inter-specific stuff
  if (b.intra) {
    ArrayView<uint16_t> ymode_cdf = inter_frame
                                        ? ArrayView<uint16_t>(cdf.y_mode[k_ymode_size_context[bs]])
                                        : ArrayView<uint16_t>(ts.cdf.kfym[k_intra_mode_context[t.a->mode[bx4]]][k_intra_mode_context[t.l.mode[by4]]]);
    b.y_mode = static_cast<uint8_t>(msac.symbol_adapt16(ymode_cdf, N_INTRA_PRED_MODES - 1));

    // angle delta
    if (b_dim[2] + b_dim[3] >= 2 && b.y_mode >= VERT_PRED && b.y_mode <= VERT_LEFT_PRED) {
      const int angle = static_cast<int>(msac.symbol_adapt8(ArrayView<uint16_t>(cdf.angle_delta[b.y_mode - VERT_PRED]), 6));
      b.y_angle = static_cast<int8_t>(angle - 3);
    } else {
      b.y_angle = 0;
    }

    if (has_chroma) {
      const bool cfl_allowed = hdr.segmentation.lossless[b.seg_id] ? cbw4 == 1 && cbh4 == 1 : !!(cfl_allowed_mask & (1u << bs));
      ArrayView<uint16_t> uvmode_cdf(cdf.uv_mode[cfl_allowed][b.y_mode]);
      b.uv_mode = static_cast<uint8_t>(msac.symbol_adapt16(uvmode_cdf, N_UV_INTRA_PRED_MODES - 1 - !cfl_allowed));

      b.uv_angle = 0;
      if (b.uv_mode == CFL_PRED) {
        const int sign = static_cast<int>(msac.symbol_adapt8(ArrayView<uint16_t>(cdf.cfl_sign), 7)) + 1;
        const int sign_u = sign * 0x56 >> 8, sign_v = sign - sign_u * 3;
        if (sign_u) {
          const int ctx = (sign_u == 2) * 3 + sign_v;
          b.cfl_alpha[0] = static_cast<int8_t>(static_cast<int>(msac.symbol_adapt16(ArrayView<uint16_t>(cdf.cfl_alpha[ctx]), 15)) + 1);
          if (sign_u == 1) b.cfl_alpha[0] = static_cast<int8_t>(-b.cfl_alpha[0]);
        } else {
          b.cfl_alpha[0] = 0;
        }
        if (sign_v) {
          const int ctx = (sign_v == 2) * 3 + sign_u;
          b.cfl_alpha[1] = static_cast<int8_t>(static_cast<int>(msac.symbol_adapt16(ArrayView<uint16_t>(cdf.cfl_alpha[ctx]), 15)) + 1);
          if (sign_v == 1) b.cfl_alpha[1] = static_cast<int8_t>(-b.cfl_alpha[1]);
        } else {
          b.cfl_alpha[1] = 0;
        }
      } else if (b_dim[2] + b_dim[3] >= 2 && b.uv_mode >= VERT_PRED && b.uv_mode <= VERT_LEFT_PRED) {
        const int angle = static_cast<int>(msac.symbol_adapt8(ArrayView<uint16_t>(cdf.angle_delta[b.uv_mode - VERT_PRED]), 6));
        b.uv_angle = static_cast<int8_t>(angle - 3);
      }
    }

    b.pal_sz[0] = b.pal_sz[1] = 0;
    if (hdr.allow_screen_content_tools && imax(bw4, bh4) <= 16 && bw4 + bh4 >= 4) {
      const int sz_ctx = b_dim[2] + b_dim[3] - 2;
      if (b.y_mode == DC_PRED) {
        const int pal_ctx = (t.a->pal_sz[bx4] > 0) + (t.l.pal_sz[by4] > 0);
        const int use_y_pal = bool_adapt(cdf.pal_y[sz_ctx][pal_ctx]);
        if (use_y_pal) read_pal_plane(t, b, false, sz_ctx, bx4, by4);
      }
      if (has_chroma && b.uv_mode == DC_PRED) {
        const int pal_ctx = b.pal_sz[0] > 0;
        const int use_uv_pal = bool_adapt(cdf.pal_uv[pal_ctx]);
        if (use_uv_pal) read_pal_uv(t, b, sz_ctx, bx4, by4);  // see aomedia bug 2183 for why we use luma coordinates
      }
    }

    if (b.y_mode == DC_PRED && !b.pal_sz[0] && imax(b_dim[2], b_dim[3]) <= 3 && seq.filter_intra) {
      const int is_filter = bool_adapt(cdf.use_filter_intra[bs]);
      if (is_filter) {
        b.y_mode = FILTER_PRED;
        b.y_angle = static_cast<int8_t>(msac.symbol_adapt8(ArrayView<uint16_t>(cdf.filter_intra), 4));
      }
    }

    if (b.pal_sz[0]) {
      if (!read_pal_indices(t, ArrayView<uint8_t>(t.scratch.pal_idx_y), b.pal_sz[0], 0, w4, h4, bw4, bh4)) return false;
    }
    if (has_chroma && b.pal_sz[1]) {
      if (!read_pal_indices(t, ArrayView<uint8_t>(t.scratch.pal_idx_uv), b.pal_sz[1], 1, cw4, ch4, cbw4, cbh4)) return false;
    }

    const TxfmInfo* t_dim;
    if (hdr.segmentation.lossless[b.seg_id]) {
      b.tx = b.uvtx = static_cast<uint8_t>(TX_4X4);
      t_dim = &k_txfm_dimensions[TX_4X4];
    } else {
      b.tx = k_max_txfm_size_for_bs[bs][0];
      b.uvtx = k_max_txfm_size_for_bs[bs][f.layout];
      t_dim = &k_txfm_dimensions[b.tx];
      if (hdr.txfm_mode == TX_SWITCHABLE && t_dim->max > TX_4X4) {
        const int tctx = get_tx_ctx(*t.a, t.l, *t_dim, by4, bx4);
        ArrayView<uint16_t> tx_cdf(cdf.txsz[t_dim->max - 1][tctx]);
        int depth = static_cast<int>(msac.symbol_adapt4(tx_cdf, static_cast<unsigned>(imin(t_dim->max, 2))));
        while (depth--) {
          b.tx = t_dim->sub;
          t_dim = &k_txfm_dimensions[b.tx];
        }
      }
    }

    // reconstruction
    if (!recon_b_intra(t, bs, intra_edge_flags, b)) return false;

    f.lfm.create_lf_mask_intra(t.bx, t.by, bs, static_cast<RectTxfmSize>(b.tx), static_cast<RectTxfmSize>(b.uvtx), b.seg_id, has_chroma);

    // update contexts
    const int y_mode_nofilt = b.y_mode == FILTER_PRED ? static_cast<int>(DC_PRED) : static_cast<int>(b.y_mode);
    for (int i = 0; i < 2; i++) {
      BlockContext& edge = i ? t.l : *t.a;
      const int off = i ? by4 : bx4;
      const int n = b_dim[2 + i];
      const int t_lsz = i ? t_dim->lh : t_dim->lw;
      set_ctx(edge.tx_intra, off, n, t_lsz);
      set_ctx(edge.tx, off, n, t_lsz);
      set_ctx(edge.mode, off, n, y_mode_nofilt);
      set_ctx(edge.pal_sz, off, n, b.pal_sz[0]);
      set_ctx(edge.seg_pred, off, n, seg_pred);
      set_ctx(edge.skip_mode, off, n, 0);
      set_ctx(edge.intra, off, n, 1);
      set_ctx(edge.skip, off, n, b.skip);
      // see aomedia bug 2183 for why we use luma coordinates here
      set_ctx(t.pal_sz_uv[i], off, n, has_chroma ? b.pal_sz[1] : 0);
      if (inter_frame) {
        set_ctx(edge.comp_type, off, n, COMP_INTER_NONE);
        set_ctx(edge.ref[0], off, n, -1);
        set_ctx(edge.ref[1], off, n, -1);
        set_ctx(edge.filter[0], off, n, N_SWITCHABLE_FILTERS);
        set_ctx(edge.filter[1], off, n, N_SWITCHABLE_FILTERS);
      }
    }
    if (b.pal_sz[0]) copy_pal_block_y(t, bx4, by4, bw4, bh4);
    if (has_chroma) {
      set_ctx(t.a->uvmode, cbx4, ulog2(static_cast<unsigned>(cbw4)), b.uv_mode);
      set_ctx(t.l.uvmode, cby4, ulog2(static_cast<unsigned>(cbh4)), b.uv_mode);
      if (b.pal_sz[1]) copy_pal_block_uv(t, bx4, by4, bw4, bh4);
    }
    if (inter_frame || hdr.allow_intrabc) splat_intraref(f, t, bs, bw4, bh4);
  } else if (is_key_or_intra(hdr)) {
    // intra block copy
    RefmvsCandidate mvstack[8] = {};
    int n_mvs, ctx;
    RefmvsRefPair rp;
    rp.ref[0] = 0;
    rp.ref[1] = -1;
    f.rf.find(t.rt, ArrayView<RefmvsCandidate>(mvstack), n_mvs, ctx, rp, bs, (intra_edge_flags & EDGE_I444_TOP_HAS_RIGHT) != 0, t.by, t.bx);

    if (mvstack[0].mv.mv[0].n)
      b.mv[0] = mvstack[0].mv.mv[0];
    else if (mvstack[1].mv.mv[0].n)
      b.mv[0] = mvstack[1].mv.mv[0];
    else if (t.by - (16 << seq.sb128) < ts.tiling.row_start) {
      b.mv[0].y = 0;
      b.mv[0].x = static_cast<int16_t>(-(512 << seq.sb128) - 2048);
    } else {
      b.mv[0].y = static_cast<int16_t>(-(512 << seq.sb128));
      b.mv[0].x = 0;
    }

    read_mv_residual(ts, b.mv[0], -1);

    // clip intrabc motion vector to decoded parts of current tile
    int border_left = ts.tiling.col_start * 4;
    int border_top = ts.tiling.row_start * 4;
    if (has_chroma) {
      if (bw4 < 2 && ss_hor) border_left += 4;
      if (bh4 < 2 && ss_ver) border_top += 4;
    }
    int src_left = t.bx * 4 + (b.mv[0].x >> 3);
    int src_top = t.by * 4 + (b.mv[0].y >> 3);
    int src_right = src_left + bw4 * 4;
    int src_bottom = src_top + bh4 * 4;
    const int border_right = ((ts.tiling.col_end + (bw4 - 1)) & ~(bw4 - 1)) * 4;

    // check against left or right tile boundary and adjust if necessary
    if (src_left < border_left) {
      src_right += border_left - src_left;
      src_left += border_left - src_left;
    } else if (src_right > border_right) {
      src_left -= src_right - border_right;
      src_right -= src_right - border_right;
    }
    // check against top tile boundary and adjust if necessary
    if (src_top < border_top) {
      src_bottom += border_top - src_top;
      src_top += border_top - src_top;
    }

    const int sbx = (t.bx >> (4 + seq.sb128)) << (6 + seq.sb128);
    const int sby = (t.by >> (4 + seq.sb128)) << (6 + seq.sb128);
    const int sb_size = 1 << (6 + seq.sb128);
    // check for overlap with current superblock
    if (src_bottom > sby && src_right > sbx) {
      if (src_top - border_top >= src_bottom - sby) {
        // if possible move src up into the previous suberblock row
        src_top -= src_bottom - sby;
        src_bottom -= src_bottom - sby;
      } else if (src_left - border_left >= src_right - sbx) {
        // if possible move src left into the previous suberblock
        src_left -= src_right - sbx;
        src_right -= src_right - sbx;
      }
    }
    // move src up if it is below current superblock row
    if (src_bottom > sby + sb_size) {
      src_top -= src_bottom - (sby + sb_size);
      src_bottom -= src_bottom - (sby + sb_size);
    }
    // error out if mv still overlaps with the current superblock
    if (src_bottom > sby && src_right > sbx) return false;

    b.mv[0].x = static_cast<int16_t>((src_left - t.bx * 4) * 8);
    b.mv[0].y = static_cast<int16_t>((src_top - t.by * 4) * 8);

    read_vartx_tree(t, b, bs, bx4, by4);

    // reconstruction
    if (!recon_b_inter(t, bs, b)) return false;

    splat_intrabc_mv(f, t, bs, b, bw4, bh4);
    for (int i = 0; i < 2; i++) {
      BlockContext& edge = i ? t.l : *t.a;
      const int off = i ? by4 : bx4;
      const int n = b_dim[2 + i];
      set_ctx(edge.tx_intra, off, n, b_dim[2 + i]);
      set_ctx(edge.mode, off, n, DC_PRED);
      set_ctx(edge.pal_sz, off, n, 0);
      // see aomedia bug 2183 for why this is outside if (has_chroma)
      set_ctx(t.pal_sz_uv[i], off, n, 0);
      set_ctx(edge.seg_pred, off, n, seg_pred);
      set_ctx(edge.skip_mode, off, n, 0);
      set_ctx(edge.intra, off, n, 0);
      set_ctx(edge.skip, off, n, b.skip);
    }
    if (has_chroma) {
      set_ctx(t.a->uvmode, cbx4, ulog2(static_cast<unsigned>(cbw4)), DC_PRED);
      set_ctx(t.l.uvmode, cby4, ulog2(static_cast<unsigned>(cbh4)), DC_PRED);
    }
  } else {
    // inter-specific mode/mv coding
    bool is_comp;
    int has_subpel_filter;

    if (b.skip_mode) {
      is_comp = true;
    } else if ((!seg || (seg->ref == -1 && !seg->globalmv && !seg->skip)) && hdr.switchable_comp_refs && imin(bw4, bh4) > 1) {
      const int ctx = get_comp_ctx(*t.a, t.l, by4, bx4, have_top, have_left);
      is_comp = bool_adapt(cdf.comp[ctx]);
    } else {
      is_comp = false;
    }

    if (b.skip_mode) {
      b.ref[0] = hdr.skip_mode_refs[0];
      b.ref[1] = hdr.skip_mode_refs[1];
      b.comp_type = COMP_INTER_AVG;
      b.inter_mode = NEARESTMV_NEARESTMV;
      b.drl_idx = NEAREST_DRL;
      has_subpel_filter = 0;

      RefmvsCandidate mvstack[8] = {};
      int n_mvs, ctx;
      RefmvsRefPair rp;
      rp.ref[0] = static_cast<int8_t>(b.ref[0] + 1);
      rp.ref[1] = static_cast<int8_t>(b.ref[1] + 1);
      f.rf.find(t.rt, ArrayView<RefmvsCandidate>(mvstack), n_mvs, ctx, rp, bs, (intra_edge_flags & EDGE_I444_TOP_HAS_RIGHT) != 0, t.by, t.bx);

      b.mv[0] = mvstack[0].mv.mv[0];
      b.mv[1] = mvstack[0].mv.mv[1];
      fix_mv_precision(hdr, b.mv[0]);
      fix_mv_precision(hdr, b.mv[1]);
    } else if (is_comp) {
      const int dir_ctx = get_comp_dir_ctx(*t.a, t.l, by4, bx4, have_top, have_left);
      if (bool_adapt(cdf.comp_dir[dir_ctx])) {
        // bidir - first reference (fw)
        const int ctx1 = get_fwd_ref_ctx(*t.a, t.l, by4, bx4, have_top, have_left);
        if (bool_adapt(cdf.comp_fwd_ref[0][ctx1])) {
          const int ctx2 = get_fwd_ref_2_ctx(*t.a, t.l, by4, bx4, have_top, have_left);
          b.ref[0] = static_cast<int8_t>(2 + bool_adapt(cdf.comp_fwd_ref[2][ctx2]));
        } else {
          const int ctx2 = get_fwd_ref_1_ctx(*t.a, t.l, by4, bx4, have_top, have_left);
          b.ref[0] = static_cast<int8_t>(bool_adapt(cdf.comp_fwd_ref[1][ctx2]));
        }

        // second reference (bw)
        const int ctx3 = get_bwd_ref_ctx(*t.a, t.l, by4, bx4, have_top, have_left);
        if (bool_adapt(cdf.comp_bwd_ref[0][ctx3])) {
          b.ref[1] = 6;
        } else {
          const int ctx4 = get_bwd_ref_1_ctx(*t.a, t.l, by4, bx4, have_top, have_left);
          b.ref[1] = static_cast<int8_t>(4 + bool_adapt(cdf.comp_bwd_ref[1][ctx4]));
        }
      } else {
        // unidir
        const int uctx_p = get_ref_ctx(*t.a, t.l, by4, bx4, have_top, have_left);
        if (bool_adapt(cdf.comp_uni_ref[0][uctx_p])) {
          b.ref[0] = 4;
          b.ref[1] = 6;
        } else {
          const int uctx_p1 = get_uni_p1_ctx(*t.a, t.l, by4, bx4, have_top, have_left);
          b.ref[0] = 0;
          b.ref[1] = static_cast<int8_t>(1 + bool_adapt(cdf.comp_uni_ref[1][uctx_p1]));
          if (b.ref[1] == 2) {
            const int uctx_p2 = get_fwd_ref_2_ctx(*t.a, t.l, by4, bx4, have_top, have_left);
            b.ref[1] = static_cast<int8_t>(b.ref[1] + bool_adapt(cdf.comp_uni_ref[2][uctx_p2]));
          }
        }
      }

      RefmvsCandidate mvstack[8] = {};
      int n_mvs, ctx;
      RefmvsRefPair rp;
      rp.ref[0] = static_cast<int8_t>(b.ref[0] + 1);
      rp.ref[1] = static_cast<int8_t>(b.ref[1] + 1);
      f.rf.find(t.rt, ArrayView<RefmvsCandidate>(mvstack), n_mvs, ctx, rp, bs, (intra_edge_flags & EDGE_I444_TOP_HAS_RIGHT) != 0, t.by, t.bx);

      b.inter_mode = static_cast<uint8_t>(msac.symbol_adapt8(ArrayView<uint16_t>(cdf.comp_inter_mode[ctx]), N_COMP_INTER_PRED_MODES - 1));

      const uint8_t* const im = k_comp_inter_pred_modes[b.inter_mode];
      b.drl_idx = NEAREST_DRL;
      const ArrayView<const RefmvsCandidate> mvs(mvstack);
      if (b.inter_mode == NEWMV_NEWMV) {
        if (n_mvs > 1) {  // NEARER, NEAR or NEARISH
          const int drl_ctx_v1 = get_drl_context(mvs, 0);
          b.drl_idx = static_cast<uint8_t>(b.drl_idx + bool_adapt(cdf.drl_bit[drl_ctx_v1]));
          if (b.drl_idx == NEARER_DRL && n_mvs > 2) {
            const int drl_ctx_v2 = get_drl_context(mvs, 1);
            b.drl_idx = static_cast<uint8_t>(b.drl_idx + bool_adapt(cdf.drl_bit[drl_ctx_v2]));
          }
        }
      } else if (im[0] == NEARMV || im[1] == NEARMV) {
        b.drl_idx = NEARER_DRL;
        if (n_mvs > 2) {  // NEAR or NEARISH
          const int drl_ctx_v2 = get_drl_context(mvs, 1);
          b.drl_idx = static_cast<uint8_t>(b.drl_idx + bool_adapt(cdf.drl_bit[drl_ctx_v2]));
          if (b.drl_idx == NEAR_DRL && n_mvs > 3) {
            const int drl_ctx_v3 = get_drl_context(mvs, 2);
            b.drl_idx = static_cast<uint8_t>(b.drl_idx + bool_adapt(cdf.drl_bit[drl_ctx_v3]));
          }
        }
      }

      has_subpel_filter = imin(bw4, bh4) == 1 || b.inter_mode != GLOBALMV_GLOBALMV;
      for (int idx = 0; idx < 2; idx++) {
        switch (im[idx]) {
          case NEARMV:
          case NEARESTMV:
            b.mv[idx] = mvstack[b.drl_idx].mv.mv[idx];
            fix_mv_precision(hdr, b.mv[idx]);
            break;
          case GLOBALMV:
            has_subpel_filter |= hdr.gmv[b.ref[idx]].type == WM_TYPE_TRANSLATION;
            b.mv[idx] = get_gmv_2d(hdr.gmv[b.ref[idx]], t.bx, t.by, bw4, bh4, hdr);
            break;
          case NEWMV: {
            b.mv[idx] = mvstack[b.drl_idx].mv.mv[idx];
            const int mv_prec = hdr.hp - hdr.force_integer_mv;
            read_mv_residual(ts, b.mv[idx], mv_prec);
            break;
          }
        }
      }

      // jnt_comp vs. seg vs. wedge
      int is_segwedge = 0;
      if (seq.masked_compound) {
        const int mask_ctx = get_mask_comp_ctx(*t.a, t.l, by4, bx4);
        is_segwedge = bool_adapt(cdf.mask_comp[mask_ctx]);
      }

      if (!is_segwedge) {
        if (seq.jnt_comp) {
          const Picture& p0 = *f.refp[b.ref[0]];
          const Picture& p1 = *f.refp[b.ref[1]];
          const int jnt_ctx = get_jnt_comp_ctx(seq.order_hint_n_bits, hdr.frame_offset, p0.hdr.frame_offset, p1.hdr.frame_offset, *t.a, t.l, by4, bx4);
          b.comp_type = static_cast<uint8_t>(COMP_INTER_WEIGHTED_AVG + bool_adapt(cdf.jnt_comp[jnt_ctx]));
        } else {
          b.comp_type = COMP_INTER_AVG;
        }
      } else {
        if (wedge_allowed_mask & (1u << bs)) {
          const int ctx = k_wedge_ctx_lut[bs];
          b.comp_type = static_cast<uint8_t>(COMP_INTER_WEDGE - bool_adapt(cdf.wedge_comp[ctx]));
          if (b.comp_type == COMP_INTER_WEDGE) b.wedge_idx = static_cast<uint8_t>(msac.symbol_adapt16(ArrayView<uint16_t>(cdf.wedge_idx[ctx]), 15));
        } else {
          b.comp_type = COMP_INTER_SEG;
        }
        b.mask_sign = static_cast<uint8_t>(msac.bool_equi());
      }
    } else {
      b.comp_type = COMP_INTER_NONE;

      // ref
      if (seg && seg->ref > 0) {
        b.ref[0] = static_cast<int8_t>(seg->ref - 1);
      } else if (seg && (seg->globalmv || seg->skip)) {
        b.ref[0] = 0;
      } else {
        const int ctx1 = get_ref_ctx(*t.a, t.l, by4, bx4, have_top, have_left);
        if (bool_adapt(cdf.ref[0][ctx1])) {
          const int ctx2 = get_bwd_ref_ctx(*t.a, t.l, by4, bx4, have_top, have_left);  // av1_get_ref_2_ctx
          if (bool_adapt(cdf.ref[1][ctx2])) {
            b.ref[0] = 6;
          } else {
            const int ctx3 = get_bwd_ref_1_ctx(*t.a, t.l, by4, bx4, have_top, have_left);  // av1_get_ref_6_ctx
            b.ref[0] = static_cast<int8_t>(4 + bool_adapt(cdf.ref[5][ctx3]));
          }
        } else {
          const int ctx2 = get_fwd_ref_ctx(*t.a, t.l, by4, bx4, have_top, have_left);  // av1_get_ref_3_ctx
          if (bool_adapt(cdf.ref[2][ctx2])) {
            const int ctx3 = get_fwd_ref_2_ctx(*t.a, t.l, by4, bx4, have_top, have_left);  // av1_get_ref_5_ctx
            b.ref[0] = static_cast<int8_t>(2 + bool_adapt(cdf.ref[4][ctx3]));
          } else {
            const int ctx3 = get_fwd_ref_1_ctx(*t.a, t.l, by4, bx4, have_top, have_left);  // av1_get_ref_4_ctx
            b.ref[0] = static_cast<int8_t>(bool_adapt(cdf.ref[3][ctx3]));
          }
        }
      }
      b.ref[1] = -1;

      RefmvsCandidate mvstack[8] = {};
      int n_mvs, ctx;
      RefmvsRefPair rp;
      rp.ref[0] = static_cast<int8_t>(b.ref[0] + 1);
      rp.ref[1] = -1;
      f.rf.find(t.rt, ArrayView<RefmvsCandidate>(mvstack), n_mvs, ctx, rp, bs, (intra_edge_flags & EDGE_I444_TOP_HAS_RIGHT) != 0, t.by, t.bx);
      const ArrayView<const RefmvsCandidate> mvs(mvstack);

      // mode parsing and mv derivation from ref_mvs
      if ((seg && (seg->skip || seg->globalmv)) || bool_adapt(cdf.newmv_mode[ctx & 7])) {
        if ((seg && (seg->skip || seg->globalmv)) || !bool_adapt(cdf.globalmv_mode[(ctx >> 3) & 1])) {
          b.inter_mode = GLOBALMV;
          b.mv[0] = get_gmv_2d(hdr.gmv[b.ref[0]], t.bx, t.by, bw4, bh4, hdr);
          has_subpel_filter = imin(bw4, bh4) == 1 || hdr.gmv[b.ref[0]].type == WM_TYPE_TRANSLATION;
        } else {
          has_subpel_filter = 1;
          if (bool_adapt(cdf.refmv_mode[(ctx >> 4) & 15])) {  // NEAREST, NEARER, NEAR or NEARISH
            b.inter_mode = NEARMV;
            b.drl_idx = NEARER_DRL;
            if (n_mvs > 2) {  // NEARER, NEAR or NEARISH
              const int drl_ctx_v2 = get_drl_context(mvs, 1);
              b.drl_idx = static_cast<uint8_t>(b.drl_idx + bool_adapt(cdf.drl_bit[drl_ctx_v2]));
              if (b.drl_idx == NEAR_DRL && n_mvs > 3) {  // NEAR or NEARISH
                const int drl_ctx_v3 = get_drl_context(mvs, 2);
                b.drl_idx = static_cast<uint8_t>(b.drl_idx + bool_adapt(cdf.drl_bit[drl_ctx_v3]));
              }
            }
          } else {
            b.inter_mode = NEARESTMV;
            b.drl_idx = NEAREST_DRL;
          }
          b.mv[0] = mvstack[b.drl_idx].mv.mv[0];
          if (b.drl_idx < NEAR_DRL) fix_mv_precision(hdr, b.mv[0]);
        }
      } else {
        has_subpel_filter = 1;
        b.inter_mode = NEWMV;
        b.drl_idx = NEAREST_DRL;
        if (n_mvs > 1) {  // NEARER, NEAR or NEARISH
          const int drl_ctx_v1 = get_drl_context(mvs, 0);
          b.drl_idx = static_cast<uint8_t>(b.drl_idx + bool_adapt(cdf.drl_bit[drl_ctx_v1]));
          if (b.drl_idx == NEARER_DRL && n_mvs > 2) {  // NEAR or NEARISH
            const int drl_ctx_v2 = get_drl_context(mvs, 1);
            b.drl_idx = static_cast<uint8_t>(b.drl_idx + bool_adapt(cdf.drl_bit[drl_ctx_v2]));
          }
        }
        if (n_mvs > 1) {
          b.mv[0] = mvstack[b.drl_idx].mv.mv[0];
        } else {
          b.mv[0] = mvstack[0].mv.mv[0];
          fix_mv_precision(hdr, b.mv[0]);
        }
        const int mv_prec = hdr.hp - hdr.force_integer_mv;
        read_mv_residual(ts, b.mv[0], mv_prec);
      }

      // interintra flags
      const int ii_sz_grp = k_ymode_size_context[bs];
      if (seq.inter_intra && (interintra_allowed_mask & (1u << bs)) && bool_adapt(cdf.interintra[ii_sz_grp])) {
        b.interintra_mode = static_cast<uint8_t>(msac.symbol_adapt4(ArrayView<uint16_t>(cdf.interintra_mode[ii_sz_grp]), N_INTER_INTRA_PRED_MODES - 1));
        const int wedge_ctx = k_wedge_ctx_lut[bs];
        b.interintra_type = static_cast<uint8_t>(INTER_INTRA_BLEND + bool_adapt(cdf.interintra_wedge[wedge_ctx]));
        if (b.interintra_type == INTER_INTRA_WEDGE) b.wedge_idx = static_cast<uint8_t>(msac.symbol_adapt16(ArrayView<uint16_t>(cdf.wedge_idx[wedge_ctx]), 15));
      } else {
        b.interintra_type = INTER_INTRA_NONE;
      }

      // motion variation
      if (hdr.switchable_motion_mode && b.interintra_type == INTER_INTRA_NONE && imin(bw4, bh4) >= 2 &&
          // is not warped global motion
          !(!hdr.force_integer_mv && b.inter_mode == GLOBALMV && hdr.gmv[b.ref[0]].type > WM_TYPE_TRANSLATION) &&
          // has overlappable neighbours
          ((have_left && findoddzero(ArrayView<const uint8_t>(t.l.intra).subview(static_cast<uint64_t>(by4) + 1), h4 >> 1)) ||
           (have_top && findoddzero(ArrayView<const uint8_t>(t.a->intra).subview(static_cast<uint64_t>(bx4) + 1), w4 >> 1)))) {
        // reaching here means the block allows obmc - check warp by finding matching-ref blocks in top/left edges
        uint64_t mask[2] = {0, 0};
        find_matching_ref(f, t, intra_edge_flags, bw4, bh4, w4, h4, have_left, have_top, b.ref[0], mask);
        const bool allow_warp = !f.svc[b.ref[0]][0].scale && !hdr.force_integer_mv && hdr.warp_motion && (mask[0] | mask[1]);

        b.motion_mode = allow_warp ? static_cast<uint8_t>(msac.symbol_adapt4(ArrayView<uint16_t>(cdf.motion_mode[bs]), 2))
                                   : static_cast<uint8_t>(bool_adapt(cdf.obmc[bs]));
        if (b.motion_mode == MM_WARP) {
          has_subpel_filter = 0;
          derive_warpmv(f, t, bw4, bh4, mask, b.mv[0], t.warpmv);
        }
      } else {
        b.motion_mode = MM_TRANSLATION;
      }
    }

    // subpel filter
    int filter[2];
    if (hdr.subpel_filter_mode == FILTER_SWITCHABLE) {
      if (has_subpel_filter) {
        const int comp = b.comp_type != COMP_INTER_NONE;
        const int ctx1 = get_filter_ctx(*t.a, t.l, comp, 0, b.ref[0], by4, bx4);
        filter[0] = static_cast<int>(msac.symbol_adapt4(ArrayView<uint16_t>(cdf.filter[0][ctx1]), N_SWITCHABLE_FILTERS - 1));
        if (seq.dual_filter) {
          const int ctx2 = get_filter_ctx(*t.a, t.l, comp, 1, b.ref[0], by4, bx4);
          filter[1] = static_cast<int>(msac.symbol_adapt4(ArrayView<uint16_t>(cdf.filter[1][ctx2]), N_SWITCHABLE_FILTERS - 1));
        } else {
          filter[1] = filter[0];
        }
      } else {
        filter[0] = filter[1] = FILTER_8TAP_REGULAR;
      }
    } else {
      filter[0] = filter[1] = hdr.subpel_filter_mode;
    }
    b.filter2d = k_filter_2d[filter[1]][filter[0]];

    read_vartx_tree(t, b, bs, bx4, by4);

    // reconstruction
    if (!recon_b_inter(t, bs, b)) return false;

    {
      const bool is_globalmv = b.inter_mode == (is_comp ? static_cast<int>(GLOBALMV_GLOBALMV) : static_cast<int>(GLOBALMV));
      f.lfm.create_lf_mask_inter(t.bx, t.by, bs, b.skip, static_cast<RectTxfmSize>(b.max_ytx), b.tx_split0, b.tx_split1,
                                 static_cast<RectTxfmSize>(b.uvtx), b.seg_id, b.ref[0], is_globalmv, has_chroma);
    }

    // context updates
    if (is_comp)
      splat_tworef_mv(f, t, bs, b, bw4, bh4);
    else
      splat_oneref_mv(f, t, bs, b, bw4, bh4);
    for (int i = 0; i < 2; i++) {
      BlockContext& edge = i ? t.l : *t.a;
      const int off = i ? by4 : bx4;
      const int n = b_dim[2 + i];
      set_ctx(edge.seg_pred, off, n, seg_pred);
      set_ctx(edge.skip_mode, off, n, b.skip_mode);
      set_ctx(edge.intra, off, n, 0);
      set_ctx(edge.skip, off, n, b.skip);
      set_ctx(edge.pal_sz, off, n, 0);
      // see aomedia bug 2183 for why this is outside if (has_chroma)
      set_ctx(t.pal_sz_uv[i], off, n, 0);
      set_ctx(edge.tx_intra, off, n, b_dim[2 + i]);
      set_ctx(edge.comp_type, off, n, b.comp_type);
      set_ctx(edge.filter[0], off, n, filter[0]);
      set_ctx(edge.filter[1], off, n, filter[1]);
      set_ctx(edge.mode, off, n, b.inter_mode);
      set_ctx(edge.ref[0], off, n, b.ref[0]);
      set_ctx(edge.ref[1], off, n, b.ref[1]);
    }
    if (has_chroma) {
      set_ctx(t.a->uvmode, cbx4, ulog2(static_cast<unsigned>(cbw4)), DC_PRED);
      set_ctx(t.l.uvmode, cby4, ulog2(static_cast<unsigned>(cbh4)), DC_PRED);
    }
  }

  // update contexts
  if (hdr.segmentation.enabled && hdr.segmentation.update_map) {
    const ArrayView<uint8_t> map = f.cur_segmap->data.view();
    int64_t pos = static_cast<int64_t>(t.by) * f.b4_stride + t.bx;
    for (int y = 0; y < bh4; y++) {
      map.subview(static_cast<uint64_t>(pos), static_cast<uint64_t>(bw4)).fill(b.seg_id);
      pos += f.b4_stride;
    }
  }
  if (!b.skip) f.lfm.mark_coded(t.bx, t.by, bs);

  return true;
}

bool Core::decode_sb(TaskContext& t, BlockLevel bl, const EdgeNode& node) {
  FrameContext& f = *t.f;
  TileState& ts = *t.ts;
  const int hsz = 16 >> bl;
  const bool have_h_split = f.bw > t.bx + hsz;
  const bool have_v_split = f.bh > t.by + hsz;

  if (!have_h_split && !have_v_split) {
    if (bl >= BL_8X8) return false;
    return decode_sb(t, static_cast<BlockLevel>(bl + 1), k_intra_edge_tree.split(node, 0));
  }

  BlockPartition bp;
  const int bx8 = (t.bx & 31) >> 1;
  const int by8 = (t.by & 31) >> 1;
  const int ctx = get_partition_ctx(*t.a, t.l, bl, by8, bx8);
  ArrayView<uint16_t> pc(ts.cdf.m.partition[bl][ctx]);

  if (have_h_split && have_v_split) {
    bp = static_cast<BlockPartition>(ts.msac.symbol_adapt16(pc, k_partition_type_count[bl]));
    if (f.layout == PIXEL_LAYOUT_I422 &&
        (bp == PARTITION_V || bp == PARTITION_V4 || bp == PARTITION_T_LEFT_SPLIT || bp == PARTITION_T_RIGHT_SPLIT))
      return false;
    const uint8_t* const b = k_block_sizes[bl][bp];

    switch (bp) {
      case PARTITION_NONE:
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_NONE, node.o)) return false;
        break;
      case PARTITION_H:
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_H, node.h[0])) return false;
        t.by += hsz;
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_H, node.h[1])) return false;
        t.by -= hsz;
        break;
      case PARTITION_V:
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_V, node.v[0])) return false;
        t.bx += hsz;
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_V, node.v[1])) return false;
        t.bx -= hsz;
        break;
      case PARTITION_SPLIT:
        if (bl == BL_8X8) {
          if (!decode_b(t, bl, BS_4x4, PARTITION_SPLIT, EDGE_ALL_TR_AND_BL)) return false;
          const int tl_filter = t.tl_4x4_filter;
          t.bx++;
          if (!decode_b(t, bl, BS_4x4, PARTITION_SPLIT, node.split[0])) return false;
          t.bx--;
          t.by++;
          if (!decode_b(t, bl, BS_4x4, PARTITION_SPLIT, node.split[1])) return false;
          t.bx++;
          t.tl_4x4_filter = tl_filter;
          if (!decode_b(t, bl, BS_4x4, PARTITION_SPLIT, node.split[2])) return false;
          t.bx--;
          t.by--;
        } else {
          const BlockLevel next = static_cast<BlockLevel>(bl + 1);
          if (!decode_sb(t, next, k_intra_edge_tree.split(node, 0))) return false;
          t.bx += hsz;
          if (!decode_sb(t, next, k_intra_edge_tree.split(node, 1))) return false;
          t.bx -= hsz;
          t.by += hsz;
          if (!decode_sb(t, next, k_intra_edge_tree.split(node, 2))) return false;
          t.bx += hsz;
          if (!decode_sb(t, next, k_intra_edge_tree.split(node, 3))) return false;
          t.bx -= hsz;
          t.by -= hsz;
        }
        break;
      case PARTITION_T_TOP_SPLIT:
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_T_TOP_SPLIT, EDGE_ALL_TR_AND_BL)) return false;
        t.bx += hsz;
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_T_TOP_SPLIT, node.v[1])) return false;
        t.bx -= hsz;
        t.by += hsz;
        if (!decode_b(t, bl, static_cast<BlockSize>(b[1]), PARTITION_T_TOP_SPLIT, node.h[1])) return false;
        t.by -= hsz;
        break;
      case PARTITION_T_BOTTOM_SPLIT:
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_T_BOTTOM_SPLIT, node.h[0])) return false;
        t.by += hsz;
        if (!decode_b(t, bl, static_cast<BlockSize>(b[1]), PARTITION_T_BOTTOM_SPLIT, node.v[0])) return false;
        t.bx += hsz;
        if (!decode_b(t, bl, static_cast<BlockSize>(b[1]), PARTITION_T_BOTTOM_SPLIT, 0)) return false;
        t.bx -= hsz;
        t.by -= hsz;
        break;
      case PARTITION_T_LEFT_SPLIT:
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_T_LEFT_SPLIT, EDGE_ALL_TR_AND_BL)) return false;
        t.by += hsz;
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_T_LEFT_SPLIT, node.h[1])) return false;
        t.by -= hsz;
        t.bx += hsz;
        if (!decode_b(t, bl, static_cast<BlockSize>(b[1]), PARTITION_T_LEFT_SPLIT, node.v[1])) return false;
        t.bx -= hsz;
        break;
      case PARTITION_T_RIGHT_SPLIT:
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_T_RIGHT_SPLIT, node.v[0])) return false;
        t.bx += hsz;
        if (!decode_b(t, bl, static_cast<BlockSize>(b[1]), PARTITION_T_RIGHT_SPLIT, node.h[0])) return false;
        t.by += hsz;
        if (!decode_b(t, bl, static_cast<BlockSize>(b[1]), PARTITION_T_RIGHT_SPLIT, 0)) return false;
        t.by -= hsz;
        t.bx -= hsz;
        break;
      case PARTITION_H4:
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_H4, node.h[0])) return false;
        t.by += hsz >> 1;
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_H4, node.h4)) return false;
        t.by += hsz >> 1;
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_H4, EDGE_ALL_LEFT_HAS_BOTTOM)) return false;
        t.by += hsz >> 1;
        if (t.by < f.bh)
          if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_H4, node.h[1])) return false;
        t.by -= hsz * 3 >> 1;
        break;
      case PARTITION_V4:
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_V4, node.v[0])) return false;
        t.bx += hsz >> 1;
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_V4, node.v4)) return false;
        t.bx += hsz >> 1;
        if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_V4, EDGE_ALL_TOP_HAS_RIGHT)) return false;
        t.bx += hsz >> 1;
        if (t.bx < f.bw)
          if (!decode_b(t, bl, static_cast<BlockSize>(b[0]), PARTITION_V4, node.v[1])) return false;
        t.bx -= hsz * 3 >> 1;
        break;
      default:
        return false;
    }
  } else if (have_h_split) {
    const unsigned is_split = ts.msac.decode_bool(gather_top_partition_prob(pc, bl));
    if (bl >= BL_8X8) return false;
    if (is_split) {
      bp = PARTITION_SPLIT;
      const BlockLevel next = static_cast<BlockLevel>(bl + 1);
      if (!decode_sb(t, next, k_intra_edge_tree.split(node, 0))) return false;
      t.bx += hsz;
      if (!decode_sb(t, next, k_intra_edge_tree.split(node, 1))) return false;
      t.bx -= hsz;
    } else {
      bp = PARTITION_H;
      if (!decode_b(t, bl, static_cast<BlockSize>(k_block_sizes[bl][PARTITION_H][0]), PARTITION_H, node.h[0])) return false;
    }
  } else {
    const unsigned is_split = ts.msac.decode_bool(gather_left_partition_prob(pc, bl));
    if (f.layout == PIXEL_LAYOUT_I422 && !is_split) return false;
    if (bl >= BL_8X8) return false;
    if (is_split) {
      bp = PARTITION_SPLIT;
      const BlockLevel next = static_cast<BlockLevel>(bl + 1);
      if (!decode_sb(t, next, k_intra_edge_tree.split(node, 0))) return false;
      t.by += hsz;
      if (!decode_sb(t, next, k_intra_edge_tree.split(node, 2))) return false;
      t.by -= hsz;
    } else {
      bp = PARTITION_V;
      if (!decode_b(t, bl, static_cast<BlockSize>(k_block_sizes[bl][PARTITION_V][0]), PARTITION_V, node.v[0])) return false;
    }
  }

  if (bp != PARTITION_SPLIT || bl == BL_8X8) {
    set_ctx(t.a->partition, bx8, 4 - bl, k_al_part_ctx[0][bl][bp]);
    set_ctx(t.l.partition, by8, 4 - bl, k_al_part_ctx[1][bl][bp]);
  }
  return true;
}

void Core::read_restoration_info(TaskContext& t, RestorationUnit& lr, int p, RestorationType frame_type) {
  TileState& ts = *t.ts;
  Msac& msac = ts.msac;
  const RestorationUnit lr_ref = ts.lr_ref[p];

  if (frame_type == RESTORATION_SWITCHABLE) {
    const int filter = static_cast<int>(msac.symbol_adapt4(ArrayView<uint16_t>(ts.cdf.m.restore_switchable), 2));
    lr.type = static_cast<uint8_t>(filter + !!filter); /* NONE/WIENER/SGRPROJ */
  } else {
    const unsigned type = msac.bool_adapt(frame_type == RESTORATION_WIENER ? ArrayView<uint16_t>(ts.cdf.m.restore_wiener)
                                                                            : ArrayView<uint16_t>(ts.cdf.m.restore_sgrproj));
    lr.type = static_cast<uint8_t>(type ? frame_type : RESTORATION_NONE);
  }

  if (lr.type == RESTORATION_WIENER) {
    lr.filter_v[0] = static_cast<int8_t>(p ? 0 : msac.subexp(lr_ref.filter_v[0] + 5, 16, 1) - 5);
    lr.filter_v[1] = static_cast<int8_t>(msac.subexp(lr_ref.filter_v[1] + 23, 32, 2) - 23);
    lr.filter_v[2] = static_cast<int8_t>(msac.subexp(lr_ref.filter_v[2] + 17, 64, 3) - 17);

    lr.filter_h[0] = static_cast<int8_t>(p ? 0 : msac.subexp(lr_ref.filter_h[0] + 5, 16, 1) - 5);
    lr.filter_h[1] = static_cast<int8_t>(msac.subexp(lr_ref.filter_h[1] + 23, 32, 2) - 23);
    lr.filter_h[2] = static_cast<int8_t>(msac.subexp(lr_ref.filter_h[2] + 17, 64, 3) - 17);
    lr.sgr_weights[0] = lr_ref.sgr_weights[0];
    lr.sgr_weights[1] = lr_ref.sgr_weights[1];
    ts.lr_ref[p] = lr;
  } else if (lr.type == RESTORATION_SGRPROJ) {
    const unsigned idx = msac.bools(4);
    lr.sgr_idx = static_cast<uint8_t>(idx);
    const uint16_t* const sgr_params = k_sgr_params[idx];
    lr.sgr_weights[0] = static_cast<int8_t>(sgr_params[0] ? msac.subexp(lr_ref.sgr_weights[0] + 96, 128, 4) - 96 : 0);
    lr.sgr_weights[1] = static_cast<int8_t>(sgr_params[1] ? msac.subexp(lr_ref.sgr_weights[1] + 32, 128, 4) - 32 : 95);
    for (int i = 0; i < 3; i++) {
      lr.filter_v[i] = lr_ref.filter_v[i];
      lr.filter_h[i] = lr_ref.filter_h[i];
    }
    ts.lr_ref[p] = lr;
  }
}

bool Core::setup_tile(TileState& ts, int tile_row, int tile_col, ConstByteView data) {
  FrameContext& f = f_;
  const FrameHeader& hdr = *f.frame_hdr;
  const int col_sb_start = hdr.tiling.col_start_sb[tile_col];
  const int col_sb_end = hdr.tiling.col_start_sb[tile_col + 1];
  const int row_sb_start = hdr.tiling.row_start_sb[tile_row];
  const int row_sb_end = hdr.tiling.row_start_sb[tile_row + 1];
  const int sb_shift = f.sb_shift;

  ts.cdf = *f.in_cdf;
  ts.last_qidx = hdr.quant.yac;
  for (int8_t& v : ts.last_delta_lf) v = 0;
  ts.dq = &f.dq;
  ts.msac.init(data, hdr.disable_cdf_update);

  ts.tiling.row = tile_row;
  ts.tiling.col = tile_col;
  ts.tiling.col_start = col_sb_start << sb_shift;
  ts.tiling.col_end = imin(col_sb_end << sb_shift, f.bw);
  ts.tiling.row_start = row_sb_start << sb_shift;
  ts.tiling.row_end = imin(row_sb_end << sb_shift, f.bh);
  for (RestorationUnit& u : ts.lr_ref) u = RestorationUnit{};
  return true;
}

bool Core::decode_tile_sbrow(TaskContext& t) {
  FrameContext& f = *t.f;
  const FrameHeader& hdr = *f.frame_hdr;
  const SequenceHeader& seq = *f.seq_hdr;
  const BlockLevel root_bl = seq.sb128 ? BL_128X128 : BL_64X64;
  TileState& ts = *t.ts;
  const int sb_step = f.sb_step;
  const int tile_row = ts.tiling.row, tile_col = ts.tiling.col;
  const int col_sb_start = hdr.tiling.col_start_sb[tile_col];
  const int col_sb128_start = col_sb_start >> !seq.sb128;

  if (is_inter_or_switch(hdr) || hdr.allow_intrabc)
    f.rf.tile_sbrow_init(t.rt, ts.tiling.col_start, ts.tiling.col_end, ts.tiling.row_start, ts.tiling.row_end, t.by >> f.sb_shift);

  reset_context(t.l, is_key_or_intra(hdr));
  ArrayView<uint8_t>(t.pal_sz_uv[1]).fill(0);
  const bool super_res = hdr.width[0] != hdr.width[1];
  const EdgeNode& root = k_intra_edge_tree.root(root_bl);
  for (t.bx = ts.tiling.col_start, t.a = &f.a[static_cast<uint64_t>(col_sb128_start + tile_row * f.sb128w)]; t.bx < ts.tiling.col_end; t.bx += sb_step) {
    // Restoration filter
    for (int p = 0; p < 3; p++) {
      if (!((f.restore_planes >> p) & 1)) continue;
      const int ss_ver = p && f.layout == PIXEL_LAYOUT_I420;
      const int ss_hor = p && f.layout != PIXEL_LAYOUT_I444;
      const LrUnitRange range = lr_unit_read_range(t.by, t.bx, sb_step, sb_step, ss_hor, ss_ver, hdr.restoration.unit_size[p != 0], super_res,
                                                   hdr.super_res.width_scale_denominator, f.lr_rows[p], f.lr_cols[p]);
      for (int ur = range.row_start; ur < range.row_end; ur++)
        for (int uc = range.col_start; uc < range.col_end; uc++)
          read_restoration_info(t, f.lr_units[p][static_cast<uint64_t>(ur) * static_cast<uint64_t>(f.lr_cols[p]) + static_cast<uint64_t>(uc)], p,
                                hdr.restoration.type[p]);
    }
    if (!decode_sb(t, root_bl, root)) return false;
    if ((t.bx & 16) || seq.sb128) t.a++;
  }

  // backup pre-loopfilter pixels for intra prediction of the next sbrow
  backup_ipred_edge(t);

  // error out on symbol decoder overread
  if (ts.msac.cnt() <= -15) return false;
  return true;
}

bool Core::decode_tile(TaskContext& t, int tile_row, int tile_col) {
  FrameContext& f = *t.f;
  const FrameHeader& hdr = *f.frame_hdr;
  TileState& ts = *t.ts;
  const int sbh_end = imin(hdr.tiling.row_start_sb[tile_row + 1], f.sbh);
  for (int sby = hdr.tiling.row_start_sb[tile_row]; sby < sbh_end; sby++) {
    t.by = sby << (4 + f.seq_hdr->sb128);
    const int by_end = (t.by + f.sb_step) >> 1;
    if (hdr.use_ref_frame_mvs) f.rf.load_tmvs(ts.tiling.col_start >> 1, ts.tiling.col_end >> 1, t.by >> 1, by_end);
    if (!decode_tile_sbrow(t)) return false;
    if (is_inter_or_switch(hdr)) f.rf.save_tmvs(t.rt, ts.tiling.col_start >> 1, ts.tiling.col_end >> 1, t.by >> 1, by_end);
  }
  return true;
}

}  // namespace av1
}  // namespace hardware
