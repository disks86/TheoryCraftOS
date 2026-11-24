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

// The H.264 picture encoder's syntax (H264PictureEncoderCore.hpp): the slice header (7.3.3), the macroblock layer in CAVLC (7.3.5, 9.2) and CABAC (9.3: the arithmetic
// encoder of 9.3.4.2, the binarizations of 9.3.2 and the context selection of 9.3.3.1.1 - the decoder's H264Cabac.cpp read backwards).

#include "Hardware/VideoCodec/Software/H264Core.hpp"
#include "Hardware/VideoCodec/Software/H264PictureEncoderCore.hpp"

namespace hardware {
namespace h264 {

#include "Hardware/VideoCodec/Software/H264Tables.inc"

namespace {

struct VlcRef {
  const VlcEntry* entries;
  int count;
};
template <int N>
constexpr VlcRef vlc(const VlcEntry (&a)[N]) {
  return {a, N};
}
const VlcRef kCoeffTokenTables[4] = {vlc(kCoeffToken0), vlc(kCoeffToken1), vlc(kCoeffToken2), vlc(kCoeffToken3)};
const VlcRef kCoeffTokenDc = vlc(kCoeffTokenChromaDc);
const VlcRef kTotalZerosTables[15] = {vlc(kTotalZeros1),  vlc(kTotalZeros2),  vlc(kTotalZeros3),  vlc(kTotalZeros4),  vlc(kTotalZeros5),
                                      vlc(kTotalZeros6),  vlc(kTotalZeros7),  vlc(kTotalZeros8),  vlc(kTotalZeros9),  vlc(kTotalZeros10),
                                      vlc(kTotalZeros11), vlc(kTotalZeros12), vlc(kTotalZeros13), vlc(kTotalZeros14), vlc(kTotalZeros15)};
const VlcRef kTotalZerosDcTables[3] = {vlc(kTotalZerosChromaDc1), vlc(kTotalZerosChromaDc2), vlc(kTotalZerosChromaDc3)};
const VlcRef kRunBeforeTables[7] = {vlc(kRunBefore1), vlc(kRunBefore2), vlc(kRunBefore3), vlc(kRunBefore4), vlc(kRunBefore5), vlc(kRunBefore6), vlc(kRunBefore7)};

const uint8_t kCatOffSig[6] = {0, 15, 29, 44, 47, 0};
const uint8_t kCatOffAbs[6] = {0, 10, 20, 30, 39, 0};
const uint8_t kCatOffCbf[5] = {0, 4, 8, 12, 16};

void put_vlc(BitWriter& w, const VlcRef& table, int symbol) {
  for (int i = 0; i < table.count; ++i) {
    if (table.entries[i].sym == symbol) {
      w.put(table.entries[i].code, table.entries[i].len);
      return;
    }
  }
  w.fail();
}

// 9.2 in reverse: one residual block of max_coeff levels in scan order (`stride` apart: the 4x4 parts of an 8x8 block interleave); nc < 0 is the chroma DC block.
// Returns TotalCoeff.
int write_block(BitWriter& w, int nc, ArrayView<const int> lvl, int max_coeff, int stride = 1) {
  int vals[16], pos[16];
  int total = 0;
  for (int i = max_coeff - 1; i >= 0; --i) {
    const int v = lvl[static_cast<uint64_t>(i * stride)];
    if (v != 0) {
      vals[total] = v;
      pos[total] = i;
      ++total;
    }
  }
  int t1 = 0;
  while (t1 < total && t1 < 3 && iabs(vals[t1]) == 1) ++t1;
  put_vlc(w, nc < 0 ? kCoeffTokenDc : kCoeffTokenTables[nc < 2 ? 0 : nc < 4 ? 1 : nc < 8 ? 2 : 3], total * 4 + t1);
  if (total == 0) return 0;
  for (int i = 0; i < t1; ++i) w.bit(vals[i] < 0);
  int suffix_len = (total > 10 && t1 < 3) ? 1 : 0;
  for (int i = t1; i < total; ++i) {
    const int v = vals[i];
    int code = v > 0 ? 2 * v - 2 : -2 * v - 1;
    if (i == t1 && t1 < 3) code -= 2;
    int prefix, suffix = 0, suffix_bits = 0;
    if (suffix_len == 0) {
      if (code < 14) {
        prefix = code;
      } else if (code < 30) {
        prefix = 14;
        suffix = code - 14;
        suffix_bits = 4;
      } else {
        prefix = 15;
        suffix = code - 30;
        suffix_bits = 12;
      }
    } else if (code < (15 << suffix_len)) {
      prefix = code >> suffix_len;
      suffix = code & ((1 << suffix_len) - 1);
      suffix_bits = suffix_len;
    } else {
      prefix = 15;
      suffix = code - (15 << suffix_len);
      suffix_bits = 12;
    }
    if (suffix_bits > 0 && suffix >= (1 << suffix_bits)) w.fail();   // (levels are clamped so that this cannot happen)
    w.put(1, prefix + 1);
    if (suffix_bits > 0) w.put(static_cast<uint32_t>(suffix), suffix_bits);
    if (suffix_len == 0) suffix_len = 1;
    if (iabs(v) > (3 << (suffix_len - 1)) && suffix_len < 6) ++suffix_len;
  }
  int zeros_left = 0;
  if (total < max_coeff) {
    zeros_left = pos[0] + 1 - total;
    put_vlc(w, nc < 0 ? kTotalZerosDcTables[total - 1] : kTotalZerosTables[total - 1], zeros_left);
  }
  for (int i = 0; i < total - 1 && zeros_left > 0; ++i) {
    const int run = pos[i] - pos[i + 1] - 1;
    put_vlc(w, kRunBeforeTables[imin(zeros_left, 7) - 1], run);
    zeros_left -= run;
  }
  return total;
}

int count_nonzero(ArrayView<const int> lvl) {
  int c = 0;
  for (const int v : lvl) c += v != 0;
  return c;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// The arithmetic encoder (9.3.4.2)

void CabacWriter::start(int model, int slice_qp) {
  const int qp = clip3(0, 51, slice_qp);
  for (int i = 0; i < kCabacContexts; ++i) {
    const int m = kCabacInit[(i * 4 + model) * 2], n = kCabacInit[(i * 4 + model) * 2 + 1];
    const int pre = clip3(1, 126, ((m * qp) >> 4) + n);
    s_.ctx[i] = pre <= 63 ? static_cast<uint8_t>((63 - pre) << 1) : static_cast<uint8_t>(((pre - 64) << 1) | 1);
  }
  s_.low = 0;
  s_.range = 510;
  s_.outstanding = 0;
  s_.first = true;
  s_.bins = 0;
}

void CabacWriter::put_bit(BitWriter& w, int b) {
  if (s_.first) s_.first = false;
  else w.bit(static_cast<uint32_t>(b));
  for (; s_.outstanding > 0; --s_.outstanding) w.bit(static_cast<uint32_t>(1 - b));
}

void CabacWriter::renorm(BitWriter& w) {
  while (s_.range < 256) {
    if (s_.low < 256) {
      put_bit(w, 0);
    } else if (s_.low >= 512) {
      s_.low -= 512;
      put_bit(w, 1);
    } else {
      s_.low -= 256;
      ++s_.outstanding;
    }
    s_.range <<= 1;
    s_.low <<= 1;
  }
}

void CabacWriter::decision(BitWriter& w, int ctx, int bin) {
  ++s_.bins;
  uint8_t& st = s_.ctx[ctx];
  int state = st >> 1, mps = st & 1;
  const uint32_t lps = kRangeLps[state * 4 + ((s_.range >> 6) & 3)];
  s_.range -= lps;
  if ((bin != 0) != (mps != 0)) {
    s_.low += s_.range;
    s_.range = lps;
    if (state == 0) mps = 1 - mps;
    state = kStateNext[state * 2];
  } else {
    state = kStateNext[state * 2 + 1];
  }
  st = static_cast<uint8_t>((state << 1) | mps);
  renorm(w);
}

void CabacWriter::bypass(BitWriter& w, int bin) {
  ++s_.bins;
  s_.low <<= 1;
  if (bin) s_.low += s_.range;
  if (s_.low >= 1024) {
    put_bit(w, 1);
    s_.low -= 1024;
  } else if (s_.low < 512) {
    put_bit(w, 0);
  } else {
    s_.low -= 512;
    ++s_.outstanding;
  }
}

void CabacWriter::terminate(BitWriter& w, int bin) {
  ++s_.bins;
  s_.range -= 2;
  if (!bin) {
    renorm(w);
    return;
  }
  s_.low += s_.range;
  // 9.3.4.5: EncodeFlush
  s_.range = 2;
  renorm(w);
  put_bit(w, static_cast<int>((s_.low >> 9) & 1));
  w.put(((s_.low >> 7) & 3) | 1, 2);
}

// ---------------------------------------------------------------------------------------------------------------
// The slice header (7.3.3)

void PictureCoder::write_slice_header(uint32_t first_mb) {
  BitWriter& w = bw_;
  const H264EncPicture& p = *pic_;
  const bool idr = p.type == H264EncFrameType::Idr;
  w.ue(first_mb);
  w.ue(p_slice() ? 5 : b_slice() ? 6 : 7);
  w.ue(p.pic_parameter_set_id);
  w.put(p.frame_num & ((1u << p.log2_max_frame_num) - 1), p.log2_max_frame_num);
  if (idr) w.ue(p.idr_pic_id);
  if (p.pic_order_cnt_type == 0) w.put(p.poc & ((1u << p.log2_max_poc_lsb) - 1), p.log2_max_poc_lsb);
  if (b_slice()) w.bit(p.direct == H264EncDirect::Temporal ? 0 : 1);   // direct_spatial_mv_pred_flag (without direct prediction no macroblock is direct)
  if (p_slice() || b_slice()) {
    w.bit(1);   // num_ref_idx_active_override_flag
    w.ue(static_cast<uint32_t>(p.list[0].size() - 1));
    if (b_slice()) w.ue(static_cast<uint32_t>(p.list[1].size() - 1));
    for (int l = 0; l < (b_slice() ? 2 : 1); ++l) {
      uint64_t count = 0;
      for (const H264EncListModification& m : p.modification[l]) {
        if (m.idc == 3) break;
        ++count;
      }
      w.bit(count ? 1 : 0);
      if (!count) continue;
      for (uint64_t i = 0; i < count; ++i) {
        const H264EncListModification& m = p.modification[l][i];
        w.ue(m.idc);
        if (m.idc == 2) w.ue(m.long_term_pic_num);
        else w.ue(m.abs_diff_pic_num_minus1);
      }
      w.ue(3);
    }
  }
  if (p.reference || idr) {
    if (idr) {
      w.bit(0);   // no_output_of_prior_pics_flag
      w.bit(0);   // long_term_reference_flag
    } else {
      w.bit(p.adaptive_ref_pic_marking ? 1 : 0);
      if (p.adaptive_ref_pic_marking) {
        for (const H264EncMarking& m : p.marking) {
          if (m.operation == 0) break;
          w.ue(m.operation);
          if (m.operation == 1 || m.operation == 3) w.ue(m.difference_of_pic_nums_minus1);
          if (m.operation == 2) w.ue(m.long_term_pic_num);
          if (m.operation == 3 || m.operation == 6) w.ue(m.long_term_frame_idx);
          if (m.operation == 4) w.ue(m.max_long_term_frame_idx_plus1);
        }
        w.ue(0);
      }
    }
  }
  if (cabac() && (p_slice() || b_slice())) w.ue(0);   // cabac_init_idc
  w.se(static_cast<int>(p.qp) - 26);                   // slice_qp_delta (pic_init_qp 26)
  w.ue(p.disable_deblocking_filter_idc);
  if (p.disable_deblocking_filter_idc != 1) {
    w.se(0);   // slice_alpha_c0_offset_div2
    w.se(0);   // slice_beta_offset_div2
  }
}

void PictureCoder::finish_slice_data() {
  if (cabac()) {
    cw_.terminate(bw_, 1);   // end_of_slice_flag (its flush writes the rbsp_stop_one_bit)
    bw_.align_with_zeros();
    return;
  }
  if (skip_run_ > 0) bw_.ue(skip_run_);
  skip_run_ = 0;
  bw_.trailing();
}

void PictureCoder::write_skip(int mbx, int mby) {
  if (!cabac()) {
    ++skip_run_;
    return;
  }
  auto cond = [](const MbData* n) { return n && !n->skip ? 1 : 0; };
  cw_.decision(bw_, (b_slice() ? 24 : 11) + cond(mb_at(mbx - 1, mby)) + cond(mb_at(mbx, mby - 1)), 1);   // mb_skip_flag
}

void PictureCoder::write_mb(int mbx, int mby, int qp_delta) {
  if (cabac()) write_mb_cabac(mbx, mby, qp_delta);
  else write_mb_cavlc(mbx, mby, qp_delta);
}

// ---------------------------------------------------------------------------------------------------------------
// CAVLC (7.3.5 with 9.2)

namespace {

// 9.2.1: nC of a 4x4 block; plane 0 luma (bx, by 0 .. 3), 1 and 2 chroma (0 .. 1). `cur` the counts of this macroblock so far.
int nc_of(const MbData* left, const MbData* above, int bx, int by, int plane, const uint8_t* cur) {
  const int w = plane == 0 ? 4 : 2;
  const int base = plane == 0 ? 0 : 16 + (plane - 1) * 4;
  int na = -1, nb = -1;
  if (bx > 0) na = cur[base + by * w + bx - 1];
  else if (left) na = left->nz[base + by * w + w - 1];
  if (by > 0) nb = cur[base + (by - 1) * w + bx];
  else if (above) nb = above->nz[base + (w - 1) * w + bx];
  if (na >= 0 && nb >= 0) return (na + nb + 1) >> 1;
  if (na >= 0) return na;
  if (nb >= 0) return nb;
  return 0;
}

}  // namespace

void PictureCoder::write_mb_cavlc(int mbx, int mby, int qp_delta) {
  BitWriter& w = bw_;
  const H264EncPicture& p = *pic_;
  Mb& m = m_;
  uint8_t* nz = m.nz;
  const MbData* left = mb_at(mbx - 1, mby);
  const MbData* above = mb_at(mbx, mby - 1);
  if (p_slice() || b_slice()) {
    w.ue(skip_run_);
    skip_run_ = 0;
  }
  const uint32_t intra_offset = p_slice() ? 5 : b_slice() ? 23 : 0;
  const bool intra = m.kind == Kind::I16 || m.kind == Kind::I8;
  const int cbp = m.cbp_luma | (m.cbp_chroma << 4);
  if (m.kind == Kind::I16) {
    const int type = 1 + m.i16_mode + 4 * m.cbp_chroma + (m.cbp_luma ? 12 : 0);
    w.ue(intra_offset + static_cast<uint32_t>(type));
    w.ue(static_cast<uint32_t>(m.chroma_mode));
    w.se(qp_delta);
    write_block(w, nc_of(left, above, 0, 0, 0, nz), m.dc, 16);
  } else {
    if (m.kind == Kind::I8) {
      w.ue(intra_offset);   // I_NxN
      w.bit(1);             // transform_size_8x8_flag
      for (int b = 0; b < 4; ++b) {
        if (m.i8_mode[b] == m.i8_pred[b]) {
          w.bit(1);
        } else {
          w.bit(0);
          w.put(static_cast<uint32_t>(m.i8_mode[b] < m.i8_pred[b] ? m.i8_mode[b] : m.i8_mode[b] - 1), 3);
        }
      }
      w.ue(static_cast<uint32_t>(m.chroma_mode));
    } else if (m.kind == Kind::Direct) {
      w.ue(0);   // B_Direct_16x16
    } else {
      const uint64_t n0 = p.list[0].size(), n1 = p.list[1].size();
      if (p_slice()) w.ue(0);   // P_L0_16x16
      else w.ue(static_cast<uint32_t>(m.pred_flags == 1 ? 1 : m.pred_flags == 2 ? 2 : 3));
      auto te = [&](uint64_t count, int v) {
        if (count == 2) w.bit(v == 0 ? 1 : 0);
        else if (count > 2) w.ue(static_cast<uint32_t>(v));
      };
      if (m.pred_flags & 1) te(n0, m.ref[0]);
      if (m.pred_flags & 2) te(n1, m.ref[1]);
      for (int l = 0; l < 2; ++l) {
        if (!(m.pred_flags & (1 << l))) continue;
        w.se(m.mv[l].x - m.mvp[l].x);
        w.se(m.mv[l].y - m.mvp[l].y);
      }
    }
    const uint8_t* table = intra ? kCbpIntra : kCbpInter;
    int code = 0;
    while (code < 48 && table[code] != cbp) ++code;
    w.ue(static_cast<uint32_t>(code));
    if (m.kind != Kind::I8 && m.cbp_luma != 0 && p.transform_8x8) w.bit(m.t8x8 ? 1 : 0);   // (direct_8x8_inference_flag 1: B_Direct_16x16 too)
    if (cbp != 0) w.se(qp_delta);
  }
  if (m.kind == Kind::I16 || cbp != 0) {
    for (int b8 = 0; b8 < 4; ++b8) {
      if (!((m.cbp_luma >> b8) & 1)) continue;
      for (int s = 0; s < 4; ++s) {
        const int bx = (b8 & 1) * 2 + (s & 1), by = (b8 >> 1) * 2 + (s >> 1);
        const int blk = by * 4 + bx;
        const int nc = nc_of(left, above, bx, by, 0, nz);
        int n;
        if (m.t8x8) n = write_block(w, nc, ArrayView<const int>(m.l8[b8]).subview(static_cast<uint64_t>(s)), 16, 4);   // (7.3.5.3.2: lumaList8x8[4 * i + s])
        else if (m.kind == Kind::I16) n = write_block(w, nc, ArrayView<const int>(m.ac[blk]).subview(1), 15);
        else n = write_block(w, nc, m.ac[blk], 16);
        nz[blk] = static_cast<uint8_t>(n);
      }
    }
    if (m.cbp_chroma > 0) {
      for (int c = 0; c < 2; ++c) write_block(w, -1, m.cdc[c], 4);
      if (m.cbp_chroma == 2)
        for (int c = 0; c < 2; ++c)
          for (int s = 0; s < 4; ++s) nz[16 + c * 4 + s] = static_cast<uint8_t>(write_block(w, nc_of(left, above, s & 1, s >> 1, 1 + c, nz), ArrayView<const int>(m.cac[c][s]).subview(1), 15));
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// CABAC (7.3.5 with 9.3)

void PictureCoder::cabac_mb_type(int mbx, int mby) {
  const Mb& m = m_;
  BitWriter& w = bw_;
  const MbData* a = mb_at(mbx - 1, mby);
  const MbData* b = mb_at(mbx, mby - 1);
  const bool intra = m.kind == Kind::I16 || m.kind == Kind::I8;
  // the I macroblock types (9.3.2.5, Table 9-36): from bin 0 at `base`, `in_pb` the suffix of an intra type of a P or B slice
  auto intra_type = [&](int base, bool in_pb) {
    if (!in_pb) {
      auto cond = [](const MbData* n) { return n && !n->inxn; };
      cw_.decision(w, base + cond(a) + cond(b), m.kind == Kind::I16 ? 1 : 0);
    } else {
      cw_.decision(w, base, m.kind == Kind::I16 ? 1 : 0);
    }
    if (m.kind != Kind::I16) return;
    cw_.terminate(w, 0);   // (not I_PCM)
    cw_.decision(w, in_pb ? base + 1 : base + 3, m.cbp_luma ? 1 : 0);
    cw_.decision(w, in_pb ? base + 2 : base + 4, m.cbp_chroma != 0 ? 1 : 0);
    if (m.cbp_chroma != 0) cw_.decision(w, in_pb ? base + 2 : base + 5, m.cbp_chroma == 2 ? 1 : 0);
    cw_.decision(w, in_pb ? base + 3 : base + 6, (m.i16_mode >> 1) & 1);
    cw_.decision(w, in_pb ? base + 3 : base + 7, m.i16_mode & 1);
  };
  if (!p_slice() && !b_slice()) {
    intra_type(3, false);
    return;
  }
  if (p_slice()) {
    if (intra) {
      cw_.decision(w, 14, 1);
      intra_type(17, true);
      return;
    }
    cw_.decision(w, 14, 0);   // P_L0_16x16: 0 0 0
    cw_.decision(w, 15, 0);
    cw_.decision(w, 16, 0);
    return;
  }
  auto cond = [](const MbData* n) { return n && !(n->skip || n->direct); };
  const int inc = cond(a) + cond(b);
  if (m.kind == Kind::Direct) {
    cw_.decision(w, 27 + inc, 0);
    return;
  }
  cw_.decision(w, 27 + inc, 1);
  if (intra) {   // prefix 1 1 1 1 0 1
    cw_.decision(w, 27 + 3, 1);
    cw_.decision(w, 27 + 4, 1);
    cw_.decision(w, 27 + 5, 1);
    cw_.decision(w, 27 + 5, 0);
    cw_.decision(w, 27 + 5, 1);
    intra_type(32, true);
    return;
  }
  if (m.pred_flags != 3) {   // B_L0_16x16 1 0 0, B_L1_16x16 1 0 1
    cw_.decision(w, 27 + 3, 0);
    cw_.decision(w, 27 + 5, m.pred_flags == 2 ? 1 : 0);
    return;
  }
  cw_.decision(w, 27 + 3, 1);   // B_Bi_16x16: 1 1 0 0 0 0
  cw_.decision(w, 27 + 4, 0);
  cw_.decision(w, 27 + 5, 0);
  cw_.decision(w, 27 + 5, 0);
  cw_.decision(w, 27 + 5, 0);
}

void PictureCoder::cabac_cbp(int mbx, int mby) {
  const Mb& m = m_;
  BitWriter& w = bw_;
  const MbData* a = mb_at(mbx - 1, mby);
  const MbData* b = mb_at(mbx, mby - 1);
  for (int b8 = 0; b8 < 4; ++b8) {
    // A: the 8x8 block to the left, B: the one above; a set bit of a coded macroblock gives 0, so does an unavailable one; a skipped one gives 1
    int ca, cb;
    if (b8 & 1) ca = ((m.cbp_luma >> (b8 - 1)) & 1) ? 0 : 1;
    else ca = (a && ((a->cbp >> (b8 + 1)) & 1) == 0) ? 1 : 0;
    if (b8 & 2) cb = ((m.cbp_luma >> (b8 - 2)) & 1) ? 0 : 1;
    else cb = (b && ((b->cbp >> (b8 + 2)) & 1) == 0) ? 1 : 0;
    cw_.decision(w, 73 + ca + 2 * cb, (m.cbp_luma >> b8) & 1);
  }
  auto cond1 = [](const MbData* n) { return n && (n->cbp >> 4) != 0; };
  auto cond2 = [](const MbData* n) { return n && (n->cbp >> 4) == 2; };
  cw_.decision(w, 77 + cond1(a) + 2 * cond1(b), m.cbp_chroma != 0 ? 1 : 0);
  if (m.cbp_chroma != 0) cw_.decision(w, 77 + 4 + cond2(a) + 2 * cond2(b), m.cbp_chroma == 2 ? 1 : 0);
}

void PictureCoder::cabac_qp_delta(int delta) {
  BitWriter& w = bw_;
  const int k = delta > 0 ? 2 * delta - 1 : -2 * delta;
  for (int i = 0; i <= k; ++i) cw_.decision(w, i == 0 ? 60 + (dqp_nonzero_ ? 1 : 0) : i == 1 ? 62 : 63, i < k ? 1 : 0);
}

void PictureCoder::cabac_ref_idx(int mbx, int mby, int list, int ref) {
  BitWriter& w = bw_;
  // (refIdxLX of the neighbouring partitions above zero, not of skipped, direct or intra macroblocks)
  auto cond = [&](const MbData* n, int b8) { return n && !n->skip && !n->intra && !n->direct && n->ref[list][b8] > 0 ? 1 : 0; };
  const int inc = cond(mb_at(mbx - 1, mby), 1) + 2 * cond(mb_at(mbx, mby - 1), 2);
  cw_.decision(w, 54 + inc, ref > 0 ? 1 : 0);
  if (ref == 0) return;
  for (int i = 1; i <= ref; ++i) cw_.decision(w, i == 1 ? 54 + 4 : 54 + 5, i < ref ? 1 : 0);
}

void PictureCoder::cabac_mvd(int mbx, int mby, int list, int comp, int value) {
  BitWriter& w = bw_;
  auto abs_of = [&](const MbData* n) { return n ? static_cast<int>(n->mvd[list][comp]) : 0; };
  const int sum = abs_of(mb_at(mbx - 1, mby)) + abs_of(mb_at(mbx, mby - 1));
  const int base = comp ? 47 : 40;
  const int a = iabs(value);
  cw_.decision(w, base + (sum < 3 ? 0 : sum > 32 ? 2 : 1), a > 0 ? 1 : 0);
  if (a == 0) return;
  // UEG3, uCoff 9: the rest of the truncated unary prefix, then the 3rd-order Exp-Golomb suffix and the sign, bypassed
  int ctx = base + 3;
  for (int j = 1; j < imin(a, 9); ++j) {
    cw_.decision(w, ctx, 1);
    if (ctx < base + 6) ++ctx;
  }
  if (a < 9) {
    cw_.decision(w, ctx, 0);
  } else {
    uint32_t v = static_cast<uint32_t>(a - 9);
    int k = 3;
    while (v >= (1u << k)) {
      cw_.bypass(w, 1);
      v -= 1u << k;
      ++k;
    }
    cw_.bypass(w, 0);
    while (k-- > 0) cw_.bypass(w, static_cast<int>((v >> k) & 1));
  }
  cw_.bypass(w, value < 0 ? 1 : 0);
}

// coded_block_flag (9.3.3.1.1.9) of a block: cat 0 Intra16x16 DC, 1 Intra16x16 AC, 2 luma 4x4, 3 chroma DC, 4 chroma AC; (bx, by) the 4x4 block (luma 0 .. 3, chroma
// 0 .. 1), plane 1 / 2 the chroma planes.
bool PictureCoder::cabac_cbf(int mbx, int mby, int cat, int bx, int by, int plane, bool coded) {
  const Mb& m = m_;
  const bool intra = m.kind == Kind::I16 || m.kind == Kind::I8;
  auto cond_mb = [&](const MbData* n, int x, int y) -> int {
    if (!n) return intra ? 1 : 0;
    switch (cat) {
      case 0: return n->i16 ? (n->cbf_dc & 1) : 0;
      case 3: return (n->cbf_dc >> plane) & 1;
      case 4: return n->nz[16 + (plane - 1) * 4 + y * 2 + x] != 0;
      default: return n->nz[y * 4 + x] != 0;
    }
  };
  auto cond_cur = [&](int x, int y) -> int { return cat == 4 ? m.nz[16 + (plane - 1) * 4 + y * 2 + x] != 0 : m.nz[y * 4 + x] != 0; };
  const int w4 = cat == 4 ? 2 : 4;
  int ca, cb;
  if (cat == 0 || cat == 3) {
    ca = cond_mb(mb_at(mbx - 1, mby), 0, 0);
    cb = cond_mb(mb_at(mbx, mby - 1), 0, 0);
  } else {
    ca = bx > 0 ? cond_cur(bx - 1, by) : cond_mb(mb_at(mbx - 1, mby), w4 - 1, by);
    cb = by > 0 ? cond_cur(bx, by - 1) : cond_mb(mb_at(mbx, mby - 1), bx, w4 - 1);
  }
  cw_.decision(bw_, 85 + kCatOffCbf[cat] + ca + 2 * cb, coded ? 1 : 0);
  return coded;
}

// 7.3.5.3.3 / 9.3.2.7: the significance map and the levels of one block (levels in scan order, at least one not zero). cat 5: an 8x8 block.
void PictureCoder::cabac_residual(int cat, ArrayView<const int> lvl, int max_coeff) {
  BitWriter& w = bw_;
  const int sig_base = cat == 5 ? 402 : 105 + kCatOffSig[cat];
  const int last_base = cat == 5 ? 417 : 166 + kCatOffSig[cat];
  const int abs_base = cat == 5 ? 426 : 227 + kCatOffAbs[cat];
  int last = max_coeff - 1;
  while (last > 0 && lvl[static_cast<uint64_t>(last)] == 0) --last;
  for (int i = 0; i < max_coeff - 1; ++i) {
    const int sc = cat == 3 ? imin(i, 2) : cat == 5 ? kSig8x8Ctx[i] : i;
    const bool sig = lvl[static_cast<uint64_t>(i)] != 0;
    cw_.decision(w, sig_base + sc, sig ? 1 : 0);
    if (!sig) continue;
    const int lc = cat == 3 ? imin(i, 2) : cat == 5 ? kLast8x8Ctx[i] : i;
    cw_.decision(w, last_base + lc, i == last ? 1 : 0);
    if (i == last) break;
  }
  int gt1 = 0, eq1 = 0;
  for (int k = last; k >= 0; --k) {
    const int level = lvl[static_cast<uint64_t>(k)];
    if (level == 0) continue;
    const int a = iabs(level);
    const int ctx0 = abs_base + (gt1 != 0 ? 0 : imin(4, 1 + eq1));
    if (a == 1) {
      cw_.decision(w, ctx0, 0);
      ++eq1;
    } else {
      cw_.decision(w, ctx0, 1);
      const int ctxn = abs_base + 5 + imin(4 - (cat == 3 ? 1 : 0), gt1);
      for (int j = 2; j < imin(a, 15); ++j) cw_.decision(w, ctxn, 1);
      if (a < 15) {
        cw_.decision(w, ctxn, 0);
      } else {   // (coeff_abs_level_minus1 - 14 as 0th-order Exp-Golomb, bypassed)
        uint32_t v = static_cast<uint32_t>(a - 15);
        int e = 0;
        while (v >= (1u << e)) {
          cw_.bypass(w, 1);
          v -= 1u << e;
          ++e;
        }
        cw_.bypass(w, 0);
        while (e-- > 0) cw_.bypass(w, static_cast<int>((v >> e) & 1));
      }
      ++gt1;
    }
    cw_.bypass(w, level < 0 ? 1 : 0);
  }
}

void PictureCoder::write_mb_cabac(int mbx, int mby, int qp_delta) {
  BitWriter& w = bw_;
  const H264EncPicture& p = *pic_;
  Mb& m = m_;
  const MbData* a = mb_at(mbx - 1, mby);
  const MbData* b = mb_at(mbx, mby - 1);
  if (p_slice() || b_slice()) {
    auto cond = [](const MbData* n) { return n && !n->skip; };
    cw_.decision(w, (b_slice() ? 24 : 11) + cond(a) + cond(b), 0);   // mb_skip_flag
  }
  cabac_mb_type(mbx, mby);
  const bool intra = m.kind == Kind::I16 || m.kind == Kind::I8;
  auto t8_cond = [](const MbData* n) { return n && n->t8x8 ? 1 : 0; };
  if (m.kind == Kind::I8) {
    cw_.decision(w, 399 + t8_cond(a) + t8_cond(b), 1);   // transform_size_8x8_flag
    for (int i = 0; i < 4; ++i) {
      const bool same = m.i8_mode[i] == m.i8_pred[i];
      cw_.decision(w, 68, same ? 1 : 0);
      if (same) continue;
      const int rem = m.i8_mode[i] < m.i8_pred[i] ? m.i8_mode[i] : m.i8_mode[i] - 1;
      for (int bit = 0; bit < 3; ++bit) cw_.decision(w, 69, (rem >> bit) & 1);
    }
  }
  if (intra) {
    auto cond = [](const MbData* n) { return n && n->intra && n->chroma_mode != 0 ? 1 : 0; };
    const int mode = m.chroma_mode;
    cw_.decision(w, 64 + cond(a) + cond(b), mode != 0 ? 1 : 0);
    if (mode != 0) {
      cw_.decision(w, 64 + 3, mode != 1 ? 1 : 0);
      if (mode != 1) cw_.decision(w, 64 + 3, mode == 3 ? 1 : 0);
    }
  } else if (m.kind == Kind::Inter) {
    for (int l = 0; l < 2; ++l)
      if ((m.pred_flags & (1 << l)) && p.list[l].size() > 1) cabac_ref_idx(mbx, mby, l, m.ref[l]);
    for (int l = 0; l < 2; ++l) {
      if (!(m.pred_flags & (1 << l))) continue;
      cabac_mvd(mbx, mby, l, 0, m.mv[l].x - m.mvp[l].x);
      cabac_mvd(mbx, mby, l, 1, m.mv[l].y - m.mvp[l].y);
    }
  }
  const int cbp = m.cbp_luma | (m.cbp_chroma << 4);
  if (m.kind != Kind::I16) {
    cabac_cbp(mbx, mby);
    if (m.kind != Kind::I8 && m.cbp_luma != 0 && p.transform_8x8) cw_.decision(w, 399 + t8_cond(a) + t8_cond(b), m.t8x8 ? 1 : 0);
  }
  if (m.kind != Kind::I16 && cbp == 0) return;
  cabac_qp_delta(qp_delta);
  // residual (7.3.5.3)
  if (m.kind == Kind::I16) {
    const bool dc = count_nonzero(m.dc) != 0;
    if (cabac_cbf(mbx, mby, 0, 0, 0, 0, dc)) {
      cabac_residual(0, m.dc, 16);
      m.cbf_dc |= 1;
    }
  }
  for (int b8 = 0; b8 < 4; ++b8) {
    if (!((m.cbp_luma >> b8) & 1)) continue;
    if (m.t8x8) {
      cabac_residual(5, m.l8[b8], 64);   // (no coded_block_flag for an 8x8 block of 4:2:0)
      const int n = count_nonzero(m.l8[b8]);
      for (int s = 0; s < 4; ++s) m.nz[((b8 >> 1) * 2 + (s >> 1)) * 4 + (b8 & 1) * 2 + (s & 1)] = static_cast<uint8_t>(n);
      continue;
    }
    for (int s = 0; s < 4; ++s) {
      const int bx = (b8 & 1) * 2 + (s & 1), by = (b8 >> 1) * 2 + (s >> 1);
      const int blk = by * 4 + bx;
      const bool i16 = m.kind == Kind::I16;
      const ArrayView<const int> lv = i16 ? ArrayView<const int>(m.ac[blk]).subview(1) : ArrayView<const int>(m.ac[blk]);
      const int n = count_nonzero(lv);
      if (!cabac_cbf(mbx, mby, i16 ? 1 : 2, bx, by, 0, n != 0)) continue;
      cabac_residual(i16 ? 1 : 2, lv, i16 ? 15 : 16);
      m.nz[blk] = static_cast<uint8_t>(n);
    }
  }
  if (m.cbp_chroma != 0) {
    for (int c = 0; c < 2; ++c) {
      if (!cabac_cbf(mbx, mby, 3, 0, 0, c + 1, count_nonzero(m.cdc[c]) != 0)) continue;
      cabac_residual(3, m.cdc[c], 4);
      m.cbf_dc = static_cast<uint8_t>(m.cbf_dc | (2 << c));
    }
    if (m.cbp_chroma == 2)
      for (int c = 0; c < 2; ++c)
        for (int s = 0; s < 4; ++s) {
          const ArrayView<const int> lv = ArrayView<const int>(m.cac[c][s]).subview(1);
          const int n = count_nonzero(lv);
          if (!cabac_cbf(mbx, mby, 4, s & 1, s >> 1, c + 1, n != 0)) continue;
          cabac_residual(4, lv, 15);
          m.nz[16 + c * 4 + s] = static_cast<uint8_t>(n);
        }
  }
}

}  // namespace h264
}  // namespace hardware
