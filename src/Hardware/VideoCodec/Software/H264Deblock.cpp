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

// H.264 decoding, part 5: the deblocking filter (8.7) for progressive frames.

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
  for (int y = 0; y < mb_h_; ++y)
    for (int x = 0; x < mb_w_; ++x) {
      if (mbs_[static_cast<uint64_t>(y) * mb_w_ + x].slice < 0) continue;
      deblock_mb(x, y);
    }
}

// 8.7.2.1: the strength of the edge between 4x4 block blk_p of macroblock mb_p and blk_q of mb_q.
int Core::boundary_strength(int mb_p, int mb_q, int blk_p, int blk_q, bool mb_edge) const {
  const MbCtx& cp = mbs_[static_cast<uint64_t>(mb_p)];
  const MbCtx& cq = mbs_[static_cast<uint64_t>(mb_q)];
  if (cp.intra || cq.intra) return mb_edge ? 4 : 3;
  if (((cp.nzmask >> blk_p) & 1) || ((cq.nzmask >> blk_q) & 1)) return 2;
  const MotionMb& mp = cur_->motion[static_cast<uint64_t>(mb_p)];
  const MotionMb& mq = cur_->motion[static_cast<uint64_t>(mb_q)];
  const int b8p = ((blk_p >> 3) << 1) | ((blk_p & 3) >> 1), b8q = ((blk_q >> 3) << 1) | ((blk_q & 3) >> 1);
  int rp[2], rq[2];
  for (int l = 0; l < 2; ++l) {
    rp[l] = mp.ref[l][b8p] >= 0 ? mp.refid[l][b8p] : -1;
    rq[l] = mq.ref[l][b8q] >= 0 ? mq.refid[l][b8q] : -1;
  }
  const int np = (rp[0] >= 0) + (rp[1] >= 0), nq = (rq[0] >= 0) + (rq[1] >= 0);
  if (np != nq) return 1;
  auto far = [](const int16_t* a, const int16_t* b) { return iabs(a[0] - b[0]) >= 4 || iabs(a[1] - b[1]) >= 4; };
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
        bs[dir][e][s] = boundary_strength(mb_p, addr, blk_p, blk_q, e == 0);
      }
    }
  }
  uint8_t* ly = cur_->y.data() + static_cast<int64_t>(mby * 16) * cur_->stride + mbx * 16;
  const int ls = cur_->stride;
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
  const int cs = cur_->cstride;
  for (int plane = 0; plane < 2; ++plane) {
    uint8_t* base = (plane == 0 ? cur_->u.data() : cur_->v.data()) + static_cast<int64_t>(mby * 8) * cs + mbx * 8;
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

}  // namespace h264
}  // namespace hardware
