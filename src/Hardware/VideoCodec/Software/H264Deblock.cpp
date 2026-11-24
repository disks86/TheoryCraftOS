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

// H.264 decoding, part 5: the deblocking filter (8.7) of frames, field pictures and MBAFF frames.

#include "Hardware/VideoCodec/Software/H264Core.hpp"

namespace hardware {
namespace h264 {

#include "Hardware/VideoCodec/Software/H264Tables.inc"

namespace {

inline int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
inline int iabs(int v) { return v < 0 ? -v : v; }
inline uint8_t clip_pixel(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

// Filters `count` sample lines across one edge. `q` points at q0 of the first line; `across` steps from q0 to q1
// (and from p0 to q0 backwards), `along` steps to the next line.
void filter_edge(uint8_t* q, int across, int along, int count, const int* bs, int index_a, int alpha, int beta, bool chroma, int lines_per_bs) {
  for (int i = 0; i < count; ++i) {
    const int s = bs[i / lines_per_bs];
    if (s == 0) continue;
    uint8_t* pq = q + i * along;
    const int p0 = pq[-across], p1 = pq[-2 * across], q0 = pq[0], q1 = pq[across];
    if (!(iabs(p0 - q0) < alpha && iabs(p1 - p0) < beta && iabs(q1 - q0) < beta)) continue;
    if (s < 4) {
      const int tc0 = kTc0[index_a * 4 + s];
      if (chroma) {
        const int tc = tc0 + 1;
        const int delta = clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
        pq[-across] = clip_pixel(p0 + delta);
        pq[0] = clip_pixel(q0 - delta);
      } else {
        const int p2 = pq[-3 * across], q2 = pq[2 * across];
        const int ap = iabs(p2 - p0), aq = iabs(q2 - q0);
        const int tc = tc0 + (ap < beta) + (aq < beta);
        const int delta = clip3(-tc, tc, (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
        pq[-across] = clip_pixel(p0 + delta);
        pq[0] = clip_pixel(q0 - delta);
        if (ap < beta) pq[-2 * across] = static_cast<uint8_t>(p1 + clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1));
        if (aq < beta) pq[across] = static_cast<uint8_t>(q1 + clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1));
      }
    } else if (chroma) {
      pq[-across] = static_cast<uint8_t>((2 * p1 + p0 + q1 + 2) >> 2);
      pq[0] = static_cast<uint8_t>((2 * q1 + q0 + p1 + 2) >> 2);
    } else {
      const int p2 = pq[-3 * across], q2 = pq[2 * across];
      const int ap = iabs(p2 - p0), aq = iabs(q2 - q0);
      const bool small = iabs(p0 - q0) < ((alpha >> 2) + 2);
      if (ap < beta && small) {
        const int p3 = pq[-4 * across];
        pq[-across] = static_cast<uint8_t>((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
        pq[-2 * across] = static_cast<uint8_t>((p2 + p1 + p0 + q0 + 2) >> 2);
        pq[-3 * across] = static_cast<uint8_t>((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
      } else {
        pq[-across] = static_cast<uint8_t>((2 * p1 + p0 + q1 + 2) >> 2);
      }
      if (aq < beta && small) {
        const int q3 = pq[3 * across];
        pq[0] = static_cast<uint8_t>((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
        pq[across] = static_cast<uint8_t>((p0 + q0 + q1 + q2 + 2) >> 2);
        pq[2 * across] = static_cast<uint8_t>((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
      } else {
        pq[0] = static_cast<uint8_t>((2 * q1 + q0 + p1 + 2) >> 2);
      }
    }
  }
}

}  // namespace

void Core::deblock_picture() {
  if (!cur_) return;
  if (mbaff_) {
    // (in order of increasing macroblock addresses: the pairs, top macroblock first)
    for (int addr = 0; addr < pic_mbs_; ++addr)
      if (mbs_[static_cast<uint64_t>(addr)].slice >= 0) deblock_mb_mbaff(addr);
    return;
  }
  for (int y = 0; y < pic_mb_h_; ++y)
    for (int x = 0; x < mb_w_; ++x) {
      if (mbs_[static_cast<uint64_t>(y) * mb_w_ + x].slice < 0) continue;
      deblock_mb(x, y);
    }
}

// 8.7.2.1: the strength of the edge between 4x4 block blk_p of macroblock mb_p and blk_q of mb_q (macroblock addresses of the current picture).
int Core::boundary_strength(int mb_p, int mb_q, int blk_p, int blk_q, bool mb_edge, bool vertical) const {
  const MbCtx& cp = mbs_[static_cast<uint64_t>(mb_p)];
  const MbCtx& cq = mbs_[static_cast<uint64_t>(mb_q)];
  // mixedModeEdgeFlag: a frame macroblock pair beside a field macroblock pair.
  const bool mixed = mbaff_ && cp.field != cq.field;
  if (cp.intra || cq.intra) {
    if (mb_edge && ((!cp.field && !cq.field) || ((mbaff_ || field_pic_) && vertical))) return 4;
    return 3;
  }
  if (((cp.nzmask >> blk_p) & 1) || ((cq.nzmask >> blk_q) & 1)) return 2;
  if (mixed) return 1;
  const MotionMb& mp = motion_of(mb_p);
  const MotionMb& mq = motion_of(mb_q);
  const int b8p = ((blk_p >> 3) << 1) | ((blk_p & 3) >> 1), b8q = ((blk_q >> 3) << 1) | ((blk_q & 3) >> 1);
  int rp[2], rq[2];
  for (int l = 0; l < 2; ++l) {
    rp[l] = mp.ref[l][b8p] >= 0 ? mp.refid[l][b8p] : -1;
    rq[l] = mq.ref[l][b8q] >= 0 ? mq.refid[l][b8q] : -1;
  }
  const int np = (rp[0] >= 0) + (rp[1] >= 0), nq = (rq[0] >= 0) + (rq[1] >= 0);
  if (np != nq) return 1;
  // A vertical difference of 4 quarter frame samples is one of 2 quarter field samples (NOTE 3).
  const int vlimit = cq.field ? 2 : 4;
  auto far = [vlimit](const int16_t* a, const int16_t* b) { return iabs(a[0] - b[0]) >= 4 || iabs(a[1] - b[1]) >= vlimit; };
  const int16_t* vp[2] = {mp.mv[0][blk_p], mp.mv[1][blk_p]};
  const int16_t* vq[2] = {mq.mv[0][blk_q], mq.mv[1][blk_q]};
  if (np == 1) {
    const int lp = rp[0] >= 0 ? 0 : 1, lq = rq[0] >= 0 ? 0 : 1;
    if (rp[lp] != rq[lq]) return 1;
    return far(vp[lp], vq[lq]) ? 1 : 0;
  }
  if (np == 0) return 0;
  // two vectors each
  if (!((rp[0] == rq[0] && rp[1] == rq[1]) || (rp[0] == rq[1] && rp[1] == rq[0]))) return 1;
  if (rp[0] != rp[1]) {
    if (rp[0] == rq[0]) return (far(vp[0], vq[0]) || far(vp[1], vq[1])) ? 1 : 0;
    return (far(vp[0], vq[1]) || far(vp[1], vq[0])) ? 1 : 0;
  }
  return ((far(vp[0], vq[0]) || far(vp[1], vq[1])) && (far(vp[0], vq[1]) || far(vp[1], vq[0]))) ? 1 : 0;
}

// A frame or a field picture: every macroblock's neighbours are those of its own picture.
void Core::deblock_mb(int mbx, int mby) {
  const int addr = mby * mb_w_ + mbx;
  const MbCtx& c = mbs_[static_cast<uint64_t>(addr)];
  if (c.dbf_idc == 1) return;
  const bool left_ok = mbx > 0 && mbs_[static_cast<uint64_t>(addr - 1)].slice >= 0 &&
                       !(c.dbf_idc == 2 && mbs_[static_cast<uint64_t>(addr - 1)].slice != c.slice);
  const bool top_ok = mby > 0 && mbs_[static_cast<uint64_t>(addr - mb_w_)].slice >= 0 &&
                      !(c.dbf_idc == 2 && mbs_[static_cast<uint64_t>(addr - mb_w_)].slice != c.slice);
  // Strengths: bs[dir][edge][segment]; dir 0 = vertical edges (left to right), 1 = horizontal edges.
  int bs[2][4][4];
  for (int dir = 0; dir < 2; ++dir) {
    for (int e = 0; e < 4; ++e) {
      for (int s = 0; s < 4; ++s) {
        bs[dir][e][s] = 0;
        if (e == 0 && !(dir == 0 ? left_ok : top_ok)) continue;
        if ((e & 1) && c.t8x8) continue;
        int mb_p, blk_p, blk_q;
        if (dir == 0) {
          blk_q = s * 4 + e;
          if (e == 0) {
            mb_p = addr - 1;
            blk_p = s * 4 + 3;
          } else {
            mb_p = addr;
            blk_p = s * 4 + e - 1;
          }
        } else {
          blk_q = e * 4 + s;
          if (e == 0) {
            mb_p = addr - mb_w_;
            blk_p = 12 + s;
          } else {
            mb_p = addr;
            blk_p = (e - 1) * 4 + s;
          }
        }
        bs[dir][e][s] = boundary_strength(mb_p, addr, blk_p, blk_q, e == 0, dir == 0);
      }
    }
  }
  uint8_t* ly = pic_y_ + static_cast<int64_t>(mby * 16) * pic_ls_ + mbx * 16;
  const int ls = pic_ls_;
  for (int dir = 0; dir < 2; ++dir) {
    for (int e = 0; e < 4; ++e) {
      if (e == 0 && !(dir == 0 ? left_ok : top_ok)) continue;
      if ((e & 1) && c.t8x8) continue;
      const MbCtx& p = e == 0 ? mbs_[static_cast<uint64_t>(dir == 0 ? addr - 1 : addr - mb_w_)] : c;
      const int qpav = (p.qp + c.qp + 1) >> 1;
      const int index_a = clip3(0, 51, qpav + c.alpha_off), index_b = clip3(0, 51, qpav + c.beta_off);
      const int alpha = kAlpha[index_a], beta = kBeta[index_b];
      if (alpha == 0 || beta == 0) continue;
      uint8_t* q = dir == 0 ? ly + e * 4 : ly + e * 4 * ls;
      filter_edge(q, dir == 0 ? 1 : ls, dir == 0 ? ls : 1, 16, bs[dir][e], index_a, alpha, beta, false, 4);
    }
  }
  if (mono_) return;
  const int cs = pic_cs_;
  for (int plane = 0; plane < 2; ++plane) {
    uint8_t* base = (plane == 0 ? pic_u_ : pic_v_) + static_cast<int64_t>(mby * 8) * cs + mbx * 8;
    for (int dir = 0; dir < 2; ++dir) {
      for (int e = 0; e < 2; ++e) {  // chroma edges 0 and 4 (luma edges 0 and 2)
        if (e == 0 && !(dir == 0 ? left_ok : top_ok)) continue;
        const MbCtx& p = e == 0 ? mbs_[static_cast<uint64_t>(dir == 0 ? addr - 1 : addr - mb_w_)] : c;
        const int qpav = (p.qpc[plane] + c.qpc[plane] + 1) >> 1;
        const int index_a = clip3(0, 51, qpav + c.alpha_off), index_b = clip3(0, 51, qpav + c.beta_off);
        const int alpha = kAlpha[index_a], beta = kBeta[index_b];
        if (alpha == 0 || beta == 0) continue;
        uint8_t* q = dir == 0 ? base + e * 4 : base + e * 4 * cs;
        filter_edge(q, dir == 0 ? 1 : cs, dir == 0 ? cs : 1, 8, bs[dir][e * 2], index_a, alpha, beta, true, 2);
      }
    }
  }
}

// A macroblock of an MBAFF frame (8.7, MbaffFrameFlag 1). Its lines are every other line of its pair when it is a field macroblock; its left edge may meet a
// pair of the other kind, line by line from either of that pair's macroblocks; the top edge of a frame macroblock under a field pair is filtered as two field
// edges, and the top edge of a field macroblock reaches into the pair above by lines of its parity.
void Core::deblock_mb_mbaff(int addr) {
  const MbCtx& c = mbs_[static_cast<uint64_t>(addr)];
  if (c.dbf_idc == 1) return;
  const int pair = addr >> 1, px = pair % mb_w_, prow = pair / mb_w_;
  const bool field = c.field != 0, bottom = (addr & 1) != 0;
  const int dy = field ? 2 : 1;
  const int yi = 32 * prow + (field ? (bottom ? 1 : 0) : (bottom ? 16 : 0));   // 6.4.1
  const int yic = (yi + 1) / 2;
  const int xi = 16 * px;
  const int ls = pic_ls_, cs = pic_cs_;
  // The macroblock of the pair in column px of pair row (row / span) holding picture line `row` (luma, or chroma with span 16), and that line inside it.
  auto mb_holding = [&](int row, bool chroma, int* row_in) {
    const int span = chroma ? 16 : 32;
    const int top = 2 * ((row / span) * mb_w_ + px);
    const int r = row % span;
    if (mbs_[static_cast<uint64_t>(top)].field) {
      *row_in = r >> 1;
      return top + (r & 1);
    }
    *row_in = r % (span / 2);
    return top + (r >= span / 2 ? 1 : 0);
  };
  auto usable = [&](int m) {
    const MbCtx& n = mbs_[static_cast<uint64_t>(m)];
    return n.slice >= 0 && !(c.dbf_idc == 2 && n.slice != c.slice);
  };
  auto thresholds = [&](int qp_p, int qp_q, int* index_a, int* alpha, int* beta) {
    const int qpav = (qp_p + qp_q + 1) >> 1;
    *index_a = clip3(0, 51, qpav + c.alpha_off);
    *alpha = kAlpha[*index_a];
    *beta = kBeta[clip3(0, 51, qpav + c.beta_off)];
    return *alpha != 0 && *beta != 0;
  };
  int index_a, alpha, beta;

  // --- the left edge, line by line
  // (the pair to the left is in one slice)
  const bool left = px > 0 && usable(2 * (pair - 1));
  int left_bs[16] = {}, left_mb[16] = {};
  if (left) {
    for (int k = 0; k < 16; ++k) {
      int row;
      const int r = yi + dy * k;
      // (the pair to the left, in column px - 1)
      const int span_top = 2 * ((r / 32) * mb_w_ + px - 1);
      int m;
      const int rr = r % 32;
      if (mbs_[static_cast<uint64_t>(span_top)].field) {
        m = span_top + (rr & 1);
        row = rr >> 1;
      } else {
        m = span_top + (rr >= 16 ? 1 : 0);
        row = rr % 16;
      }
      left_mb[k] = m;
      left_bs[k] = boundary_strength(m, addr, (row >> 2) * 4 + 3, (k >> 2) * 4, true, true);
    }
    for (int k = 0; k < 16; ++k) {
      if (!left_bs[k] || !thresholds(mbs_[static_cast<uint64_t>(left_mb[k])].qp, c.qp, &index_a, &alpha, &beta)) continue;
      filter_edge(pic_y_ + static_cast<int64_t>(yi + dy * k) * ls + xi, 1, 0, 1, &left_bs[k], index_a, alpha, beta, false, 1);
    }
  }
  // --- the inner vertical edges, on the macroblock's own lines
  uint8_t* ly = pic_y_ + static_cast<int64_t>(yi) * ls + xi;
  const int mls = dy * ls;
  int inner_bs[2][4][4] = {};
  for (int e = 1; e < 4; ++e) {
    if ((e & 1) && c.t8x8) continue;
    for (int s = 0; s < 4; ++s) {
      inner_bs[0][e][s] = boundary_strength(addr, addr, s * 4 + e - 1, s * 4 + e, false, true);
      inner_bs[1][e][s] = boundary_strength(addr, addr, (e - 1) * 4 + s, e * 4 + s, false, false);
    }
  }
  if (thresholds(c.qp, c.qp, &index_a, &alpha, &beta)) {
    for (int e = 1; e < 4; ++e) {
      if ((e & 1) && c.t8x8) continue;
      filter_edge(ly + e * 4, 1, mls, 16, inner_bs[0][e], index_a, alpha, beta, false, 4);
    }
  }
  // --- the top edge
  bool top = prow > 0 || (!field && bottom);
  const bool split = !field && !bottom && prow > 0 && mbs_[static_cast<uint64_t>(2 * (pair - mb_w_) + 1)].field;
  int top_mb[2] = {0, 0};
  int top_bs[2][4] = {};
  if (top) {
    if (split) {
      // A frame macroblock under a field pair: field mode filtering of its top field lines with the top field macroblock above, then of its bottom field lines.
      for (int f = 0; f < 2; ++f) {
        top_mb[f] = 2 * (pair - mb_w_) + f;
        for (int s = 0; s < 4; ++s) top_bs[f][s] = boundary_strength(top_mb[f], addr, 12 + s, s, true, false);
      }
      top = usable(top_mb[0]);
    } else {
      int row;
      top_mb[0] = mb_holding(yi - dy, false, &row);
      for (int s = 0; s < 4; ++s) top_bs[0][s] = boundary_strength(top_mb[0], addr, (row >> 2) * 4 + s, s, true, false);
      top = usable(top_mb[0]);
    }
  }
  if (top) {
    for (int f = 0; f < (split ? 2 : 1); ++f) {
      if (!thresholds(mbs_[static_cast<uint64_t>(top_mb[f])].qp, c.qp, &index_a, &alpha, &beta)) continue;
      if (split) filter_edge(pic_y_ + static_cast<int64_t>(yi + f) * ls + xi, 2 * ls, 1, 16, top_bs[f], index_a, alpha, beta, false, 4);
      else filter_edge(ly, mls, 1, 16, top_bs[0], index_a, alpha, beta, false, 4);
    }
  }
  // --- the inner horizontal edges
  if (thresholds(c.qp, c.qp, &index_a, &alpha, &beta)) {
    for (int e = 1; e < 4; ++e) {
      if ((e & 1) && c.t8x8) continue;
      filter_edge(ly + e * 4 * mls, mls, 1, 16, inner_bs[1][e], index_a, alpha, beta, false, 4);
    }
  }
  if (mono_) return;
  // --- chroma: the same edges; a chroma line takes the strength of the luma line of its field (8.7.2)
  for (int plane = 0; plane < 2; ++plane) {
    uint8_t* const pl = plane == 0 ? pic_u_ : pic_v_;
    uint8_t* cb = pl + static_cast<int64_t>(yic) * cs + xi / 2;
    const int mcs = dy * cs;
    if (left) {
      for (int k = 0; k < 8; ++k) {
        int row;
        const int rc = yic + dy * k;
        const int top_c = 2 * ((rc / 16) * mb_w_ + px - 1);
        const int r = rc % 16;
        int m;
        if (mbs_[static_cast<uint64_t>(top_c)].field) {
          m = top_c + (r & 1);
          row = r >> 1;
        } else {
          m = top_c + (r >= 8 ? 1 : 0);
          row = r % 8;
        }
        (void)row;
        const int kl = field ? 2 * k : 4 * (k >> 1) + (k & 1);
        int b = left_bs[kl];
        if (!b || !thresholds(mbs_[static_cast<uint64_t>(m)].qpc[plane], c.qpc[plane], &index_a, &alpha, &beta)) continue;
        filter_edge(pl + static_cast<int64_t>(rc) * cs + xi / 2, 1, 0, 1, &b, index_a, alpha, beta, true, 1);
      }
    }
    if (thresholds(c.qpc[plane], c.qpc[plane], &index_a, &alpha, &beta)) filter_edge(cb + 4, 1, mcs, 8, inner_bs[0][2], index_a, alpha, beta, true, 2);
    if (top) {
      for (int f = 0; f < (split ? 2 : 1); ++f) {
        if (!thresholds(mbs_[static_cast<uint64_t>(top_mb[f])].qpc[plane], c.qpc[plane], &index_a, &alpha, &beta)) continue;
        if (split) filter_edge(pl + static_cast<int64_t>(yic + f) * cs + xi / 2, 2 * cs, 1, 8, top_bs[f], index_a, alpha, beta, true, 2);
        else filter_edge(cb, mcs, 1, 8, top_bs[0], index_a, alpha, beta, true, 2);
      }
    }
    if (thresholds(c.qpc[plane], c.qpc[plane], &index_a, &alpha, &beta)) filter_edge(cb + 4 * mcs, mcs, 1, 8, inner_bs[1][2], index_a, alpha, beta, true, 2);
  }
}

}  // namespace h264
}  // namespace hardware
