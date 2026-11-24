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


// H.265 decoding, part 5: the in-loop filters - deblocking (8.7.2) and sample adaptive offset (8.7.3).
#include "Hardware/VideoCodec/Software/H265Core.hpp"
#include "Hardware/VideoCodec/Software/H265Trace.hpp"

namespace hardware {
namespace h265 {

namespace {
constexpr int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
constexpr int iabs(int a) { return a < 0 ? -a : a; }
template <typename Pel>
inline Pel clip_sample(int v, int max_value) { return static_cast<Pel>(v < 0 ? 0 : v > max_value ? max_value : v); }

// Table 8-12
const uint8_t kTcTable[54] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
                              5, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 20, 22, 24};
const uint8_t kBetaTable[52] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36,
                                38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64};
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Deblocking

namespace {
// The reference pictures of a block's motion, for the boundary strength: a picture is the same when it is the same buffer, whatever list or index names it.
struct BlockMotion {
  const Picture* ref[2];
  int16_t mv[2][2];
  int count;
};
}  // namespace

void Core::deblock_picture() {
  if (cur_->deep) deblock_px<uint16_t>();
  else deblock_px<uint8_t>();
}

// (Pel is the sample type, as in H265Predict.cpp: at 8 bits the depth is the constant 8; tC and beta are scaled by 1 << (BitDepth - 8), 8.7.2.5.3 / 8.7.2.5.5.)
template <typename Pel>
void Core::deblock_px() {
  const int bd_y = sizeof(Pel) == 1 ? 8 : bd_y_, bd_c = sizeof(Pel) == 1 ? 8 : bd_c_;
  const int max_y = (1 << bd_y) - 1, max_c = (1 << bd_c) - 1;
  const Sps& s = *sp_;
  const Pps& pps = *pp_;
  Picture* pic = cur_;
  const int w4 = w4_;
  const int pic_w4 = (s.width + 3) >> 2, pic_h4 = (s.height + 3) >> 2;
  auto ctb_of = [&](int x4, int y4) { return ((y4 << 2) >> s.log2_ctb) * ctb_w_ + ((x4 << 2) >> s.log2_ctb); };
  auto slice_of = [&](int x4, int y4) -> const SliceInfo* {
    const int si = ctb_slice_[ctb_of(x4, y4)];
    return si < 0 ? nullptr : &slices_[static_cast<uint64_t>(si)];
  };
  auto motion_of = [&](int x4, int y4, BlockMotion* m) {
    const MvField& f = mvf_[static_cast<uint64_t>(y4) * w4 + x4];
    const SliceInfo* sl = slice_of(x4, y4);
    m->count = 0;
    for (int l = 0; l < 2; ++l) {
      if (!(f.pred_flags & (1 << l))) continue;
      m->ref[m->count] = sl ? sl->ref_pic[l][f.ref_idx[l]] : nullptr;
      m->mv[m->count][0] = f.mv[l][0];
      m->mv[m->count][1] = f.mv[l][1];
      ++m->count;
    }
  };
  auto strength = [&](int px4, int py4, int qx4, int qy4, bool tu_edge) -> int {
    const uint8_t pm = pred_mode_[static_cast<uint64_t>(py4) * w4 + px4] & 3, qm = pred_mode_[static_cast<uint64_t>(qy4) * w4 + qx4] & 3;
    if (pm == 1 || qm == 1) return 2;
    if (tu_edge && (nz_[static_cast<uint64_t>(py4) * w4 + px4] || nz_[static_cast<uint64_t>(qy4) * w4 + qx4])) return 1;
    BlockMotion p, q;
    motion_of(px4, py4, &p);
    motion_of(qx4, qy4, &q);
    if (p.count != q.count) return 1;
    auto far = [](const int16_t* a, const int16_t* b) { return iabs(a[0] - b[0]) >= 4 || iabs(a[1] - b[1]) >= 4; };
    if (p.count == 1) {
      if (p.ref[0] != q.ref[0]) return 1;
      return far(p.mv[0], q.mv[0]) ? 1 : 0;
    }
    // two motion vectors each
    const bool same_set = (p.ref[0] == q.ref[0] && p.ref[1] == q.ref[1]) || (p.ref[0] == q.ref[1] && p.ref[1] == q.ref[0]);
    if (!same_set) return 1;
    if (p.ref[0] != p.ref[1]) {
      if (p.ref[0] == q.ref[0]) return (far(p.mv[0], q.mv[0]) || far(p.mv[1], q.mv[1])) ? 1 : 0;
      return (far(p.mv[0], q.mv[1]) || far(p.mv[1], q.mv[0])) ? 1 : 0;
    }
    // both motion vectors refer to the same picture
    const bool straight = far(p.mv[0], q.mv[0]) || far(p.mv[1], q.mv[1]);
    const bool crossed = far(p.mv[0], q.mv[1]) || far(p.mv[1], q.mv[0]);
    return (straight && crossed) ? 1 : 0;
  };
  // Is the edge between blocks p and q one that is filtered (slice, tile and picture rules)? The slice of q (the block the edge belongs to) decides.
  auto edge_filtered = [&](int px4, int py4, int qx4, int qy4) -> bool {
    const SliceInfo* qs = slice_of(qx4, qy4);
    const SliceInfo* ps = slice_of(px4, py4);
    if (!qs || !ps || qs->deblocking_disabled) return false;
    if (qs->slice_addr_rs != ps->slice_addr_rs && !qs->loop_filter_across_slices) return false;
    if (!pps.loop_filter_across_tiles && tile_id_ts_[rs_to_ts_[ctb_of(px4, py4)]] != tile_id_ts_[rs_to_ts_[ctb_of(qx4, qy4)]]) return false;
    return true;
  };

  // bS of every 4-sample edge segment, vertical edges first (8.7.2 filters all vertical edges of the picture, then all horizontal edges)
  static const int kChromaQp[14] = {29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37};
  // QpC of the edge (8.7.2.5.5): Table 8-10 of qPi = ((QpQ + QpP + 1) >> 1) + cQpPicOffset for 4:2:0, Min(qPi, 51) for the other chroma formats
  auto chroma_tc = [&](int qp_avg, int c, int bs, const SliceInfo* sl) {
    const int offset = c == 1 ? pps.cb_qp_offset : pps.cr_qp_offset;
    const int qpi = qp_avg + offset;
    const int qpc = cat_ != 1 ? (qpi < 51 ? qpi : 51) : qpi < 30 ? qpi : qpi >= 43 ? qpi - 6 : kChromaQp[qpi - 30];
    return kTcTable[clip3(0, 53, qpc + 2 * (bs - 1) + (sl->tc_offset_div2 << 1))] * (1 << (bd_c - 8));
  };
  for (int dir = 0; dir < 2; ++dir) {   // 0: vertical edges (filtering across x), 1: horizontal edges
    // luma
    for (int y4 = 0; y4 < pic_h4; ++y4) {
      for (int x4 = 0; x4 < pic_w4; ++x4) {
        if (dir == 0 ? (x4 == 0 || (x4 & 1)) : (y4 == 0 || (y4 & 1))) continue;
        const uint8_t e = edge_[static_cast<uint64_t>(y4) * w4 + x4];
        const bool tu = dir == 0 ? (e & 1) : (e & 4), pu = dir == 0 ? (e & 2) : (e & 8);
        if (!tu && !pu) continue;
        const int px4 = dir == 0 ? x4 - 1 : x4, py4 = dir == 0 ? y4 : y4 - 1;
        if (!edge_filtered(px4, py4, x4, y4)) continue;
        const int bs = strength(px4, py4, x4, y4, tu);
        if (bs == 0) continue;
        const SliceInfo* qs = slice_of(x4, y4);
        const int qp = (qp_map_[static_cast<uint64_t>(py4) * w4 + px4] + qp_map_[static_cast<uint64_t>(y4) * w4 + x4] + 1) >> 1;
        const int beta = kBetaTable[clip3(0, 51, qp + (qs->beta_offset_div2 << 1))] * (1 << (bd_y - 8));
        const int tc = kTcTable[clip3(0, 53, qp + 2 * (bs - 1) + (qs->tc_offset_div2 << 1))] * (1 << (bd_y - 8));
        const bool no_p = pred_mode_[static_cast<uint64_t>(py4) * w4 + px4] & 8, no_q = pred_mode_[static_cast<uint64_t>(y4) * w4 + x4] & 8;
        // the four lines across the edge
        const int luma_stride = pic->stride[0] / static_cast<int>(sizeof(Pel));
        Pel* base = samples<Pel>(pic->plane[0]) + static_cast<uint64_t>(y4 << 2) * luma_stride + (x4 << 2);
        const int along = dir == 0 ? luma_stride : 1;    // step from one line to the next
        const int across = dir == 0 ? 1 : luma_stride;   // step across the edge
        auto P = [&](int line, int i) -> Pel& { return base[line * along - (i + 1) * across]; };
        auto Q = [&](int line, int i) -> Pel& { return base[line * along + i * across]; };
        const int dp0 = iabs(P(0, 2) - 2 * P(0, 1) + P(0, 0)), dp3 = iabs(P(3, 2) - 2 * P(3, 1) + P(3, 0));
        const int dq0 = iabs(Q(0, 2) - 2 * Q(0, 1) + Q(0, 0)), dq3 = iabs(Q(3, 2) - 2 * Q(3, 1) + Q(3, 0));
        const int dpq0 = dp0 + dq0, dpq3 = dp3 + dq3, dp = dp0 + dp3, dq = dq0 + dq3, d = dpq0 + dpq3;
        if (d >= beta) continue;
        auto strong = [&](int line, int dpq) {
          return 2 * dpq < (beta >> 2) && iabs(P(line, 3) - P(line, 0)) + iabs(Q(line, 0) - Q(line, 3)) < (beta >> 3) && iabs(P(line, 0) - Q(line, 0)) < ((5 * tc + 1) >> 1);
        };
        const bool strong_filter = strong(0, dpq0) && strong(3, dpq3);
        const bool de_p = dp < ((beta + (beta >> 1)) >> 3), de_q = dq < ((beta + (beta >> 1)) >> 3);
        for (int line = 0; line < 4; ++line) {
          const int p0 = P(line, 0), p1 = P(line, 1), p2 = P(line, 2), p3 = P(line, 3);
          const int q0 = Q(line, 0), q1 = Q(line, 1), q2 = Q(line, 2), q3 = Q(line, 3);
          if (strong_filter) {
            if (!no_p) {
              P(line, 0) = static_cast<Pel>(clip3(p0 - 2 * tc, p0 + 2 * tc, (p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3));
              P(line, 1) = static_cast<Pel>(clip3(p1 - 2 * tc, p1 + 2 * tc, (p2 + p1 + p0 + q0 + 2) >> 2));
              P(line, 2) = static_cast<Pel>(clip3(p2 - 2 * tc, p2 + 2 * tc, (2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3));
            }
            if (!no_q) {
              Q(line, 0) = static_cast<Pel>(clip3(q0 - 2 * tc, q0 + 2 * tc, (p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3));
              Q(line, 1) = static_cast<Pel>(clip3(q1 - 2 * tc, q1 + 2 * tc, (p0 + q0 + q1 + q2 + 2) >> 2));
              Q(line, 2) = static_cast<Pel>(clip3(q2 - 2 * tc, q2 + 2 * tc, (p0 + q0 + q1 + 3 * q2 + 2 * q3 + 4) >> 3));
            }
          } else {
            int delta = (9 * (q0 - p0) - 3 * (q1 - p1) + 8) >> 4;
            if (iabs(delta) >= tc * 10) continue;
            delta = clip3(-tc, tc, delta);
            if (!no_p) {
              P(line, 0) = clip_sample<Pel>(p0 + delta, max_y);
              if (de_p) P(line, 1) = clip_sample<Pel>(p1 + clip3(-(tc >> 1), tc >> 1, (((p2 + p0 + 1) >> 1) - p1 + delta) >> 1), max_y);
            }
            if (!no_q) {
              Q(line, 0) = clip_sample<Pel>(q0 - delta, max_y);
              if (de_q) Q(line, 1) = clip_sample<Pel>(q1 + clip3(-(tc >> 1), tc >> 1, (((q2 + q0 + 1) >> 1) - q1 - delta) >> 1), max_y);
            }
          }
        }
      }
    }
    // chroma: edges on the 8-sample chroma grid (16 luma samples across a subsampled direction, 8 across a full one), filtered where bS is 2, in segments of four
    // chroma samples that take bS and the quantiser from the luma segment at their first sample
    const int cw = s.width >> sub_w_, ch = s.height >> sub_h_;
    for (int c = 1; c < 3 && cat_ != 0; ++c) {
      Pel* plane = samples<Pel>(pic->plane[c]);
      const int stride = pic->stride[c] / static_cast<int>(sizeof(Pel));
      for (int yc = 0; yc < ch; yc += (dir == 0 ? 4 : 8)) {
        for (int xc = 0; xc < cw; xc += (dir == 0 ? 8 : 4)) {
          if (dir == 0 ? xc == 0 : yc == 0) continue;
          const int xl = xc << sub_w_, yl = yc << sub_h_;   // luma position of the segment's first sample on the edge
          const int x4 = xl >> 2, y4 = yl >> 2;
          const uint8_t e = edge_[static_cast<uint64_t>(y4) * w4 + x4];
          const bool tu = dir == 0 ? (e & 1) : (e & 4), pu = dir == 0 ? (e & 2) : (e & 8);
          if (!tu && !pu) continue;
          const int px4 = dir == 0 ? x4 - 1 : x4, py4 = dir == 0 ? y4 : y4 - 1;
          if (!edge_filtered(px4, py4, x4, y4)) continue;
          if (strength(px4, py4, x4, y4, tu) != 2) continue;
          const SliceInfo* qs = slice_of(x4, y4);
          const int qp = (qp_map_[static_cast<uint64_t>(py4) * w4 + px4] + qp_map_[static_cast<uint64_t>(y4) * w4 + x4] + 1) >> 1;
          const int tc = chroma_tc(qp, c, 2, qs);
          const bool no_p = pred_mode_[static_cast<uint64_t>(py4) * w4 + px4] & 8, no_q = pred_mode_[static_cast<uint64_t>(y4) * w4 + x4] & 8;
          for (int k = 0; k < 4; ++k) {
            Pel* base = plane + static_cast<uint64_t>(yc + (dir == 0 ? k : 0)) * stride + xc + (dir == 0 ? 0 : k);
            const int across = dir == 0 ? 1 : stride;
            const int p0 = base[-across], p1 = base[-2 * across], q0 = base[0], q1 = base[across];
            const int delta = clip3(-tc, tc, ((((q0 - p0) << 2) + p1 - q1 + 4) >> 3));
            if (!no_p) base[-across] = clip_sample<Pel>(p0 + delta, max_c);
            if (!no_q) base[0] = clip_sample<Pel>(q0 - delta, max_c);
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Sample adaptive offset

void Core::sao_picture() {
  if (!sp_->sao) return;
  if (cur_->deep) sao_px<uint16_t>();
  else sao_px<uint8_t>();
}

// (8.7.3: the offsets come scaled to the bit depth from read_sao(); the bands are the top five bits of a sample, bandShift = BitDepth - 5.)
template <typename Pel>
void Core::sao_px() {
  const Sps& s = *sp_;
  Picture* pic = cur_;
  bool any = false;
  for (int rs = 0; rs < ctb_w_ * ctb_h_ && !any; ++rs) {
    const int si = ctb_slice_[rs];
    if (si >= 0 && (slices_[static_cast<uint64_t>(si)].sao_luma || slices_[static_cast<uint64_t>(si)].sao_chroma)) any = true;
  }
  if (!any) return;
  // the filter reads the deblocked samples, not the ones it has changed
  for (int c = 0; c < 3; ++c)
    for (uint64_t i = 0; i < pic->plane[c].size(); ++i) sao_plane_[c][i] = pic->plane[c][i];
  const Pps& pps = *pp_;
  for (int ry = 0; ry < ctb_h_; ++ry) {
    for (int rx = 0; rx < ctb_w_; ++rx) {
      const int rs = ry * ctb_w_ + rx;
      const int si = ctb_slice_[rs];
      if (si < 0) continue;
      const SliceInfo& info = slices_[static_cast<uint64_t>(si)];
      const SaoParams& p = sao_[rs];
      for (int c = 0; c < (cat_ != 0 ? 3 : 1); ++c) {
        if (!(c == 0 ? info.sao_luma : info.sao_chroma) || p.type[c] == 0) continue;
        const int sx = c ? sub_w_ : 0, sy = c ? sub_h_ : 0;
        const int pw = s.width >> sx, ph = s.height >> sy;
        const int ctb_w = s.ctb_size >> sx, ctb_h = s.ctb_size >> sy;
        const int x_begin = rx * ctb_w, y_begin = ry * ctb_h;
        const int x_end = x_begin + ctb_w < pw ? x_begin + ctb_w : pw, y_end = y_begin + ctb_h < ph ? y_begin + ctb_h : ph;
        const int bd = sizeof(Pel) == 1 ? 8 : (c ? bd_c_ : bd_y_);
        const int max_value = (1 << bd) - 1, band_shift = bd - 5;
        const Pel* src = samples<Pel>(sao_plane_[c]);
        Pel* dst = samples<Pel>(pic->plane[c]);
        const int stride = pic->stride[c] / static_cast<int>(sizeof(Pel));
        int offset_val[5] = {0, p.offset[c][0], p.offset[c][1], p.offset[c][2], p.offset[c][3]};
        uint8_t band_table[32] = {};
        if (p.type[c] == 1)
          for (int k = 0; k < 4; ++k) band_table[(k + p.band_position[c]) & 31] = static_cast<uint8_t>(k + 1);
        static const int kHPos[4][2] = {{-1, 1}, {0, 0}, {-1, 1}, {1, -1}};
        static const int kVPos[4][2] = {{0, 0}, {-1, 1}, {-1, 1}, {-1, 1}};
        for (int y = y_begin; y < y_end; ++y) {
          for (int x = x_begin; x < x_end; ++x) {
            const int l4x = (x << sx) >> 2, l4y = (y << sy) >> 2;
            if (pred_mode_[static_cast<uint64_t>(l4y) * w4_ + l4x] & 8) continue;   // pcm / lossless samples are left alone
            const int v = src[static_cast<uint64_t>(y) * stride + x];
            int idx;
            if (p.type[c] == 1) {
              idx = band_table[v >> band_shift];
            } else {
              int edge_idx = 2;
              bool usable = true;
              for (int k = 0; k < 2 && usable; ++k) {
                const int xn = x + kHPos[p.eo_class[c]][k], yn = y + kVPos[p.eo_class[c]][k];
                if (xn < 0 || yn < 0 || xn >= pw || yn >= ph) { usable = false; break; }
                const int nl4x = (xn << sx) >> 2, nl4y = (yn << sy) >> 2;
                const int nctb = ((yn << sy) >> s.log2_ctb) * ctb_w_ + ((xn << sx) >> s.log2_ctb);
                if (nctb != rs) {
                  const int nsi = ctb_slice_[nctb];
                  if (nsi < 0) { usable = false; break; }
                  const SliceInfo& ni = slices_[static_cast<uint64_t>(nsi)];
                  if (ni.slice_addr_rs != info.slice_addr_rs) {
                    const bool neighbour_first = zscan_[static_cast<uint64_t>(nl4y) * w4_ + nl4x] < zscan_[static_cast<uint64_t>(l4y) * w4_ + l4x];
                    if (neighbour_first ? !info.loop_filter_across_slices : !ni.loop_filter_across_slices) { usable = false; break; }
                  }
                  if (!pps.loop_filter_across_tiles && tile_id_ts_[rs_to_ts_[nctb]] != tile_id_ts_[rs_to_ts_[rs]]) { usable = false; break; }
                }
                const int nv = src[static_cast<uint64_t>(yn) * stride + xn];
                edge_idx += (v > nv) - (v < nv);
              }
              if (!usable) continue;
              // edgeIdx 0, 1, 2 map to 1, 2, 0; 3 and 4 stay
              idx = edge_idx == 2 ? 0 : (edge_idx < 2 ? edge_idx + 1 : edge_idx);
            }
            dst[static_cast<uint64_t>(y) * stride + x] = clip_sample<Pel>(v + offset_val[idx], max_value);
          }
        }
      }
    }
  }
}

}  // namespace h265
}  // namespace hardware
