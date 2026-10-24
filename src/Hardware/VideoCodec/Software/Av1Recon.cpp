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
#include "Hardware/VideoCodec/Software/Av1Itx.hpp"
#include "Hardware/VideoCodec/Software/Av1Mc.hpp"

// Port of dav1d src/recon_tmpl.c (coefficient decoding and block reconstruction), src/qm.c (matrix selection) and src/scan.c usage
// (BSD-2-Clause, Copyright (c) VideoLAN and dav1d authors). Single threaded, single pass: dav1d's frame_thread paths are gone.
// The Core members declared in Av1Core.hpp (recon_b_intra, recon_b_inter, the palette readers, backup_ipred_edge) are at the end.

namespace hardware {
namespace av1 {

namespace {

#include "Hardware/VideoCodec/Software/Av1Tables.inc"
#include "Hardware/VideoCodec/Software/Av1Scan.inc"
#include "Hardware/VideoCodec/Software/Av1Qm.inc"

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline unsigned umin(unsigned a, unsigned b) { return a < b ? a : b; }
inline int ulog2(unsigned v) { return 31 ^ __builtin_clz(v); }

// ---- scan tables ---------------------------------------------------------------------------------------------------------------------

// dav1d_scans[tx]
ArrayView<const uint16_t> scan_for(int tx) {
  switch (tx) {
    case TX_4X4: return ArrayView<const uint16_t>(k_scan_4x4);
    case TX_8X8: return ArrayView<const uint16_t>(k_scan_8x8);
    case TX_16X16: return ArrayView<const uint16_t>(k_scan_16x16);
    case TX_32X32:
    case TX_64X64:
    case RTX_32X64:
    case RTX_64X32: return ArrayView<const uint16_t>(k_scan_32x32);
    case RTX_4X8: return ArrayView<const uint16_t>(k_scan_4x8);
    case RTX_8X4: return ArrayView<const uint16_t>(k_scan_8x4);
    case RTX_8X16: return ArrayView<const uint16_t>(k_scan_8x16);
    case RTX_16X8: return ArrayView<const uint16_t>(k_scan_16x8);
    case RTX_16X32:
    case RTX_16X64: return ArrayView<const uint16_t>(k_scan_16x32);
    case RTX_32X16:
    case RTX_64X16: return ArrayView<const uint16_t>(k_scan_32x16);
    case RTX_4X16: return ArrayView<const uint16_t>(k_scan_4x16);
    case RTX_16X4: return ArrayView<const uint16_t>(k_scan_16x4);
    case RTX_8X32: return ArrayView<const uint16_t>(k_scan_8x32);
    case RTX_32X8: return ArrayView<const uint16_t>(k_scan_32x8);
    default: return ArrayView<const uint16_t>();
  }
}

// ---- contexts ------------------------------------------------------------------------------------------------------------------------

// The bytes a[0 .. n) differ from the "no coefficients" marker 0x40 (dav1d reads them as one integer and compares).
bool any_not_marker(ArrayView<const uint8_t> a, unsigned n) {
  for (uint8_t v : a.first(n))
    if (v != 0x40) return true;
  return false;
}

// The OR of the bytes a[0 .. n).
unsigned or_bytes(ArrayView<const uint8_t> a, unsigned n) {
  unsigned r = 0;
  for (uint8_t v : a.first(n)) r |= v;
  return r;
}

// The sum of the sign bits (top two bits: 0 = negative, 1 = none, 2 = positive) of a[0 .. n).
unsigned sum_sign_bits(ArrayView<const uint8_t> a, unsigned n) {
  unsigned r = 0;
  for (uint8_t v : a.first(n)) r += v >> 6;
  return r;
}

unsigned get_skip_ctx(const TxfmInfo& t_dim, BlockSize bs, ArrayView<const uint8_t> a, ArrayView<const uint8_t> l, bool chroma,
                      PixelLayout layout) {
  const uint8_t(&b_dim)[4] = k_block_dimensions[bs];
  if (chroma) {
    const int ss_ver = layout == PIXEL_LAYOUT_I420;
    const int ss_hor = layout != PIXEL_LAYOUT_I444;
    const int not_one_blk = b_dim[2] - (!!b_dim[2] && ss_hor) > t_dim.lw || b_dim[3] - (!!b_dim[3] && ss_ver) > t_dim.lh;
    const unsigned ca = any_not_marker(a, 1u << t_dim.lw);
    const unsigned cl = any_not_marker(l, 1u << t_dim.lh);
    return 7 + not_one_blk * 3 + ca + cl;
  }
  if (b_dim[2] == t_dim.lw && b_dim[3] == t_dim.lh) return 0;
  const unsigned la = or_bytes(a, 1u << t_dim.lw);
  const unsigned ll = or_bytes(l, 1u << t_dim.lh);
  return k_skip_ctx[umin(la & 0x3F, 4)][umin(ll & 0x3F, 4)];
}

unsigned get_dc_sign_ctx(const TxfmInfo& t_dim, ArrayView<const uint8_t> a, ArrayView<const uint8_t> l) {
  const int s = static_cast<int>(sum_sign_bits(a, t_dim.w) + sum_sign_bits(l, t_dim.h)) - static_cast<int>(t_dim.w) - static_cast<int>(t_dim.h);
  return (s != 0) + (s > 0);
}

unsigned get_lo_ctx(ArrayView<const uint8_t> levels, unsigned idx, int tx_class, unsigned& hi_mag, const uint8_t (*ctx_offsets)[5], unsigned x,
                    unsigned y, unsigned stride) {
  unsigned mag = levels[idx + 0 * stride + 1] + levels[idx + 1 * stride + 0];
  unsigned offset;
  if (tx_class == TX_CLASS_2D) {
    mag += levels[idx + 1 * stride + 1];
    hi_mag = mag;
    mag += levels[idx + 0 * stride + 2] + levels[idx + 2 * stride + 0];
    offset = ctx_offsets[umin(y, 4)][umin(x, 4)];
  } else {
    mag += levels[idx + 0 * stride + 2];
    hi_mag = mag;
    mag += levels[idx + 0 * stride + 3] + levels[idx + 0 * stride + 4];
    offset = 26 + (y > 1 ? 10 : y * 5);
  }
  return offset + (mag > 512 ? 4 : (mag + 64) >> 7);
}

// get_uv_inter_txtp
int get_uv_inter_txtp(const TxfmInfo& uvt_dim, int ytxtp) {
  if (uvt_dim.max == TX_32X32) return ytxtp == IDTX ? IDTX : DCT_DCT;
  if (uvt_dim.min == TX_16X16 && ((1 << ytxtp) & ((1 << H_FLIPADST) | (1 << V_FLIPADST) | (1 << H_ADST) | (1 << V_ADST)))) return DCT_DCT;
  return ytxtp;
}

// ---- coefficient decoding -------------------------------------------------------------------------------------------------------------

// The token pass of decode_coefs (the DECODE_COEFS_CLASS macro): reads the base and range tokens from the end of block backwards and leaves
// the tokens in cf (as token << 11 plus the link to the next non-zero coefficient in the low bits). Returns the token of the DC coefficient
// and sets `rc_out` to the position of the first non-zero AC coefficient in the chain (0 if none).
unsigned decode_tokens(Msac& msac, CdfCoefContext& cc, int chroma, int tx, const TxfmInfo& t_dim, int tx_class, int eob, ArrayView<int32_t> cf,
                       ArrayView<uint8_t> levels, unsigned& rc_out) {
  const int slw = imin(t_dim.lw, TX_32X32), slh = imin(t_dim.lh, TX_32X32);
  const int tx2dszctx = slw + slh;
  auto& eob_cdf = cc.eob_base_tok[t_dim.ctx][chroma];
  auto& hi_cdf = cc.br_tok[imin(t_dim.ctx, 3)][chroma];
  auto& lo_cdf = cc.base_tok[t_dim.ctx][chroma];
  unsigned dc_tok;
  unsigned rc;

  if (eob == 0) {  // dc-only
    const int tok_br = static_cast<int>(msac.symbol_adapt4(ArrayView<uint16_t>(eob_cdf[0]), 2));
    dc_tok = 1 + tok_br;
    if (tok_br == 2) dc_tok = msac.hi_tok(ArrayView<uint16_t>(hi_cdf[0]));
    rc_out = 0;
    return dc_tok;
  }

  ArrayView<const uint16_t> scan;
  const uint8_t(*lo_ctx_offsets)[5] = nullptr;
  unsigned stride, shift, shift2, mask;
  const bool is2d = tx_class == TX_CLASS_2D;
  if (tx_class == TX_CLASS_2D) {
    const unsigned nonsquare_tx = tx >= RTX_4X8;
    lo_ctx_offsets = k_lo_ctx_offsets[nonsquare_tx + (tx & nonsquare_tx)];
    scan = scan_for(tx);
    stride = 4u << slh;
    shift = slh + 2;
    shift2 = 0;
    mask = (4u << slh) - 1;
    levels.first(stride * ((4u << slw) + 2)).fill(0);
  } else if (tx_class == TX_CLASS_H) {
    stride = 16;
    shift = slh + 2;
    shift2 = 0;
    mask = (4u << slh) - 1;
    levels.first(stride * ((4u << slh) + 2)).fill(0);
  } else {
    stride = 16;
    shift = slw + 2;
    shift2 = slh + 2;
    mask = (4u << slw) - 1;
    levels.first(stride * ((4u << slw) + 2)).fill(0);
  }

  unsigned ctx = 1 + (eob > (2 << tx2dszctx)) + (eob > (4 << tx2dszctx));
  const int eob_tok = static_cast<int>(msac.symbol_adapt4(ArrayView<uint16_t>(eob_cdf[ctx]), 2));
  int tok = eob_tok + 1;
  int level_tok = tok * 0x41;
  unsigned mag = 0;
  unsigned x, y;
  if (is2d) {
    rc = scan[static_cast<uint64_t>(eob)];
    x = rc >> shift;
    y = rc & mask;
  } else if (tx_class == TX_CLASS_H) {
    x = eob & mask;
    y = static_cast<unsigned>(eob) >> shift;
    rc = eob;
  } else {
    x = eob & mask;
    y = static_cast<unsigned>(eob) >> shift;
    rc = (x << shift2) | y;
  }
  if (eob_tok == 2) {
    ctx = (is2d ? (x | y) > 1 : y != 0) ? 14 : 7;
    tok = static_cast<int>(msac.hi_tok(ArrayView<uint16_t>(hi_cdf[ctx])));
    level_tok = tok + (3 << 6);
  }
  cf[rc] = tok << 11;
  unsigned level_idx = is2d ? rc : x * stride + y;
  levels[level_idx] = static_cast<uint8_t>(level_tok);
  for (int i = eob - 1; i > 0; i--) {  // ac
    unsigned rc_i;
    if (is2d) {
      rc_i = scan[static_cast<uint64_t>(i)];
      x = rc_i >> shift;
      y = rc_i & mask;
    } else if (tx_class == TX_CLASS_H) {
      x = i & mask;
      y = static_cast<unsigned>(i) >> shift;
      rc_i = i;
    } else {
      x = i & mask;
      y = static_cast<unsigned>(i) >> shift;
      rc_i = (x << shift2) | y;
    }
    level_idx = is2d ? rc_i : x * stride + y;
    ctx = get_lo_ctx(levels, level_idx, tx_class, mag, lo_ctx_offsets, x, y, stride);
    if (is2d) y |= x;
    tok = static_cast<int>(msac.symbol_adapt4(ArrayView<uint16_t>(lo_cdf[ctx]), 3));
    if (tok == 3) {
      mag &= 63;
      ctx = (y > static_cast<unsigned>(is2d) ? 14 : 7) + (mag > 12 ? 6 : (mag + 1) >> 1);
      tok = static_cast<int>(msac.hi_tok(ArrayView<uint16_t>(hi_cdf[ctx])));
      levels[level_idx] = static_cast<uint8_t>(tok + (3 << 6));
      cf[rc_i] = static_cast<int32_t>((static_cast<unsigned>(tok) << 11) | rc);
      rc = rc_i;
    } else {
      // 0x1 for tok, 0x7ff as bitmask for rc, 0x41 for level_tok
      unsigned v = static_cast<unsigned>(tok) * 0x17ff41u;
      levels[level_idx] = static_cast<uint8_t>(v);
      // tok ? (tok << 11) | rc : 0
      v = (v >> 9) & (rc + ~0x7ffu);
      if (v) rc = rc_i;
      cf[rc_i] = static_cast<int32_t>(v);
    }
  }
  // dc
  ctx = is2d ? 0 : get_lo_ctx(levels, 0, tx_class, mag, lo_ctx_offsets, 0, 0, stride);
  dc_tok = msac.symbol_adapt4(ArrayView<uint16_t>(lo_cdf[ctx]), 3);
  if (dc_tok == 3) {
    if (is2d) mag = levels[0 * stride + 1] + levels[1 * stride + 0] + levels[1 * stride + 1];
    mag &= 63;
    ctx = mag > 12 ? 6 : (mag + 1) >> 1;
    dc_tok = msac.hi_tok(ArrayView<uint16_t>(hi_cdf[ctx]));
  }
  rc_out = rc;
  return dc_tok;
}

// dav1d decode_coefs. `a` / `l` start at the entries of the transform block in the above / left coefficient contexts. Returns the end of
// block position (the value inv_txfm_add wants), or -1 if the block has no coefficients (nothing to add). `txtp` is the transform type
// (input for inter chroma: the type of the luma block), `res_ctx` the value for the coefficient contexts.
int decode_coefs(TaskContext& t, ArrayView<uint8_t> a, ArrayView<uint8_t> l, int tx, BlockSize bs, const Av1Block& b, bool intra, int plane,
                 ArrayView<int32_t> cf, int& txtp, uint8_t& res_ctx) {
  TileState& ts = *t.ts;
  Msac& msac = ts.msac;
  const bool chroma = plane != 0;
  const FrameContext& f = *t.f;
  const int lossless = f.frame_hdr->segmentation.lossless[b.seg_id];
  const TxfmInfo& t_dim = k_txfm_dimensions[tx];

  // does this block have any non-zero coefficients
  const unsigned sctx = get_skip_ctx(t_dim, bs, a, l, chroma, f.layout);
  const int all_skip = static_cast<int>(msac.bool_adapt(ArrayView<uint16_t>(ts.cdf.coef.skip[t_dim.ctx][sctx])));
  if (all_skip) {
    res_ctx = 0x40;
    txtp = lossless * WHT_WHT; /* lossless ? WHT_WHT : DCT_DCT */
    return -1;
  }

  // transform type (chroma: derived, luma: explicitly coded)
  if (lossless) {
    txtp = WHT_WHT;
  } else if (t_dim.max + intra >= TX_64X64) {
    txtp = DCT_DCT;
  } else if (chroma) {
    // inferred from either the luma txtp (inter) or a LUT (intra)
    txtp = intra ? k_txtp_from_uvmode[b.uv_mode] : get_uv_inter_txtp(t_dim, txtp);
  } else if (!f.frame_hdr->segmentation.qidx[b.seg_id]) {
    // In libaom, lossless is checked by a literal qidx == 0, but not all such blocks are actually lossless. The remainder gets an
    // implicit transform type (for luma)
    txtp = DCT_DCT;
  } else {
    unsigned idx;
    if (intra) {
      const int y_mode_nofilt = b.y_mode == FILTER_PRED ? k_filter_mode_to_y_mode[b.y_angle] : b.y_mode;
      if (f.frame_hdr->reduced_txtp_set || t_dim.min == TX_16X16) {
        idx = msac.symbol_adapt8(ArrayView<uint16_t>(ts.cdf.m.txtp_intra2[t_dim.min][y_mode_nofilt]), 4);
        txtp = k_tx_types_per_set[idx + 0];
      } else {
        idx = msac.symbol_adapt8(ArrayView<uint16_t>(ts.cdf.m.txtp_intra1[t_dim.min][y_mode_nofilt]), 6);
        txtp = k_tx_types_per_set[idx + 5];
      }
    } else {
      if (f.frame_hdr->reduced_txtp_set || t_dim.max == TX_32X32) {
        idx = msac.bool_adapt(ArrayView<uint16_t>(ts.cdf.m.txtp_inter3[t_dim.min]));
        txtp = static_cast<int>((idx - 1) & IDTX); /* idx ? DCT_DCT : IDTX */
      } else if (t_dim.min == TX_16X16) {
        idx = msac.symbol_adapt16(ArrayView<uint16_t>(ts.cdf.m.txtp_inter2), 11);
        txtp = k_tx_types_per_set[idx + 12];
      } else {
        idx = msac.symbol_adapt16(ArrayView<uint16_t>(ts.cdf.m.txtp_inter1[t_dim.min]), 15);
        txtp = k_tx_types_per_set[idx + 24];
      }
    }
  }

  // find end-of-block (eob)
  int eob = 0;
  const int slw = imin(t_dim.lw, TX_32X32), slh = imin(t_dim.lh, TX_32X32);
  const int tx2dszctx = slw + slh;
  const int tx_class = k_tx_type_class[txtp];
  const int is_1d = tx_class != TX_CLASS_2D;
  CdfCoefContext& cc = ts.cdf.coef;
  switch (tx2dszctx) {
    case 0: eob = static_cast<int>(msac.symbol_adapt8(ArrayView<uint16_t>(cc.eob_bin_16[chroma][is_1d]), 4 + 0)); break;
    case 1: eob = static_cast<int>(msac.symbol_adapt8(ArrayView<uint16_t>(cc.eob_bin_32[chroma][is_1d]), 4 + 1)); break;
    case 2: eob = static_cast<int>(msac.symbol_adapt8(ArrayView<uint16_t>(cc.eob_bin_64[chroma][is_1d]), 4 + 2)); break;
    case 3: eob = static_cast<int>(msac.symbol_adapt8(ArrayView<uint16_t>(cc.eob_bin_128[chroma][is_1d]), 4 + 3)); break;
    case 4: eob = static_cast<int>(msac.symbol_adapt16(ArrayView<uint16_t>(cc.eob_bin_256[chroma][is_1d]), 4 + 4)); break;
    case 5: eob = static_cast<int>(msac.symbol_adapt16(ArrayView<uint16_t>(cc.eob_bin_512[chroma]), 4 + 5)); break;
    default: eob = static_cast<int>(msac.symbol_adapt16(ArrayView<uint16_t>(cc.eob_bin_1024[chroma]), 4 + 6)); break;
  }
  if (eob > 1) {
    const int eob_bin = eob - 2;
    const int eob_hi_bit = static_cast<int>(msac.bool_adapt(ArrayView<uint16_t>(cc.eob_hi_bit[t_dim.ctx][chroma][eob_bin])));
    eob = ((eob_hi_bit | 2) << eob_bin) | static_cast<int>(msac.bools(static_cast<unsigned>(eob_bin)));
  }

  // base tokens
  unsigned rc;
  unsigned dc_tok = decode_tokens(msac, cc, chroma, tx, t_dim, tx_class, eob, cf, ArrayView<uint8_t>(t.scratch.levels), rc);

  // residual and sign
  const uint16_t(&dq_tbl)[2] = ts.dq->q[b.seg_id][plane];
  const ArrayView<const uint8_t> qm_tbl = txtp < IDTX ? f.qm[tx][plane] : ArrayView<const uint8_t>();
  const bool have_qm = !qm_tbl.empty();
  const int dq_shift = imax(0, t_dim.ctx - 2);
  const unsigned cf_max = ~(~127U << f.cur->bpc());
  unsigned cul_level, dc_sign_level;

  if (!dc_tok) {
    cul_level = 0;
    dc_sign_level = 1 << 6;
  } else {
    const unsigned dc_sign_ctx = get_dc_sign_ctx(t_dim, a, l);
    const int dc_sign = static_cast<int>(msac.bool_adapt(ArrayView<uint16_t>(cc.dc_sign[chroma][dc_sign_ctx])));
    int dc_dq = dq_tbl[0];
    dc_sign_level = (dc_sign - 1) & (2 << 6);

    if (have_qm) {
      dc_dq = static_cast<int>((static_cast<unsigned>(dc_dq) * qm_tbl[0] + 16) >> 5);
      if (dc_tok == 15) {
        dc_tok = msac.golomb() + 15;
        dc_tok &= 0xfffff;
        dc_dq = static_cast<int>((static_cast<unsigned>(dc_dq) * dc_tok) & 0xffffff);
      } else {
        dc_dq = static_cast<int>(static_cast<unsigned>(dc_dq) * dc_tok);
      }
      cul_level = dc_tok;
      dc_dq >>= dq_shift;
      dc_dq = static_cast<int>(umin(static_cast<unsigned>(dc_dq), cf_max + dc_sign));
      cf[0] = dc_sign ? -dc_dq : dc_dq;
    } else {
      // non-qmatrix is the common case
      if (dc_tok == 15) {
        dc_tok = msac.golomb() + 15;
        dc_tok &= 0xfffff;
        dc_dq = static_cast<int>(((static_cast<unsigned>(dc_dq) * dc_tok) & 0xffffff) >> dq_shift);
        dc_dq = static_cast<int>(umin(static_cast<unsigned>(dc_dq), cf_max + dc_sign));
      } else {
        dc_dq = static_cast<int>((static_cast<unsigned>(dc_dq) * dc_tok) >> dq_shift);
      }
      cul_level = dc_tok;
      cf[0] = dc_sign ? -dc_dq : dc_dq;
    }
  }

  if (rc) {
    const unsigned ac_dq = dq_tbl[1];
    do {
      const int sign = static_cast<int>(msac.bool_equi());
      const unsigned rc_tok = static_cast<unsigned>(cf[rc]);
      unsigned tok;
      if (have_qm) {
        unsigned dq = (ac_dq * qm_tbl[rc] + 16) >> 5;
        if (rc_tok >= (15 << 11)) {
          tok = msac.golomb() + 15;
          tok &= 0xfffff;
          dq = (dq * tok) & 0xffffff;
        } else {
          tok = rc_tok >> 11;
          dq *= tok;
        }
        cul_level += tok;
        dq >>= dq_shift;
        const int dq_sat = static_cast<int>(umin(dq, cf_max + sign));
        cf[rc] = sign ? -dq_sat : dq_sat;
      } else {
        int dq;
        if (rc_tok >= (15 << 11)) {
          // residual: coefficient parsing, see 5.11.39
          tok = msac.golomb() + 15;
          tok &= 0xfffff;
          // dequant, see 7.12.3
          dq = static_cast<int>(((ac_dq * tok) & 0xffffff) >> dq_shift);
          dq = static_cast<int>(umin(static_cast<unsigned>(dq), cf_max + sign));
        } else {
          // cannot exceed cf_max, so we can avoid the clipping
          tok = rc_tok >> 11;
          dq = static_cast<int>((ac_dq * tok) >> dq_shift);
        }
        cul_level += tok;
        cf[rc] = sign ? -dq : dq;
      }
      rc = rc_tok & 0x3ff;  // next non-zero rc, zero if eob
    } while (rc);
  }

  // context
  res_ctx = static_cast<uint8_t>(umin(cul_level, 63) | dc_sign_level);
  return eob;
}

// ---- small helpers -------------------------------------------------------------------------------------------------------------------

// Sets n entries of a coefficient context from `start` (dav1d_memset_pow2 / dav1d_memset_likely_pow2).
void set_ctx(uint8_t (&ctx)[32], int start, int n, uint8_t value) {
  if (n <= 0) return;
  ArrayView<uint8_t>(ctx).subview(static_cast<uint64_t>(start), static_cast<uint64_t>(n)).fill(value);
}

ArrayView<uint8_t> ctx_view(uint8_t (&ctx)[32], int start) { return ArrayView<uint8_t>(ctx).subview(static_cast<uint64_t>(start)); }

// The row of samples at (x, y) of the plane, n long.
ArrayView<Pixel> pixel_row(PixelView v, int x, int y, int n) {
  const int64_t index = v.origin + static_cast<int64_t>(y) * v.stride + x;
  if (index < 0) bounds_violation("pixel_row", 0, v.plane.size());
  return v.plane.subview(static_cast<uint64_t>(index), static_cast<uint64_t>(n));
}

// dav1d sm_flag / sm_uv_flag
int sm_flag(const BlockContext& bc, int idx) {
  if (!bc.intra[idx]) return 0;
  return smooth_edge_flag(bc.mode[idx]);
}
int sm_uv_flag(const BlockContext& bc, int idx) { return smooth_edge_flag(bc.uvmode[idx]); }

// f->ipred_edge[plane] + sb128w * 128 * (sby - 1) for the first row of a superblock, else an empty view.
ArrayView<const Pixel> top_sb_edge_of(const FrameContext& f, int plane, int by) {
  if (by & (f.sb_step - 1)) return ArrayView<const Pixel>();
  const int sby = by >> f.sb_shift;
  if (sby <= 0) return ArrayView<const Pixel>();
  return f.ipred_edge[plane].view().subview(static_cast<uint64_t>(f.sb128w) * 128u * static_cast<uint64_t>(sby - 1));
}

// ---- inverse transform of one block --------------------------------------------------------------------------------------------------

// Adds the residual of one decoded transform block. False for a transform size / type the kernel rejects (cannot happen for a valid parse).
bool add_residual(TaskContext& t, PixelView dst, int tx, int txtp, int eob) {
  const FrameContext& f = *t.f;
  if (inv_txfm_add(dst, ArrayView<int32_t>(t.cf), tx, txtp, eob, f.bitdepth_max)) return true;
  ArrayView<int32_t>(t.cf).fill(0);
  return false;
}

// ---- transform tree of inter blocks --------------------------------------------------------------------------------------------------

// dav1d read_coef_tree: dst is the window at the top left sample of the transform block at (t.bx, t.by).
bool read_coef_tree(TaskContext& t, BlockSize bs, const Av1Block& b, int ytx, int depth, const uint16_t (&tx_split)[2], int x_off, int y_off,
                    PixelView dst) {
  FrameContext& f = *t.f;
  const TxfmInfo& t_dim = k_txfm_dimensions[ytx];
  const int txw = t_dim.w, txh = t_dim.h;

  /* y_off can be larger than 3 since lossless blocks use TX_4X4 but can't be splitted. Avoids an undefined left shift. */
  if (depth < 2 && tx_split[depth] && y_off * 4 + x_off < 16 && ((tx_split[depth] >> (y_off * 4 + x_off)) & 1)) {
    const int sub = t_dim.sub;
    const TxfmInfo& sub_t_dim = k_txfm_dimensions[sub];
    const int txsw = sub_t_dim.w, txsh = sub_t_dim.h;

    if (!read_coef_tree(t, bs, b, sub, depth + 1, tx_split, x_off * 2 + 0, y_off * 2 + 0, dst)) return false;
    t.bx += txsw;
    if (txw >= txh && t.bx < f.bw)
      if (!read_coef_tree(t, bs, b, sub, depth + 1, tx_split, x_off * 2 + 1, y_off * 2 + 0, dst.offset(4 * txsw, 0))) return false;
    t.bx -= txsw;
    t.by += txsh;
    if (txh >= txw && t.by < f.bh) {
      dst = dst.offset(0, 4 * txsh);
      if (!read_coef_tree(t, bs, b, sub, depth + 1, tx_split, x_off * 2 + 0, y_off * 2 + 1, dst)) return false;
      t.bx += txsw;
      if (txw >= txh && t.bx < f.bw)
        if (!read_coef_tree(t, bs, b, sub, depth + 1, tx_split, x_off * 2 + 1, y_off * 2 + 1, dst.offset(4 * txsw, 0))) return false;
      t.bx -= txsw;
    }
    t.by -= txsh;
  } else {
    const int bx4 = t.bx & 31, by4 = t.by & 31;
    int txtp = DCT_DCT;
    uint8_t cf_ctx = 0;
    const int eob = decode_coefs(t, ctx_view(t.a->lcoef, bx4), ctx_view(t.l.lcoef, by4), ytx, bs, b, false, 0, ArrayView<int32_t>(t.cf), txtp, cf_ctx);
    set_ctx(t.a->lcoef, bx4, imin(txw, f.bw - t.bx), cf_ctx);
    set_ctx(t.l.lcoef, by4, imin(txh, f.bh - t.by), cf_ctx);
    ArrayView<uint8_t> txtp_map = ArrayView<uint8_t>(t.scratch.txtp_map);
    for (int y = 0; y < txh; y++) txtp_map.subview(static_cast<uint64_t>((by4 + y) * 32 + bx4), static_cast<uint64_t>(1 << t_dim.lw)).fill(static_cast<uint8_t>(txtp));
    if (eob >= 0) {
      if (!add_residual(t, dst, ytx, txtp, eob)) return false;
    }
  }
  return true;
}

// ---- motion compensation helpers -----------------------------------------------------------------------------------------------------

// dav1d mc(): predicts the bw4 x bh4 (4x4 units of luma) block at (bx, by) (4x4 units) of plane `pl` from `refp` into dst (pixels) or, if
// `tmp` is not empty, into the 16-bit intermediate buffer tmp.
bool mc_block(TaskContext& t, PixelView dst, ArrayView<int16_t> tmp, int bw4, int bh4, int bx, int by, int pl, Mv mv, const Picture& refp,
              int refidx, Filter2d filter_2d) {
  FrameContext& f = *t.f;
  const Picture& cur = *f.cur;
  if (refp.layout() != cur.layout() || refp.bpc() != cur.bpc()) return false;
  const int ss_ver = pl && f.layout == PIXEL_LAYOUT_I420;
  const int ss_hor = pl && f.layout != PIXEL_LAYOUT_I444;
  const int h_mul = 4 >> ss_hor, v_mul = 4 >> ss_ver;
  const int mvx = mv.x, mvy = mv.y;
  const int mx = mvx & (15 >> !ss_hor), my = mvy & (15 >> !ss_ver);
  const PixelView ref_plane = refp.plane(pl);
  const int bdm = f.bitdepth_max;

  if (refp.width() == cur.width() && refp.height() == cur.height()) {
    const int dx = bx * h_mul + (mvx >> (3 + ss_hor));
    const int dy = by * v_mul + (mvy >> (3 + ss_ver));
    int w, h;
    if (&refp != &cur) {  // i.e. not for intrabc
      w = (cur.width() + ss_hor) >> ss_hor;
      h = (cur.height() + ss_ver) >> ss_ver;
    } else {
      w = f.bw * 4 >> ss_hor;
      h = f.bh * 4 >> ss_ver;
    }
    PixelView ref = ref_plane;
    if (dx < !!mx * 3 || dy < !!my * 3 || dx + bw4 * h_mul + !!mx * 4 > w || dy + bh4 * v_mul + !!my * 4 > h) {
      const PixelView emu{ArrayView<Pixel>(t.scratch.emu_edge), 0, 192};
      emu_edge(bw4 * h_mul + !!mx * 7, bh4 * v_mul + !!my * 7, w, h, dx - !!mx * 3, dy - !!my * 3, emu, ref_plane);
      ref = emu.offset(!!mx * 3, !!my * 3);
    } else {
      ref = ref_plane.offset(dx, dy);
    }
    if (tmp.empty())
      mc(filter_2d, dst, ref, bw4 * h_mul, bh4 * v_mul, mx << !ss_hor, my << !ss_ver, bdm);
    else
      mct(filter_2d, tmp, ref, bw4 * h_mul, bh4 * v_mul, mx << !ss_hor, my << !ss_ver, bdm);
  } else {
    if (refidx < 0 || refidx >= 7) return false;
    const int orig_pos_y = (by * v_mul << 4) + mvy * (1 << !ss_ver);
    const int orig_pos_x = (bx * h_mul << 4) + mvx * (1 << !ss_hor);
    auto scale_mv = [](int val, int scale) {
      const int64_t v = static_cast<int64_t>(val) * scale + static_cast<int64_t>(scale - 0x4000) * 8;
      const int64_t mag = v < 0 ? -v : v;
      const int r = static_cast<int>((mag + 128) >> 8);
      return (v < 0 ? -r : r) + 32;
    };
    const ScalableMotionParams& sx = f.svc[refidx][0];
    const ScalableMotionParams& sy = f.svc[refidx][1];
    const int pos_x = scale_mv(orig_pos_x, sx.scale);
    const int pos_y = scale_mv(orig_pos_y, sy.scale);
    const int left = pos_x >> 10;
    const int top = pos_y >> 10;
    const int right = ((pos_x + (bw4 * h_mul - 1) * sx.step) >> 10) + 1;
    const int bottom = ((pos_y + (bh4 * v_mul - 1) * sy.step) >> 10) + 1;

    const int w = (refp.width() + ss_hor) >> ss_hor;
    const int h = (refp.height() + ss_ver) >> ss_ver;
    PixelView ref = ref_plane;
    if (left < 3 || top < 3 || right + 4 > w || bottom + 4 > h) {
      const PixelView emu{ArrayView<Pixel>(t.scratch.emu_edge), 0, 320};
      emu_edge(right - left + 7, bottom - top + 7, w, h, left - 3, top - 3, emu, ref_plane);
      ref = emu.offset(3, 3);
    } else {
      ref = ref_plane.offset(left, top);
    }
    if (tmp.empty())
      mc_scaled(filter_2d, dst, ref, bw4 * h_mul, bh4 * v_mul, pos_x & 0x3ff, pos_y & 0x3ff, sx.step, sy.step, bdm);
    else
      mct_scaled(filter_2d, tmp, ref, bw4 * h_mul, bh4 * v_mul, pos_x & 0x3ff, pos_y & 0x3ff, sx.step, sy.step, bdm);
  }
  return true;
}

const Picture* ref_picture(const FrameContext& f, int ref) {
  if (ref < 0 || ref >= 7) return nullptr;
  return f.refp[ref].get();
}

Filter2d filter_2d_of(int dir1, int dir0) { return static_cast<Filter2d>(k_filter_2d[dir1 & 3][dir0 & 3]); }

// dav1d obmc(): blends the predictions made with the motion of the above and left neighbours into dst.
bool obmc_block(TaskContext& t, PixelView dst, const uint8_t (&b_dim)[4], int pl, int bx4, int by4, int w4, int h4) {
  FrameContext& f = *t.f;
  const int slot = (t.by & 31) + 5;
  const int ss_ver = pl && f.layout == PIXEL_LAYOUT_I420;
  const int ss_hor = pl && f.layout != PIXEL_LAYOUT_I444;
  const int h_mul = 4 >> ss_hor, v_mul = 4 >> ss_ver;
  const PixelView lap{ArrayView<Pixel>(t.scratch.lap), 0, 0};

  if (t.by > t.ts->tiling.row_start && (!pl || b_dim[0] * h_mul + b_dim[1] * v_mul >= 16)) {
    for (int i = 0, x = 0; x < w4 && i < imin(b_dim[2], 4);) {
      // only odd blocks are considered for overlap handling, hence +1
      const RefmvsBlock& a_r = f.rf.row(t.rt, slot - 1)[static_cast<uint64_t>(t.bx + x + 1)];
      const uint8_t(&a_b_dim)[4] = k_block_dimensions[a_r.bs];
      const int step4 = imax(2, imin(a_b_dim[0], 16));

      if (a_r.ref.ref[0] > 0) {
        const Picture* rp = ref_picture(f, a_r.ref.ref[0] - 1);
        if (!rp) return false;
        const int ow4 = imin(step4, b_dim[0]);
        const int oh4 = imin(b_dim[1], 16) >> 1;
        PixelView lap_dst = lap;
        lap_dst.stride = ow4 * h_mul;
        if (!mc_block(t, lap_dst, ArrayView<int16_t>(), ow4, (oh4 * 3 + 3) >> 2, t.bx + x, t.by, pl, a_r.mv.mv[0], *rp, a_r.ref.ref[0] - 1,
                      filter_2d_of(t.a->filter[1][bx4 + x + 1], t.a->filter[0][bx4 + x + 1])))
          return false;
        blend_h(dst.offset(x * h_mul, 0), ArrayView<const Pixel>(t.scratch.lap), h_mul * ow4, v_mul * oh4);
        i++;
      }
      x += step4;
    }
  }

  if (t.bx > t.ts->tiling.col_start)
    for (int i = 0, y = 0; y < h4 && i < imin(b_dim[3], 4);) {
      // only odd blocks are considered for overlap handling, hence +1
      const RefmvsBlock& l_r = f.rf.row(t.rt, slot + y + 1)[static_cast<uint64_t>(t.bx - 1)];
      const uint8_t(&l_b_dim)[4] = k_block_dimensions[l_r.bs];
      const int step4 = imax(2, imin(l_b_dim[1], 16));

      if (l_r.ref.ref[0] > 0) {
        const Picture* rp = ref_picture(f, l_r.ref.ref[0] - 1);
        if (!rp) return false;
        const int ow4 = imin(b_dim[0], 16) >> 1;
        const int oh4 = imin(step4, b_dim[1]);
        PixelView lap_dst = lap;
        lap_dst.stride = h_mul * ow4;
        if (!mc_block(t, lap_dst, ArrayView<int16_t>(), ow4, oh4, t.bx, t.by + y, pl, l_r.mv.mv[0], *rp, l_r.ref.ref[0] - 1,
                      filter_2d_of(t.l.filter[1][by4 + y + 1], t.l.filter[0][by4 + y + 1])))
          return false;
        blend_v(dst.offset(0, y * v_mul), ArrayView<const Pixel>(t.scratch.lap), h_mul * ow4, v_mul * oh4);
        i++;
      }
      y += step4;
    }
  return true;
}

// dav1d warp_affine(): warped prediction of the block into dst (pixels) or into tmp (rows `tmp_stride` entries apart).
bool warp_affine_block(TaskContext& t, PixelView dst, ArrayView<int16_t> tmp, int tmp_stride, const uint8_t (&b_dim)[4], int pl, const Picture& refp,
                       const WarpedMotionParams& wmp) {
  FrameContext& f = *t.f;
  const Picture& cur = *f.cur;
  if (refp.layout() != cur.layout() || refp.bpc() != cur.bpc()) return false;
  const int ss_ver = pl && f.layout == PIXEL_LAYOUT_I420;
  const int ss_hor = pl && f.layout != PIXEL_LAYOUT_I444;
  const int h_mul = 4 >> ss_hor, v_mul = 4 >> ss_ver;
  if (((b_dim[0] * h_mul) & 7) || ((b_dim[1] * v_mul) & 7)) return false;
  const int32_t* mat = wmp.matrix;
  const int width = (refp.width() + ss_hor) >> ss_hor;
  const int height = (refp.height() + ss_ver) >> ss_ver;
  const PixelView ref_plane = refp.plane(pl);
  const int bdm = f.bitdepth_max;
  const ArrayView<const int16_t> abcd(wmp.u.abcd);

  for (int y = 0; y < b_dim[1] * v_mul; y += 8) {
    const int src_y = t.by * 4 + ((y + 4) << ss_ver);
    const int64_t mat3_y = static_cast<int64_t>(mat[3]) * src_y + mat[0];
    const int64_t mat5_y = static_cast<int64_t>(mat[5]) * src_y + mat[1];
    for (int x = 0; x < b_dim[0] * h_mul; x += 8) {
      // calculate transformation relative to center of 8x8 block in luma pixel units
      const int src_x = t.bx * 4 + ((x + 4) << ss_hor);
      const int64_t mvx = (static_cast<int64_t>(mat[2]) * src_x + mat3_y) >> ss_hor;
      const int64_t mvy = (static_cast<int64_t>(mat[4]) * src_x + mat5_y) >> ss_ver;

      const int dx = static_cast<int>(mvx >> 16) - 4;
      const int mx = ((static_cast<int>(mvx) & 0xffff) - wmp.u.p.alpha * 4 - wmp.u.p.beta * 7) & ~0x3f;
      const int dy = static_cast<int>(mvy >> 16) - 4;
      const int my = ((static_cast<int>(mvy) & 0xffff) - wmp.u.p.gamma * 4 - wmp.u.p.delta * 4) & ~0x3f;

      PixelView ref_ptr = ref_plane;
      if (dx < 3 || dx + 8 + 4 > width || dy < 3 || dy + 8 + 4 > height) {
        const PixelView emu{ArrayView<Pixel>(t.scratch.emu_edge), 0, 32};
        emu_edge(15, 15, width, height, dx - 3, dy - 3, emu, ref_plane);
        ref_ptr = emu.offset(3, 3);
      } else {
        ref_ptr = ref_plane.offset(dx, dy);
      }
      if (!tmp.empty())
        warp8x8t(tmp.subview(static_cast<uint64_t>(y) * static_cast<uint64_t>(tmp_stride) + static_cast<uint64_t>(x)), tmp_stride, ref_ptr, abcd, mx, my, bdm);
      else
        warp8x8(dst.offset(x, y), ref_ptr, abcd, mx, my, bdm);
    }
  }
  return true;
}

}  // namespace

// ---- quantizer matrices ----------------------------------------------------------------------------------------------------------------

// dav1d_qm_tbl[level][is_uv][tx] (qm.c: the tables are stored transposed, hence the swapped names).
ArrayView<const uint8_t> qm_table(int level, int is_uv, int tx) {
  if (level < 0 || level > 14 || is_uv < 0 || is_uv > 1) return ArrayView<const uint8_t>();
  const uint64_t slot = static_cast<uint64_t>(level) * 2 + static_cast<uint64_t>(is_uv);
  auto pick = [&](ArrayView<const uint8_t> table, uint64_t w, uint64_t h) { return table.subview(slot * w * h, w * h); };
  switch (tx) {
    case TX_4X4: return pick(ArrayView<const uint8_t>(k_qm_tbl_4x4), 4, 4);
    case TX_8X8: return pick(ArrayView<const uint8_t>(k_qm_tbl_8x8), 8, 8);
    case TX_16X16: return pick(ArrayView<const uint8_t>(k_qm_tbl_16x16), 16, 16);
    case TX_32X32:
    case TX_64X64:
    case RTX_64X32:
    case RTX_32X64: return pick(ArrayView<const uint8_t>(k_qm_tbl_32x32), 32, 32);
    case RTX_4X8: return pick(ArrayView<const uint8_t>(k_qm_tbl_8x4), 8, 4);
    case RTX_8X4: return pick(ArrayView<const uint8_t>(k_qm_tbl_4x8), 4, 8);
    case RTX_4X16: return pick(ArrayView<const uint8_t>(k_qm_tbl_16x4), 16, 4);
    case RTX_16X4: return pick(ArrayView<const uint8_t>(k_qm_tbl_4x16), 4, 16);
    case RTX_8X16: return pick(ArrayView<const uint8_t>(k_qm_tbl_16x8), 16, 8);
    case RTX_16X8: return pick(ArrayView<const uint8_t>(k_qm_tbl_8x16), 8, 16);
    case RTX_8X32: return pick(ArrayView<const uint8_t>(k_qm_tbl_32x8), 32, 8);
    case RTX_32X8: return pick(ArrayView<const uint8_t>(k_qm_tbl_8x32), 8, 32);
    case RTX_16X32:
    case RTX_64X16: return pick(ArrayView<const uint8_t>(k_qm_tbl_32x16), 32, 16);
    case RTX_32X16:
    case RTX_16X64: return pick(ArrayView<const uint8_t>(k_qm_tbl_16x32), 16, 32);
    default: return ArrayView<const uint8_t>();
  }
}

// ---- intra blocks ----------------------------------------------------------------------------------------------------------------------

bool Core::recon_b_intra(TaskContext& t, BlockSize bs, int intra_edge_flags, const Av1Block& b) {
  TileState& ts = *t.ts;
  FrameContext& f = *t.f;
  Picture& cur = *f.cur;
  const int bdm = f.bitdepth_max;
  const int bx4 = t.bx & 31, by4 = t.by & 31;
  const int ss_ver = f.layout == PIXEL_LAYOUT_I420;
  const int ss_hor = f.layout != PIXEL_LAYOUT_I444;
  const int cbx4 = bx4 >> ss_hor, cby4 = by4 >> ss_ver;
  const uint8_t(&b_dim)[4] = k_block_dimensions[bs];
  const int bw4 = b_dim[0], bh4 = b_dim[1];
  const int w4 = imin(bw4, f.bw - t.bx), h4 = imin(bh4, f.bh - t.by);
  const int cw4 = (w4 + ss_hor) >> ss_hor, ch4 = (h4 + ss_ver) >> ss_ver;
  const bool has_chroma = f.layout != PIXEL_LAYOUT_I400 && (bw4 > ss_hor || (t.bx & 1)) && (bh4 > ss_ver || (t.by & 1));
  const TxfmInfo& t_dim = k_txfm_dimensions[b.tx];
  const TxfmInfo& uv_t_dim = k_txfm_dimensions[b.uvtx];
  IntraEdge& edge = t.scratch.edge;
  const int cbw4 = (bw4 + ss_hor) >> ss_hor, cbh4 = (bh4 + ss_ver) >> ss_ver;
  const int intra_edge_filter_flag = f.seq_hdr->intra_edge_filter << 10;
  const PixelView plane_y = cur.plane(0);

  for (int init_y = 0; init_y < h4; init_y += 16) {
    const int sub_h4 = imin(h4, 16 + init_y);
    const int sub_ch4 = imin(ch4, (init_y + 16) >> ss_ver);
    for (int init_x = 0; init_x < w4; init_x += 16) {
      if (b.pal_sz[0]) {
        const PixelView dst = plane_y.offset(4 * t.bx, 4 * t.by);
        pal_pred(dst, ArrayView<const Pixel>(t.scratch.pal[0]), ArrayView<const uint8_t>(t.scratch.pal_idx_y), bw4 * 4, bh4 * 4);
      }

      const int intra_flags = sm_flag(*t.a, bx4) | sm_flag(t.l, by4) | intra_edge_filter_flag;
      const int sb_has_tr = init_x + 16 < w4 ? 1 : init_y ? 0 : (intra_edge_flags & EDGE_I444_TOP_HAS_RIGHT);
      const int sb_has_bl = init_x ? 0 : init_y + 16 < h4 ? 1 : (intra_edge_flags & EDGE_I444_LEFT_HAS_BOTTOM);
      int y, x;
      const int sub_w4 = imin(w4, init_x + 16);
      for (y = init_y, t.by += init_y; y < sub_h4; y += t_dim.h, t.by += t_dim.h) {
        PixelView dst = plane_y.offset(4 * (t.bx + init_x), 4 * t.by);
        for (x = init_x, t.bx += init_x; x < sub_w4; x += t_dim.w, t.bx += t_dim.w) {
          if (!b.pal_sz[0]) {
            int angle = b.y_angle;
            const int edge_flags = (((y > init_y || !sb_has_tr) && (x + t_dim.w >= sub_w4)) ? 0 : EDGE_I444_TOP_HAS_RIGHT) |
                                   ((x > init_x || (!sb_has_bl && y + t_dim.h >= sub_h4)) ? 0 : EDGE_I444_LEFT_HAS_BOTTOM);
            const IntraPredMode m = prepare_intra_edges(t.bx, t.bx > ts.tiling.col_start, t.by, t.by > ts.tiling.row_start, ts.tiling.col_end,
                                                        ts.tiling.row_end, edge_flags, dst, top_sb_edge_of(f, 0, t.by),
                                                        static_cast<IntraPredMode>(b.y_mode), angle, t_dim.w, t_dim.h,
                                                        f.seq_hdr->intra_edge_filter, edge, bdm);
            if (!intra_pred(m, dst, edge, t_dim.w * 4, t_dim.h * 4, angle | intra_flags, 4 * f.bw - 4 * t.bx, 4 * f.bh - 4 * t.by, bdm)) return false;
          }

          if (!b.skip) {
            int eob;
            int txtp = DCT_DCT;
            uint8_t cf_ctx = 0;
            eob = decode_coefs(t, ctx_view(t.a->lcoef, bx4 + x), ctx_view(t.l.lcoef, by4 + y), b.tx, bs, b, true, 0, ArrayView<int32_t>(t.cf), txtp,
                               cf_ctx);
            set_ctx(t.a->lcoef, bx4 + x, imin(t_dim.w, f.bw - t.bx), cf_ctx);
            set_ctx(t.l.lcoef, by4 + y, imin(t_dim.h, f.bh - t.by), cf_ctx);
            if (eob >= 0) {
              if (!add_residual(t, dst, b.tx, txtp, eob)) return false;
            }
          } else {
            set_ctx(t.a->lcoef, bx4 + x, 1 << t_dim.lw, 0x40);
            set_ctx(t.l.lcoef, by4 + y, 1 << t_dim.lh, 0x40);
          }
          dst = dst.offset(4 * t_dim.w, 0);
        }
        t.bx -= x;
      }
      t.by -= y;

      if (!has_chroma) continue;

      if (b.uv_mode == CFL_PRED) {
        const PixelView y_src = plane_y.offset(4 * (t.bx & ~ss_hor), 4 * (t.by & ~ss_ver));
        const PixelView uv_dst[2] = {cur.plane(1).offset(4 * (t.bx >> ss_hor), 4 * (t.by >> ss_ver)),
                                     cur.plane(2).offset(4 * (t.bx >> ss_hor), 4 * (t.by >> ss_ver))};

        const int furthest_r = ((cw4 << ss_hor) + t_dim.w - 1) & ~(t_dim.w - 1);
        const int furthest_b = ((ch4 << ss_ver) + t_dim.h - 1) & ~(t_dim.h - 1);
        if (!cfl_ac(f.layout, ArrayView<int16_t>(t.scratch.ac), y_src, cbw4 - (furthest_r >> ss_hor), cbh4 - (furthest_b >> ss_ver), cbw4 * 4,
                    cbh4 * 4))
          return false;
        for (int pl = 0; pl < 2; pl++) {
          if (!b.cfl_alpha[pl]) continue;
          int angle = 0;
          const int xpos = t.bx >> ss_hor, ypos = t.by >> ss_ver;
          const int xstart = ts.tiling.col_start >> ss_hor;
          const int ystart = ts.tiling.row_start >> ss_ver;
          const IntraPredMode m =
              prepare_intra_edges(xpos, xpos > xstart, ypos, ypos > ystart, ts.tiling.col_end >> ss_hor, ts.tiling.row_end >> ss_ver, 0, uv_dst[pl],
                                  top_sb_edge_of(f, pl + 1, t.by & ~ss_ver), DC_PRED, angle, uv_t_dim.w, uv_t_dim.h, false, edge, bdm);
          if (!cfl_pred(m, uv_dst[pl], edge, uv_t_dim.w * 4, uv_t_dim.h * 4, ArrayView<const int16_t>(t.scratch.ac), b.cfl_alpha[pl], bdm)) return false;
        }
      } else if (b.pal_sz[1]) {
        const int ux = 4 * (t.bx >> ss_hor), uy = 4 * (t.by >> ss_ver);
        pal_pred(cur.plane(1).offset(ux, uy), ArrayView<const Pixel>(t.scratch.pal[1]), ArrayView<const uint8_t>(t.scratch.pal_idx_uv), cbw4 * 4,
                 cbh4 * 4);
        pal_pred(cur.plane(2).offset(ux, uy), ArrayView<const Pixel>(t.scratch.pal[2]), ArrayView<const uint8_t>(t.scratch.pal_idx_uv), cbw4 * 4,
                 cbh4 * 4);
      }

      const int sm_uv_fl = sm_uv_flag(*t.a, cbx4) | sm_uv_flag(t.l, cby4);
      const int uv_sb_has_tr = ((init_x + 16) >> ss_hor) < cw4 ? 1 : init_y ? 0 : (intra_edge_flags & (EDGE_I420_TOP_HAS_RIGHT >> (f.layout - 1)));
      const int uv_sb_has_bl = init_x ? 0 : ((init_y + 16) >> ss_ver) < ch4 ? 1 : (intra_edge_flags & (EDGE_I420_LEFT_HAS_BOTTOM >> (f.layout - 1)));
      const int sub_cw4 = imin(cw4, (init_x + 16) >> ss_hor);
      for (int pl = 0; pl < 2; pl++) {
        const PixelView plane_uv = cur.plane(1 + pl);
        for (y = init_y >> ss_ver, t.by += init_y; y < sub_ch4; y += uv_t_dim.h, t.by += uv_t_dim.h << ss_ver) {
          PixelView dst = plane_uv.offset(4 * ((t.bx + init_x) >> ss_hor), 4 * (t.by >> ss_ver));
          for (x = init_x >> ss_hor, t.bx += init_x; x < sub_cw4; x += uv_t_dim.w, t.bx += uv_t_dim.w << ss_hor) {
            if (!((b.uv_mode == CFL_PRED && b.cfl_alpha[pl]) || b.pal_sz[1])) {
              int angle = b.uv_angle;
              // this probably looks weird because we're using luma flags in a chroma loop, but that's because prepare_intra_edges() expects luma
              // flags as input
              const int edge_flags =
                  (((y > (init_y >> ss_ver) || !uv_sb_has_tr) && (x + uv_t_dim.w >= sub_cw4)) ? 0 : EDGE_I444_TOP_HAS_RIGHT) |
                  ((x > (init_x >> ss_hor) || (!uv_sb_has_bl && y + uv_t_dim.h >= sub_ch4)) ? 0 : EDGE_I444_LEFT_HAS_BOTTOM);
              const IntraPredMode uv_mode = b.uv_mode == CFL_PRED ? DC_PRED : static_cast<IntraPredMode>(b.uv_mode);
              const int xpos = t.bx >> ss_hor, ypos = t.by >> ss_ver;
              const int xstart = ts.tiling.col_start >> ss_hor;
              const int ystart = ts.tiling.row_start >> ss_ver;
              const IntraPredMode m = prepare_intra_edges(xpos, xpos > xstart, ypos, ypos > ystart, ts.tiling.col_end >> ss_hor,
                                                          ts.tiling.row_end >> ss_ver, edge_flags, dst, top_sb_edge_of(f, 1 + pl, t.by & ~ss_ver),
                                                          uv_mode, angle, uv_t_dim.w, uv_t_dim.h, f.seq_hdr->intra_edge_filter, edge, bdm);
              angle |= intra_edge_filter_flag;
              if (!intra_pred(m, dst, edge, uv_t_dim.w * 4, uv_t_dim.h * 4, angle | sm_uv_fl, (4 * f.bw + ss_hor - 4 * (t.bx & ~ss_hor)) >> ss_hor,
                              (4 * f.bh + ss_ver - 4 * (t.by & ~ss_ver)) >> ss_ver, bdm))
                return false;
            }

            if (!b.skip) {
              int txtp = DCT_DCT;
              uint8_t cf_ctx = 0;
              const int eob = decode_coefs(t, ctx_view(t.a->ccoef[pl], cbx4 + x), ctx_view(t.l.ccoef[pl], cby4 + y), b.uvtx, bs, b, true, 1 + pl,
                                           ArrayView<int32_t>(t.cf), txtp, cf_ctx);
              const int ctw = imin(uv_t_dim.w, (f.bw - t.bx + ss_hor) >> ss_hor);
              const int cth = imin(uv_t_dim.h, (f.bh - t.by + ss_ver) >> ss_ver);
              set_ctx(t.a->ccoef[pl], cbx4 + x, ctw, cf_ctx);
              set_ctx(t.l.ccoef[pl], cby4 + y, cth, cf_ctx);
              if (eob >= 0) {
                if (!add_residual(t, dst, b.uvtx, txtp, eob)) return false;
              }
            } else {
              set_ctx(t.a->ccoef[pl], cbx4 + x, 1 << uv_t_dim.lw, 0x40);
              set_ctx(t.l.ccoef[pl], cby4 + y, 1 << uv_t_dim.lh, 0x40);
            }
            dst = dst.offset(uv_t_dim.w * 4, 0);
          }
          t.bx -= x << ss_hor;
        }
        t.by -= y << ss_ver;
      }
    }
  }
  return true;
}

// ---- inter blocks ----------------------------------------------------------------------------------------------------------------------

bool Core::recon_b_inter(TaskContext& t, BlockSize bs, const Av1Block& b) {
  TileState& ts = *t.ts;
  FrameContext& f = *t.f;
  Picture& cur = *f.cur;
  const int bdm = f.bitdepth_max;
  const int bx4 = t.bx & 31, by4 = t.by & 31;
  const int ss_ver = f.layout == PIXEL_LAYOUT_I420;
  const int ss_hor = f.layout != PIXEL_LAYOUT_I444;
  const int cbx4 = bx4 >> ss_hor, cby4 = by4 >> ss_ver;
  const uint8_t(&b_dim)[4] = k_block_dimensions[bs];
  const int bw4 = b_dim[0], bh4 = b_dim[1];
  const int w4 = imin(bw4, f.bw - t.bx), h4 = imin(bh4, f.bh - t.by);
  const bool has_chroma = f.layout != PIXEL_LAYOUT_I400 && (bw4 > ss_hor || (t.bx & 1)) && (bh4 > ss_ver || (t.by & 1));
  const int chr_layout_idx = f.layout == PIXEL_LAYOUT_I400 ? 0 : PIXEL_LAYOUT_I444 - f.layout;

  // prediction
  const int cbh4 = (bh4 + ss_ver) >> ss_ver, cbw4 = (bw4 + ss_hor) >> ss_hor;
  const PixelView plane_y = cur.plane(0);
  const PixelView dst = plane_y.offset(4 * t.bx, 4 * t.by);
  const int uvx = 4 * (t.bx >> ss_hor), uvy = 4 * (t.by >> ss_ver);
  const PixelView uvdst_of[2] = {cur.plane(1).offset(uvx, uvy), cur.plane(2).offset(uvx, uvy)};  // (unused planes of a monochrome picture stay empty)
  const bool key_or_intra = (f.frame_hdr->frame_type & 1) == 0;

  if (key_or_intra) {
    // intrabc
    if (!mc_block(t, dst, ArrayView<int16_t>(), bw4, bh4, t.bx, t.by, 0, b.mv[0], cur, 0 /* unused */, FILTER_2D_BILINEAR)) return false;
    if (has_chroma)
      for (int pl = 1; pl < 3; pl++)
        if (!mc_block(t, uvdst_of[pl - 1], ArrayView<int16_t>(), bw4 << (bw4 == ss_hor), bh4 << (bh4 == ss_ver), t.bx & ~ss_hor, t.by & ~ss_ver, pl,
                      b.mv[0], cur, 0 /* unused */, FILTER_2D_BILINEAR))
          return false;
  } else if (b.comp_type == COMP_INTER_NONE) {
    const Picture* refp = ref_picture(f, b.ref[0]);
    if (!refp) return false;
    const Filter2d filter_2d = static_cast<Filter2d>(b.filter2d);

    if (imin(bw4, bh4) > 1 &&
        ((b.inter_mode == GLOBALMV && f.gmv_warp_allowed[b.ref[0]]) || (b.motion_mode == MM_WARP && t.warpmv.type > WM_TYPE_TRANSLATION))) {
      if (!warp_affine_block(t, dst, ArrayView<int16_t>(), 0, b_dim, 0, *refp, b.motion_mode == MM_WARP ? t.warpmv : f.frame_hdr->gmv[b.ref[0]]))
        return false;
    } else {
      if (!mc_block(t, dst, ArrayView<int16_t>(), bw4, bh4, t.bx, t.by, 0, b.mv[0], *refp, b.ref[0], filter_2d)) return false;
      if (b.motion_mode == MM_OBMC)
        if (!obmc_block(t, dst, b_dim, 0, bx4, by4, w4, h4)) return false;
    }
    if (b.interintra_type) {
      IntraEdge& tl_edge = t.scratch.edge;
      IntraPredMode m = b.interintra_mode == II_SMOOTH_PRED ? SMOOTH_PRED : static_cast<IntraPredMode>(b.interintra_mode);
      const PixelView tmp{ArrayView<Pixel>(t.scratch.interintra), 0, 4 * bw4};
      int angle = 0;
      m = prepare_intra_edges(t.bx, t.bx > ts.tiling.col_start, t.by, t.by > ts.tiling.row_start, ts.tiling.col_end, ts.tiling.row_end, 0, dst,
                              top_sb_edge_of(f, 0, t.by), m, angle, bw4, bh4, false, tl_edge, bdm);
      if (!intra_pred(m, tmp, tl_edge, bw4 * 4, bh4 * 4, 0, 0, 0, bdm)) return false;
      const ConstByteView ii_mask = wedge_masks().ii_mask(0, bs, static_cast<InterIntraType>(b.interintra_type), b.interintra_mode, b.wedge_idx);
      if (ii_mask.empty()) return false;
      blend(dst, ArrayView<const Pixel>(t.scratch.interintra), bw4 * 4, bh4 * 4, ii_mask);
    }

    if (has_chroma) {
      // sub8x8 derivation
      bool is_sub8x8 = bw4 == ss_hor || bh4 == ss_ver;
      const int rslot = (t.by & 31) + 5;
      auto R = [&](int dy, int x) -> const RefmvsBlock& { return f.rf.row(t.rt, rslot + dy)[static_cast<uint64_t>(x)]; };
      if (is_sub8x8) {
        if (bw4 == 1) is_sub8x8 &= R(0, t.bx - 1).ref.ref[0] > 0;
        if (bh4 == ss_ver) is_sub8x8 &= R(-1, t.bx).ref.ref[0] > 0;
        if (bw4 == 1 && bh4 == ss_ver) is_sub8x8 &= R(-1, t.bx - 1).ref.ref[0] > 0;
      }

      // chroma prediction
      if (is_sub8x8) {
        int h_off = 0, v_off = 0;
        if (bw4 == 1 && bh4 == ss_ver) {
          const RefmvsBlock& rb = R(-1, t.bx - 1);
          const Picture* rp = ref_picture(f, rb.ref.ref[0] - 1);
          if (!rp) return false;
          for (int pl = 0; pl < 2; pl++)
            if (!mc_block(t, uvdst_of[pl], ArrayView<int16_t>(), bw4, bh4, t.bx - 1, t.by - 1, 1 + pl, rb.mv.mv[0], *rp, rb.ref.ref[0] - 1,
                          static_cast<Filter2d>(t.tl_4x4_filter)))
              return false;
          v_off = 2;
          h_off = 2;
        }
        if (bw4 == 1) {
          const Filter2d left_filter_2d = filter_2d_of(t.l.filter[1][by4], t.l.filter[0][by4]);
          const RefmvsBlock& rb = R(0, t.bx - 1);
          const Picture* rp = ref_picture(f, rb.ref.ref[0] - 1);
          if (!rp) return false;
          for (int pl = 0; pl < 2; pl++)
            if (!mc_block(t, uvdst_of[pl].offset(0, v_off), ArrayView<int16_t>(), bw4, bh4, t.bx - 1, t.by, 1 + pl, rb.mv.mv[0], *rp,
                          rb.ref.ref[0] - 1, left_filter_2d))
              return false;
          h_off = 2;
        }
        if (bh4 == ss_ver) {
          const Filter2d top_filter_2d = filter_2d_of(t.a->filter[1][bx4], t.a->filter[0][bx4]);
          const RefmvsBlock& rb = R(-1, t.bx);
          const Picture* rp = ref_picture(f, rb.ref.ref[0] - 1);
          if (!rp) return false;
          for (int pl = 0; pl < 2; pl++)
            if (!mc_block(t, uvdst_of[pl].offset(h_off, 0), ArrayView<int16_t>(), bw4, bh4, t.bx, t.by - 1, 1 + pl,
                          rb.mv.mv[0], *rp, rb.ref.ref[0] - 1, top_filter_2d))
              return false;
          v_off = 2;
        }
        for (int pl = 0; pl < 2; pl++)
          if (!mc_block(t, uvdst_of[pl].offset(h_off, v_off), ArrayView<int16_t>(), bw4, bh4, t.bx, t.by, 1 + pl, b.mv[0], *refp, b.ref[0], filter_2d))
            return false;
      } else {
        if (imin(cbw4, cbh4) > 1 &&
            ((b.inter_mode == GLOBALMV && f.gmv_warp_allowed[b.ref[0]]) || (b.motion_mode == MM_WARP && t.warpmv.type > WM_TYPE_TRANSLATION))) {
          for (int pl = 0; pl < 2; pl++)
            if (!warp_affine_block(t, uvdst_of[pl], ArrayView<int16_t>(), 0, b_dim, 1 + pl, *refp,
                                   b.motion_mode == MM_WARP ? t.warpmv : f.frame_hdr->gmv[b.ref[0]]))
              return false;
        } else {
          for (int pl = 0; pl < 2; pl++) {
            if (!mc_block(t, uvdst_of[pl], ArrayView<int16_t>(), bw4 << (bw4 == ss_hor), bh4 << (bh4 == ss_ver), t.bx & ~ss_hor, t.by & ~ss_ver, 1 + pl,
                          b.mv[0], *refp, b.ref[0], filter_2d))
              return false;
            if (b.motion_mode == MM_OBMC)
              if (!obmc_block(t, uvdst_of[pl], b_dim, 1 + pl, bx4, by4, w4, h4)) return false;
          }
        }
        if (b.interintra_type) {
          // FIXME for 8x32 with 4:2:2 subsampling, this probably does the wrong thing since it will select 4x16, not 4x32, as a transform size...
          const ConstByteView ii_mask =
              wedge_masks().ii_mask(chr_layout_idx, bs, static_cast<InterIntraType>(b.interintra_type), b.interintra_mode, b.wedge_idx);
          if (ii_mask.empty()) return false;

          for (int pl = 0; pl < 2; pl++) {
            const PixelView tmp{ArrayView<Pixel>(t.scratch.interintra), 0, cbw4 * 4};
            IntraEdge& tl_edge = t.scratch.edge;
            IntraPredMode m = b.interintra_mode == II_SMOOTH_PRED ? SMOOTH_PRED : static_cast<IntraPredMode>(b.interintra_mode);
            int angle = 0;
            m = prepare_intra_edges(t.bx >> ss_hor, (t.bx >> ss_hor) > (ts.tiling.col_start >> ss_hor), t.by >> ss_ver,
                                    (t.by >> ss_ver) > (ts.tiling.row_start >> ss_ver), ts.tiling.col_end >> ss_hor, ts.tiling.row_end >> ss_ver, 0,
                                    uvdst_of[pl], top_sb_edge_of(f, pl + 1, t.by), m, angle, cbw4, cbh4, false, tl_edge, bdm);
            if (!intra_pred(m, tmp, tl_edge, cbw4 * 4, cbh4 * 4, 0, 0, 0, bdm)) return false;
            blend(uvdst_of[pl], ArrayView<const Pixel>(t.scratch.interintra), cbw4 * 4, cbh4 * 4, ii_mask);
          }
        }
      }
    }
    t.tl_4x4_filter = filter_2d;
  } else {
    const Filter2d filter_2d = static_cast<Filter2d>(b.filter2d);
    int jnt_weight = 0;
    ConstByteView mask;
    const Picture* refs[2] = {ref_picture(f, b.ref[0]), ref_picture(f, b.ref[1])};
    if (!refs[0] || !refs[1]) return false;
    const ArrayView<int16_t> tmp[2] = {ArrayView<int16_t>(t.scratch.compinter[0]), ArrayView<int16_t>(t.scratch.compinter[1])};

    for (int i = 0; i < 2; i++) {
      if (b.inter_mode == GLOBALMV_GLOBALMV && f.gmv_warp_allowed[b.ref[i]]) {
        if (!warp_affine_block(t, PixelView{}, tmp[i], bw4 * 4, b_dim, 0, *refs[i], f.frame_hdr->gmv[b.ref[i]])) return false;
      } else {
        if (!mc_block(t, PixelView{}, tmp[i], bw4, bh4, t.bx, t.by, 0, b.mv[i], *refs[i], b.ref[i], filter_2d)) return false;
      }
    }
    switch (b.comp_type) {
      case COMP_INTER_AVG: avg(dst, tmp[0], tmp[1], bw4 * 4, bh4 * 4, bdm); break;
      case COMP_INTER_WEIGHTED_AVG:
        jnt_weight = f.jnt_weights[b.ref[0]][b.ref[1]];
        w_avg(dst, tmp[0], tmp[1], bw4 * 4, bh4 * 4, jnt_weight, bdm);
        break;
      case COMP_INTER_SEG:
        w_mask(chr_layout_idx > 0, chr_layout_idx > 1, dst, tmp[b.mask_sign], tmp[!b.mask_sign], bw4 * 4, bh4 * 4, ArrayView<uint8_t>(t.scratch.seg_mask),
               b.mask_sign, bdm);
        mask = ArrayView<const uint8_t>(t.scratch.seg_mask);
        break;
      case COMP_INTER_WEDGE:
        mask = wedge_masks().wedge_mask(0, bs, 0, b.wedge_idx);
        if (mask.empty()) return false;
        ::hardware::av1::mask(dst, tmp[b.mask_sign], tmp[!b.mask_sign], bw4 * 4, bh4 * 4, mask, bdm);
        if (has_chroma) mask = wedge_masks().wedge_mask(chr_layout_idx, bs, b.mask_sign, b.wedge_idx);
        break;
      default: return false;
    }

    // chroma
    if (has_chroma)
      for (int pl = 0; pl < 2; pl++) {
        for (int i = 0; i < 2; i++) {
          if (b.inter_mode == GLOBALMV_GLOBALMV && imin(cbw4, cbh4) > 1 && f.gmv_warp_allowed[b.ref[i]]) {
            if (!warp_affine_block(t, PixelView{}, tmp[i], (bw4 * 4) >> ss_hor, b_dim, 1 + pl, *refs[i], f.frame_hdr->gmv[b.ref[i]])) return false;
          } else {
            if (!mc_block(t, PixelView{}, tmp[i], bw4, bh4, t.bx, t.by, 1 + pl, b.mv[i], *refs[i], b.ref[i], filter_2d)) return false;
          }
        }
        const PixelView uvdst = uvdst_of[pl];
        switch (b.comp_type) {
          case COMP_INTER_AVG: avg(uvdst, tmp[0], tmp[1], (bw4 * 4) >> ss_hor, (bh4 * 4) >> ss_ver, bdm); break;
          case COMP_INTER_WEIGHTED_AVG: w_avg(uvdst, tmp[0], tmp[1], (bw4 * 4) >> ss_hor, (bh4 * 4) >> ss_ver, jnt_weight, bdm); break;
          case COMP_INTER_WEDGE:
          case COMP_INTER_SEG:
            ::hardware::av1::mask(uvdst, tmp[b.mask_sign], tmp[!b.mask_sign], (bw4 * 4) >> ss_hor, (bh4 * 4) >> ss_ver, mask, bdm);
            break;
          default: break;
        }
      }
  }

  const int cw4 = (w4 + ss_hor) >> ss_hor, ch4 = (h4 + ss_ver) >> ss_ver;

  if (b.skip) {
    // reset coef contexts
    set_ctx(t.a->lcoef, bx4, 1 << b_dim[2], 0x40);
    set_ctx(t.l.lcoef, by4, 1 << b_dim[3], 0x40);
    if (has_chroma) {
      const int cw = 1 << ulog2(static_cast<unsigned>(cbw4)), ch = 1 << ulog2(static_cast<unsigned>(cbh4));
      set_ctx(t.a->ccoef[0], cbx4, cw, 0x40);
      set_ctx(t.a->ccoef[1], cbx4, cw, 0x40);
      set_ctx(t.l.ccoef[0], cby4, ch, 0x40);
      set_ctx(t.l.ccoef[1], cby4, ch, 0x40);
    }
    return true;
  }

  const TxfmInfo& uvtx = k_txfm_dimensions[b.uvtx];
  const TxfmInfo& ytx = k_txfm_dimensions[b.max_ytx];
  const uint16_t tx_split[2] = {b.tx_split0, b.tx_split1};

  for (int init_y = 0; init_y < bh4; init_y += 16) {
    for (int init_x = 0; init_x < bw4; init_x += 16) {
      // coefficient coding & inverse transforms
      int y_off = !!init_y, y;
      for (y = init_y, t.by += init_y; y < imin(h4, init_y + 16); y += ytx.h, y_off++) {
        int x, x_off = !!init_x;
        for (x = init_x, t.bx += init_x; x < imin(w4, init_x + 16); x += ytx.w, x_off++) {
          if (!read_coef_tree(t, bs, b, b.max_ytx, 0, tx_split, x_off, y_off, dst.offset(x * 4, y * 4))) return false;
          t.bx += ytx.w;
        }
        t.bx -= x;
        t.by += ytx.h;
      }
      t.by -= y;

      // chroma coefs and inverse transform
      if (has_chroma)
        for (int pl = 0; pl < 2; pl++) {
          const PixelView uvdst = uvdst_of[pl];
          for (y = init_y >> ss_ver, t.by += init_y; y < imin(ch4, (init_y + 16) >> ss_ver); y += uvtx.h) {
            int x;
            for (x = init_x >> ss_hor, t.bx += init_x; x < imin(cw4, (init_x + 16) >> ss_hor); x += uvtx.w) {
              int txtp = t.scratch.txtp_map[(by4 + (y << ss_ver)) * 32 + bx4 + (x << ss_hor)];
              uint8_t cf_ctx = 0;
              const int eob = decode_coefs(t, ctx_view(t.a->ccoef[pl], cbx4 + x), ctx_view(t.l.ccoef[pl], cby4 + y), b.uvtx, bs, b, false, 1 + pl,
                                           ArrayView<int32_t>(t.cf), txtp, cf_ctx);
              const int ctw = imin(uvtx.w, (f.bw - t.bx + ss_hor) >> ss_hor);
              const int cth = imin(uvtx.h, (f.bh - t.by + ss_ver) >> ss_ver);
              set_ctx(t.a->ccoef[pl], cbx4 + x, ctw, cf_ctx);
              set_ctx(t.l.ccoef[pl], cby4 + y, cth, cf_ctx);
              if (eob >= 0) {
                if (!add_residual(t, uvdst.offset(4 * x, 4 * y), b.uvtx, txtp, eob)) return false;
              }
              t.bx += uvtx.w << ss_hor;
            }
            t.bx -= x << ss_hor;
            t.by += uvtx.h << ss_ver;
          }
          t.by -= y << ss_ver;
        }
    }
  }
  return true;
}

// ---- palettes --------------------------------------------------------------------------------------------------------------------------

void Core::read_pal_plane(TaskContext& t, Av1Block& b, bool pl_flag, int sz_ctx, int bx4, int by4) {
  TileState& ts = *t.ts;
  const FrameContext& f = *t.f;
  Msac& msac = ts.msac;
  const int pl = pl_flag ? 1 : 0;
  const int pal_sz = b.pal_sz[pl] = static_cast<uint8_t>(msac.symbol_adapt8(ArrayView<uint16_t>(ts.cdf.m.pal_sz[pl][sz_ctx]), 6) + 2);
  Pixel cache_store[16], used_cache_store[8];
  ArrayView<Pixel> cache(cache_store), used_cache(used_cache_store);
  int l_cache = pl ? t.pal_sz_uv[1][by4] : t.l.pal_sz[by4];
  int n_cache = 0;
  // don't reuse above palette outside SB64 boundaries
  int a_cache = by4 & 15 ? (pl ? t.pal_sz_uv[0][bx4] : t.a->pal_sz[bx4]) : 0;
  const ArrayView<const Pixel> l_pal(t.al_pal[1][by4][pl]);
  const ArrayView<const Pixel> a_pal(t.al_pal[0][bx4][pl]);
  uint64_t li = 0, ai = 0;

  // fill/sort cache
  while (l_cache && a_cache) {
    if (l_pal[li] < a_pal[ai]) {
      if (!n_cache || cache[n_cache - 1] != l_pal[li]) cache[n_cache++] = l_pal[li];
      li++;
      l_cache--;
    } else {
      if (a_pal[ai] == l_pal[li]) {
        li++;
        l_cache--;
      }
      if (!n_cache || cache[n_cache - 1] != a_pal[ai]) cache[n_cache++] = a_pal[ai];
      ai++;
      a_cache--;
    }
  }
  if (l_cache) {
    do {
      if (!n_cache || cache[n_cache - 1] != l_pal[li]) cache[n_cache++] = l_pal[li];
      li++;
    } while (--l_cache > 0);
  } else if (a_cache) {
    do {
      if (!n_cache || cache[n_cache - 1] != a_pal[ai]) cache[n_cache++] = a_pal[ai];
      ai++;
    } while (--a_cache > 0);
  }

  // find reused cache entries
  int i = 0;
  for (int n = 0; n < n_cache && i < pal_sz; n++)
    if (msac.bool_equi()) used_cache[i++] = cache[n];
  const int n_used_cache = i;

  // parse new entries
  const ArrayView<Pixel> pal(t.scratch.pal[pl]);
  if (i < pal_sz) {
    const int bpc = f.cur->bpc();
    int prev = pal[i++] = static_cast<Pixel>(msac.bools(static_cast<unsigned>(bpc)));

    if (i < pal_sz) {
      int bits = bpc - 3 + static_cast<int>(msac.bools(2));
      const int max = (1 << bpc) - 1;

      do {
        const int delta = static_cast<int>(msac.bools(static_cast<unsigned>(bits)));
        prev = imin(prev + delta + !pl, max);
        pal[i++] = static_cast<Pixel>(prev);
        if (prev + !pl >= max) {
          for (; i < pal_sz; i++) pal[i] = static_cast<Pixel>(max);
          break;
        }
        bits = imin(bits, 1 + ulog2(static_cast<unsigned>(max - prev - !pl)));
      } while (i < pal_sz);
    }

    // merge cache+new entries
    int n = 0, m = n_used_cache;
    for (i = 0; i < pal_sz; i++) {
      if (n < n_used_cache && (m >= pal_sz || used_cache[n] <= pal[m])) {
        pal[i] = used_cache[n++];
      } else {
        pal[i] = pal[m < pal_sz ? m : pal_sz - 1];
        m++;
      }
    }
  } else {
    for (int n = 0; n < n_used_cache; n++) pal[n] = used_cache[n];
  }
}

void Core::read_pal_uv(TaskContext& t, Av1Block& b, int sz_ctx, int bx4, int by4) {
  read_pal_plane(t, b, true, sz_ctx, bx4, by4);

  // V pal coding
  TileState& ts = *t.ts;
  const FrameContext& f = *t.f;
  Msac& msac = ts.msac;
  const ArrayView<Pixel> pal(t.scratch.pal[2]);
  const int bpc = f.cur->bpc();
  if (msac.bool_equi()) {
    const int bits = bpc - 4 + static_cast<int>(msac.bools(2));
    int prev = static_cast<int>(msac.bools(static_cast<unsigned>(bpc)));
    pal[0] = static_cast<Pixel>(prev);
    const int max = (1 << bpc) - 1;
    for (int i = 1; i < b.pal_sz[1]; i++) {
      int delta = static_cast<int>(msac.bools(static_cast<unsigned>(bits)));
      if (delta && msac.bool_equi()) delta = -delta;
      prev = (prev + delta) & max;
      pal[i] = static_cast<Pixel>(prev);
    }
  } else {
    for (int i = 0; i < b.pal_sz[1]; i++) pal[i] = static_cast<Pixel>(msac.bools(static_cast<unsigned>(bpc)));
  }
}

void Core::copy_pal_block_y(TaskContext& t, int bx4, int by4, int bw4, int bh4) {
  const ArrayView<const Pixel> pal(t.scratch.pal[0]);
  for (int x = 0; x < bw4; x++) ArrayView<Pixel>(t.al_pal[0][bx4 + x][0]).copy_from(pal);
  for (int y = 0; y < bh4; y++) ArrayView<Pixel>(t.al_pal[1][by4 + y][0]).copy_from(pal);
}

void Core::copy_pal_block_uv(TaskContext& t, int bx4, int by4, int bw4, int bh4) {
  // see aomedia bug 2183 for why we use luma coordinates here
  for (int pl = 1; pl <= 2; pl++) {
    const ArrayView<const Pixel> pal(t.scratch.pal[pl]);
    for (int x = 0; x < bw4; x++) ArrayView<Pixel>(t.al_pal[0][bx4 + x][pl]).copy_from(pal);
    for (int y = 0; y < bh4; y++) ArrayView<Pixel>(t.al_pal[1][by4 + y][pl]).copy_from(pal);
  }
}

// ---- intra edge backup -----------------------------------------------------------------------------------------------------------------

void Core::backup_ipred_edge(TaskContext& t) {
  FrameContext& f = *t.f;
  TileState& ts = *t.ts;
  Picture& cur = *f.cur;
  const int sby = t.by >> f.sb_shift;
  const uint64_t sby_off = static_cast<uint64_t>(f.sb128w) * 128u * static_cast<uint64_t>(sby);
  const int x_off = ts.tiling.col_start;

  const int count = 4 * (ts.tiling.col_end - x_off);
  const ArrayView<const Pixel> y = pixel_row(cur.plane(0), x_off * 4, (t.by + f.sb_step) * 4 - 1, count);
  f.ipred_edge[0].view().subview(sby_off + static_cast<uint64_t>(x_off) * 4u, static_cast<uint64_t>(count)).copy_from(y);

  if (f.layout != PIXEL_LAYOUT_I400) {
    const int ss_ver = f.layout == PIXEL_LAYOUT_I420;
    const int ss_hor = f.layout != PIXEL_LAYOUT_I444;
    const int ucount = count >> ss_hor;
    for (int pl = 1; pl <= 2; pl++) {
      const ArrayView<const Pixel> src =
          pixel_row(cur.plane(pl), (x_off * 4) >> ss_hor, (((t.by + f.sb_step) * 4) >> ss_ver) - 1, ucount);
      f.ipred_edge[pl].view().subview(sby_off + static_cast<uint64_t>((x_off * 4) >> ss_hor), static_cast<uint64_t>(ucount)).copy_from(src);
    }
  }
}

}  // namespace av1
}  // namespace hardware
