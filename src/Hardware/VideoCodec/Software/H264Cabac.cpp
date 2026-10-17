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

// H.264 decoding, part 3: the CABAC macroblock syntax (9.3).

#include "Hardware/VideoCodec/Software/H264Core.hpp"

namespace hardware {
namespace h264 {

#include "Hardware/VideoCodec/Software/H264Tables.inc"

namespace {

inline int imin(int a, int b) { return a < b ? a : b; }

enum MbKind { kI4x4 = 0, kI8x8, kI16, kPcm, kInter };

const uint8_t kCatOffSig[6] = {0, 15, 29, 44, 47, 0};
const uint8_t kCatOffAbs[6] = {0, 10, 20, 30, 39, 0};
const uint8_t kCatOffCbf[5] = {0, 4, 8, 12, 16};

struct BTypeInfo {
  uint8_t part_mode;
  uint8_t pred0, pred1;
};
const BTypeInfo kBTypes[23] = {
    {0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {1, 1, 1}, {2, 1, 1}, {1, 2, 2}, {2, 2, 2},
    {1, 1, 2}, {2, 1, 2}, {1, 2, 1}, {2, 2, 1}, {1, 1, 3}, {2, 1, 3}, {1, 2, 3}, {2, 2, 3},
    {1, 3, 1}, {2, 3, 1}, {1, 3, 2}, {2, 3, 2}, {1, 3, 3}, {2, 3, 3}, {3, 0, 0}};
const uint8_t kBSubShape[13] = {0, 0, 0, 0, 1, 2, 1, 2, 1, 2, 3, 3, 3};
const uint8_t kBSubPred[13] = {0, 1, 2, 3, 1, 1, 2, 2, 3, 3, 1, 2, 3};
const uint8_t kBlkRaster[16] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};

}  // namespace

int Core::cabac_mb_skip() {
  const MbCtx* a = nb(-1, 0);
  const MbCtx* b = nb(0, -1);
  const int inc = (a && !a->skip) + (b && !b->skip);
  return cabac_decision((sh_.type == kSliceB ? 24 : 11) + inc);
}

// mb_type of an I macroblock; with `in_pb` the suffix of an intra type in a P or B slice (ctxIdxOffset `base`).
int Core::cabac_mb_type_i(int base, bool in_pb) {
  if (!in_pb) {
    const MbCtx* a = nb(-1, 0);
    const MbCtx* b = nb(0, -1);
    auto cond = [](const MbCtx* n) { return n && (n->i16 || n->pcm || !n->intra); };
    if (!cabac_decision(base + cond(a) + cond(b))) return 0;
  } else if (!cabac_decision(base)) {
    return 0;
  }
  if (cabac_terminate()) return 25;
  const int luma15 = cabac_decision(in_pb ? base + 1 : base + 3);
  int chroma = 0;
  if (cabac_decision(in_pb ? base + 2 : base + 4)) chroma = 1 + cabac_decision(in_pb ? base + 2 : base + 5);
  int pm = cabac_decision(in_pb ? base + 3 : base + 6) << 1;
  pm |= cabac_decision(in_pb ? base + 3 : base + 7);
  return 1 + pm + 4 * chroma + 12 * luma15;
}

int Core::cabac_intra_chroma_mode() {
  const MbCtx* a = nb(-1, 0);
  const MbCtx* b = nb(0, -1);
  auto cond = [](const MbCtx* n) { return n && n->intra && !n->pcm && n->chroma_mode != 0; };
  if (!cabac_decision(64 + cond(a) + cond(b))) return 0;
  if (!cabac_decision(64 + 3)) return 1;
  return cabac_decision(64 + 3) ? 3 : 2;
}

bool Core::cabac_transform8x8_flag() {
  const MbCtx* a = nb(-1, 0);
  const MbCtx* b = nb(0, -1);
  return cabac_decision(399 + (a && a->t8x8) + (b && b->t8x8)) != 0;
}

int Core::cabac_qp_delta() {
  int ctx = 60 + (prev_qp_delta_nz_ ? 1 : 0);
  int k = 0;
  while (cabac_decision(ctx)) {
    ++k;
    ctx = k == 1 ? 62 : 63;
    if (k > 200 || cbr_->error()) return 1000;
  }
  return (k & 1) ? (k + 1) >> 1 : -(k >> 1);
}

int Core::cabac_ref_idx(int list, int bx, int by) {
  auto cond = [&](int x, int y) -> int {
    int ref, direct;
    if (x >= 0 && y >= 0) {
      const int b8 = (y >> 1) * 2 + (x >> 1);
      ref = cmotion_->ref[list][b8];
      direct = (cm_->direct_mask >> b8) & 1;
    } else {
      const MbCtx* n = x < 0 ? nb(-1, 0) : nb(0, -1);
      if (!n) return 0;
      const int xx = x < 0 ? 3 : x, yy = y < 0 ? 3 : y;
      const int b8 = (yy >> 1) * 2 + (xx >> 1);
      const MotionMb& mo = cur_->motion[static_cast<uint64_t>((mby_ + (y < 0 ? -1 : 0)) * mb_w_ + mbx_ + (x < 0 ? -1 : 0))];
      ref = mo.ref[list][b8];
      direct = (n->direct_mask >> b8) & 1;
    }
    return ref > 0 && !direct;
  };
  const int inc = cond(bx - 1, by) + 2 * cond(bx, by - 1);
  if (!cabac_decision(54 + inc)) return 0;
  int r = 1;
  int ctx = 54 + 4;
  while (cabac_decision(ctx)) {
    ctx = 54 + 5;
    if (++r > 32 || cbr_->error()) return 1000;
  }
  return r;
}

int Core::cabac_mvd(int list, int comp, int bx, int by) {
  auto mvd_at = [&](int x, int y) -> int {
    if (x >= 0 && y >= 0) return cm_->mvd[list][y * 4 + x][comp];
    const MbCtx* n = x < 0 ? nb(-1, 0) : nb(0, -1);
    if (!n) return 0;
    const int xx = x < 0 ? 3 : x, yy = y < 0 ? 3 : y;
    return n->mvd[list][yy * 4 + xx][comp];
  };
  const int sum = mvd_at(bx - 1, by) + mvd_at(bx, by - 1);
  const int base = comp ? 47 : 40;
  const int inc = sum < 3 ? 0 : sum > 32 ? 2 : 1;
  if (!cabac_decision(base + inc)) return 0;
  int v = 1;
  int ctx = base + 3;
  while (v < 9 && cabac_decision(ctx)) {
    ++v;
    if (ctx < base + 6) ++ctx;
  }
  if (v >= 9) {
    int k = 3;
    while (cabac_bypass()) {
      v += 1 << k;
      if (++k > 24 || cbr_->error()) return 0;
    }
    while (k--) v += cabac_bypass() << k;
  }
  return cabac_bypass() ? -v : v;
}

int Core::cabac_cbp() {
  const MbCtx* a = nb(-1, 0);
  const MbCtx* b = nb(0, -1);
  int cbp = 0;
  for (int b8 = 0; b8 < 4; ++b8) {
    int ca, cb;
    // A: the 8x8 block to the left; B: the 8x8 block above. A bit that is set (or an I_PCM macroblock) gives 0.
    if (b8 & 1) ca = ((cbp >> (b8 - 1)) & 1) ? 0 : 1;
    else ca = (a && (a->cbp >> (b8 + 1) & 1) == 0) ? 1 : 0;
    if (b8 & 2) cb = ((cbp >> (b8 - 2)) & 1) ? 0 : 1;
    else cb = (b && (b->cbp >> (b8 + 2) & 1) == 0) ? 1 : 0;
    if (cabac_decision(73 + ca + 2 * cb)) cbp |= 1 << b8;
  }
  auto cond1 = [](const MbCtx* n) { return n && (n->cbp >> 4) != 0; };
  auto cond2 = [](const MbCtx* n) { return n && (n->cbp >> 4) == 2; };
  if (cabac_decision(77 + cond1(a) + 2 * cond1(b))) {
    cbp |= (1 + cabac_decision(77 + 4 + cond2(a) + 2 * cond2(b))) << 4;
  }
  return cbp;
}

// coded_block_flag (9.3.3.1.1.9). cat: 0 Intra16x16 DC, 1 Intra16x16 AC, 2 luma 4x4, 3 chroma DC, 4 chroma AC.
bool Core::cabac_cbf(int cat, int bx, int by, int plane) {
  const bool intra = mb_type_ != kInter;
  auto cond = [&](int dx, int dy, int x, int y) -> int {
    // (dx, dy): the neighbouring macroblock; x, y: the block in it (or in this macroblock when dx = dy = 0).
    const MbCtx* n = (dx == 0 && dy == 0) ? cm_ : nb(dx, dy);
    if (!n) return intra ? 1 : 0;
    if (n->pcm) return 1;
    switch (cat) {
      case 0: return n->i16 ? (n->cbf_dc & 1) : 0;
      case 3: return (n->cbf_dc >> plane) & 1;
      case 4: return n->nz[16 + (plane - 1) * 4 + y * 2 + x] != 0;
      default: return n->nz[y * 4 + x] != 0;
    }
  };
  const int w = (cat == 3 || cat == 4) ? 2 : 4;
  int ca, cb;
  if (cat == 0 || cat == 3) {
    ca = cond(-1, 0, 0, 0);
    cb = cond(0, -1, 0, 0);
  } else {
    ca = bx > 0 ? cond(0, 0, bx - 1, by) : cond(-1, 0, w - 1, by);
    cb = by > 0 ? cond(0, 0, bx, by - 1) : cond(0, -1, bx, w - 1);
  }
  return cabac_decision(85 + kCatOffCbf[cat] + ca + 2 * cb) != 0;
}

// 7.3.5.3.3 / 9.3.2.7: the significance map and levels of one block. lvl[0 .. max_coeff-1] in scan order.
int Core::cabac_residual(int cat, int max_coeff, int* lvl) {
  for (int i = 0; i < max_coeff; ++i) lvl[i] = 0;
  const int sig_base = cat == 5 ? 402 : 105 + kCatOffSig[cat];
  const int last_base = cat == 5 ? 417 : 166 + kCatOffSig[cat];
  const int abs_base = cat == 5 ? 426 : 227 + kCatOffAbs[cat];
  int pos[64];
  int n = 0;
  bool ended = false;
  for (int i = 0; i < max_coeff - 1; ++i) {
    const int sc = cat == 3 ? imin(i, 2) : cat == 5 ? kSig8x8Ctx[i] : i;
    if (cabac_decision(sig_base + sc)) {
      pos[n++] = i;
      const int lc = cat == 3 ? imin(i, 2) : cat == 5 ? kLast8x8Ctx[i] : i;
      if (cabac_decision(last_base + lc)) {
        ended = true;
        break;
      }
    }
  }
  if (!ended) pos[n++] = max_coeff - 1;
  int gt1 = 0, eq1 = 0;
  for (int k = n - 1; k >= 0; --k) {
    const int ctx0 = abs_base + (gt1 != 0 ? 0 : imin(4, 1 + eq1));
    int a;
    if (!cabac_decision(ctx0)) {
      a = 1;
      ++eq1;
    } else {
      const int ctxn = abs_base + 5 + imin(4 - (cat == 3 ? 1 : 0), gt1);
      a = 2;
      while (a < 15 && cabac_decision(ctxn)) ++a;
      if (a == 15) {
        int e = 0;
        while (cabac_bypass()) {
          a += 1 << e;
          if (++e > 24 || cbr_->error()) return -1;
        }
        while (e--) a += cabac_bypass() << e;
      }
      ++gt1;
    }
    lvl[pos[k]] = cabac_bypass() ? -a : a;
  }
  return n;
}

bool Core::read_residual_cabac() {
  MbCtx* m = cm_;
  int lvl[64];
  const int qp = qp_;
  const bool intra = mb_type_ != kInter;
  const int list_y = intra ? 0 : 3;
  if (mb_type_ == kI16) {
    if (cabac_cbf(0, 0, 0, 0)) {
      if (cabac_residual(0, 16, lvl) < 0) return false;
      finish_luma_dc(lvl);
      m->cbf_dc |= 1;
    }
  }
  for (int b8 = 0; b8 < 4; ++b8) {
    if (!((cbp_ >> b8) & 1)) continue;
    const int qx = (b8 & 1) * 2, qy = (b8 >> 1) * 2;
    if (m->t8x8) {
      const int n = cabac_residual(5, 64, lvl);
      if (n < 0) return false;
      store8x8(&coef_[0][b8 * 64], lvl, intra ? 0 : 1, qp);
      coef_present_[0][b8] = true;
      for (int s = 0; s < 4; ++s) {
        const int bx = qx + (s & 1), by = qy + (s >> 1);
        m->nz[by * 4 + bx] = static_cast<uint8_t>(n);
        m->nzmask = static_cast<uint16_t>(m->nzmask | (1u << (by * 4 + bx)));
      }
    } else {
      for (int s = 0; s < 4; ++s) {
        const int bx = qx + (s & 1), by = qy + (s >> 1);
        const int blk = by * 4 + bx;
        const int cat = mb_type_ == kI16 ? 1 : 2;
        if (!cabac_cbf(cat, bx, by, 0)) continue;
        int n;
        if (cat == 1) {
          n = cabac_residual(1, 15, lvl);
          if (n < 0) return false;
          store4x4(&coef_[0][blk * 16], lvl, 1, 15, 0, qp);
        } else {
          n = cabac_residual(2, 16, lvl);
          if (n < 0) return false;
          store4x4(&coef_[0][blk * 16], lvl, 0, 16, list_y, qp);
        }
        m->nz[blk] = static_cast<uint8_t>(n);
        coef_present_[0][blk] = true;
        m->nzmask = static_cast<uint16_t>(m->nzmask | (1u << blk));
      }
    }
  }
  if (cbp_ >> 4) {
    int dc[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
    for (int c = 0; c < 2; ++c) {
      if (cabac_cbf(3, 0, 0, c + 1)) {
        if (cabac_residual(3, 4, lvl) < 0) return false;
        for (int i = 0; i < 4; ++i) dc[c][i] = lvl[i];
        m->cbf_dc = static_cast<uint8_t>(m->cbf_dc | (2 << c));
      }
    }
    for (int c = 0; c < 2; ++c) finish_chroma_dc(c + 1, dc[c]);
    if (cbp_ >> 4 == 2) {
      for (int c = 0; c < 2; ++c) {
        const int plane = c + 1;
        const int list = (intra ? 0 : 3) + plane;
        for (int s = 0; s < 4; ++s) {
          const int bx = s & 1, by = s >> 1;
          if (!cabac_cbf(4, bx, by, plane)) continue;
          const int n = cabac_residual(4, 15, lvl);
          if (n < 0) return false;
          m->nz[16 + c * 4 + s] = static_cast<uint8_t>(n);
          store4x4(&coef_[plane][s * 16], lvl, 1, 15, list, m->qpc[c]);
          coef_present_[plane][s] = true;
        }
      }
    }
  }
  return !cbr_->error();
}

bool Core::decode_mb_cabac() {
  set_inter_defaults();
  MbCtx* m = cm_;
  int itype = -1;
  int inter_type = -1;
  if (sh_.type == kSliceI) {
    itype = cabac_mb_type_i(3, false);
  } else if (sh_.type == kSliceP) {
    if (cabac_decision(14)) {
      itype = cabac_mb_type_i(17, true);
    } else if (!cabac_decision(15)) {
      inter_type = cabac_decision(16) ? 3 : 0;
    } else {
      inter_type = cabac_decision(17) ? 1 : 2;
    }
  } else {
    const MbCtx* a = nb(-1, 0);
    const MbCtx* b = nb(0, -1);
    const int inc = (a && !(a->skip || a->direct16)) + (b && !(b->skip || b->direct16));
    if (!cabac_decision(27 + inc)) {
      inter_type = 0;
    } else if (!cabac_decision(27 + 3)) {
      inter_type = 1 + cabac_decision(27 + 5);
    } else {
      int bits = cabac_decision(27 + 4) << 3;
      bits |= cabac_decision(27 + 5) << 2;
      bits |= cabac_decision(27 + 5) << 1;
      bits |= cabac_decision(27 + 5);
      if (bits < 8) inter_type = bits + 3;
      else if (bits == 13) itype = cabac_mb_type_i(32, true);
      else if (bits == 14) inter_type = 11;
      else if (bits == 15) inter_type = 22;
      else inter_type = ((bits << 1) | cabac_decision(27 + 5)) - 4;
    }
  }
  if (cbr_->error()) return false;
  bool no_sub_lt8 = true;
  if (itype >= 0) {
    if (itype == 25) {
      mb_type_ = kPcm;
      // The arithmetic decoder has consumed exactly the bits up to the PCM alignment.
      BitReader* br = cbr_;
      if (!read_pcm(br)) return false;
      cabac_init_engine();
      m->pcm = 1;
      m->cbp = 0x2f;
      m->cbf_dc = 7;
      for (int i = 0; i < 24; ++i) m->nz[i] = 16;
      m->nzmask = 0xffff;
      prev_qp_delta_nz_ = 0;
      return finish_mb(false);
    }
    if (itype == 0) {
      mb_type_ = kI4x4;
      if (pp_->transform_8x8 && cabac_transform8x8_flag()) {
        m->t8x8 = 1;
        mb_type_ = kI8x8;
      }
      const int n = m->t8x8 ? 4 : 16;
      for (int i = 0; i < n; ++i) {
        const int flag = cabac_decision(68);
        int rem = 0;
        if (!flag) {
          rem = cabac_decision(69);
          rem |= cabac_decision(69) << 1;
          rem |= cabac_decision(69) << 2;
        }
        const int blk = m->t8x8 ? (i & 1) * 2 + (i >> 1) * 8 : kBlkRaster[i];
        const int bx = blk & 3, by = blk >> 2;
        const int pred = predicted_intra_mode(bx, by, m->t8x8 != 0);
        const int mode = flag ? pred : (rem < pred ? rem : rem + 1);
        if (m->t8x8) {
          for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) m->ipred[(by + dy) * 4 + bx + dx] = static_cast<int8_t>(mode);
        } else {
          m->ipred[by * 4 + bx] = static_cast<int8_t>(mode);
        }
      }
    } else {
      mb_type_ = kI16;
      m->i16 = 1;
      i16_mode_ = (itype - 1) & 3;
      cbp_ = (((itype - 1) >> 2) % 3) << 4 | ((itype - 1) / 12 ? 15 : 0);
    }
    m->chroma_mode = static_cast<uint8_t>(cabac_intra_chroma_mode());
  } else {
    mb_type_ = kInter;
    const bool is_b = sh_.type == kSliceB;
    int part_mode, pred0 = 1, pred1 = 1;
    int preds[4] = {1, 1, 0, 0};
    int sub_shape[4] = {0, 0, 0, 0};
    if (!is_b) {
      part_mode = inter_type;
    } else if (inter_type == 0) {
      m->direct16 = 1;
      m->direct_mask = 15;
      part_mode_ = 3;
      direct_block(15);
      part_mode = -1;
    } else {
      part_mode = kBTypes[inter_type].part_mode;
      pred0 = kBTypes[inter_type].pred0;
      pred1 = kBTypes[inter_type].pred1;
      preds[0] = pred0;
      preds[1] = pred1;
    }
    if (part_mode >= 0) {
      part_mode_ = part_mode;
      int nparts = part_mode == 0 ? 1 : part_mode == 3 ? 4 : 2;
      if (part_mode == 3) {
        for (int i = 0; i < 4; ++i) {
          if (!is_b) {
            int st;
            if (cabac_decision(21)) st = 0;
            else if (!cabac_decision(22)) st = 1;
            else st = cabac_decision(23) ? 2 : 3;
            sub_shape[i] = st;
            preds[i] = 1;
          } else {
            int st;
            if (!cabac_decision(36)) st = 0;
            else if (!cabac_decision(37)) st = 1 + cabac_decision(39);
            else {
              st = 3;
              if (cabac_decision(38)) {
                if (cabac_decision(39)) st = 11 + cabac_decision(39);
                else {
                  st += 4;
                  st += 2 * cabac_decision(39);
                  st += cabac_decision(39);
                }
              } else {
                st += 2 * cabac_decision(39);
                st += cabac_decision(39);
              }
            }
            if (st > 12) return false;
            sub_shape[i] = kBSubShape[st];
            preds[i] = kBSubPred[st];
            if (st == 0) m->direct_mask = static_cast<uint8_t>(m->direct_mask | (1 << i));
          }
          if (preds[i] == 0) {
            if (!sp_->direct_8x8_inference) no_sub_lt8 = false;
          } else if (sub_shape[i] != 0) {
            no_sub_lt8 = false;
          }
        }
      }
      int refs[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
      const int lists = is_b ? 2 : 1;
      for (int l = 0; l < lists; ++l) {
        for (int p = 0; p < nparts; ++p) {
          if (!((preds[p] >> l) & 1)) continue;
          int bx, by;
          int b8s[4], nb8 = 0;
          if (part_mode == 0) { bx = by = 0; b8s[0] = 0; b8s[1] = 1; b8s[2] = 2; b8s[3] = 3; nb8 = 4; }
          else if (part_mode == 1) { bx = 0; by = p * 2; b8s[0] = p * 2; b8s[1] = p * 2 + 1; nb8 = 2; }
          else if (part_mode == 2) { bx = p * 2; by = 0; b8s[0] = p; b8s[1] = p + 2; nb8 = 2; }
          else { bx = (p & 1) * 2; by = (p >> 1) * 2; b8s[0] = p; nb8 = 1; }
          int r = 0;
          if (ref_count_[l] > 1) r = cabac_ref_idx(l, bx, by);
          if (r >= ref_count_[l] || cbr_->error()) return false;
          refs[l][p] = r;
          for (int k = 0; k < nb8; ++k) cmotion_->ref[l][b8s[k]] = static_cast<int8_t>(r);
        }
      }
      if (!finish_inter_parse(nullptr, part_mode, preds, sub_shape, refs)) return false;
    }
  }
  if (cbr_->error()) return false;
  if (mb_type_ != kI16) {
    cbp_ = cabac_cbp();
    if (mb_type_ == kInter && (cbp_ & 15) && pp_->transform_8x8 && no_sub_lt8 &&
        (!m->direct16 || sp_->direct_8x8_inference)) {
      m->t8x8 = cabac_transform8x8_flag();
    }
  }
  m->cbp = static_cast<uint8_t>(cbp_);
  if (cbp_ > 0 || mb_type_ == kI16) {
    const int dqp = cabac_qp_delta();
    if (dqp < -26 || dqp > 25) return false;
    qp_ = (qp_ + dqp + 52) % 52;
    prev_qp_delta_nz_ = dqp != 0;
    update_qp();
    if (!read_residual_cabac()) return false;
  } else {
    prev_qp_delta_nz_ = 0;
  }
  return finish_mb(false);
}

}  // namespace h264
}  // namespace hardware
