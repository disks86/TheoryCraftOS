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

#include "Hardware/VideoCodec/Software/Av1Refmvs.hpp"
#include "Utility/AllocatedView.hpp"

namespace hardware {
namespace av1 {

namespace {
#include "Hardware/VideoCodec/Software/Av1Tables.inc"

// Bounds-checked access to a small fixed array with an int index.
template <class T, uint64_t N>
T& at(T (&array)[N], int index) {
  return ArrayView<T>(array)[static_cast<uint64_t>(static_cast<int64_t>(index))];
}

int imin(int a, int b) { return a < b ? a : b; }
int imax(int a, int b) { return a > b ? a : b; }
int iclip(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int iabs(int v) { return v < 0 ? -v : v; }
int apply_sign(int v, int s) { return s < 0 ? -v : v; }

Mv make_mv(int y, int x) {
  Mv mv;
  mv.y = static_cast<int16_t>(y);
  mv.x = static_cast<int16_t>(x);
  return mv;
}
Mv make_mv_n(uint32_t n) {
  Mv mv;
  mv.n = n;
  return mv;
}

int get_poc_diff(int order_hint_n_bits, int poc0, int poc1) {
  if (!order_hint_n_bits) return 0;
  const int mask = 1 << (order_hint_n_bits - 1);
  const int diff = poc0 - poc1;
  return (diff & (mask - 1)) - (diff & mask);
}

void fix_int_mv_precision(Mv& mv) {
  mv.x = static_cast<int16_t>((mv.x - (mv.x >> 15) + 3) & ~7U);
  mv.y = static_cast<int16_t>((mv.y - (mv.y >> 15) + 3) & ~7U);
}

void fix_mv_precision(const FrameHeader& hdr, Mv& mv) {
  if (hdr.force_integer_mv) {
    fix_int_mv_precision(mv);
  } else if (!hdr.hp) {
    mv.x = static_cast<int16_t>((mv.x - (mv.x >> 15)) & ~1U);
    mv.y = static_cast<int16_t>((mv.y - (mv.y >> 15)) & ~1U);
  }
}

// The 32-bit products wrap like dav1d's (the header parser bounds the matrix, so they do not wrap for a valid stream).
int wrap_mul_add(int32_t a, int b, int32_t c, int d, int32_t e) {
  return static_cast<int>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b) + static_cast<uint32_t>(c) * static_cast<uint32_t>(d) +
                          static_cast<uint32_t>(e));
}

Mv get_gmv_2d(const WarpedMotionParams& gmv, int bx4, int by4, int bw4, int bh4, const FrameHeader& hdr) {
  switch (gmv.type) {
    case WM_TYPE_IDENTITY:
      return make_mv(0, 0);
    case WM_TYPE_TRANSLATION: {
      Mv res = make_mv(gmv.matrix[0] >> 13, gmv.matrix[1] >> 13);
      if (hdr.force_integer_mv) fix_int_mv_precision(res);
      return res;
    }
    case WM_TYPE_ROT_ZOOM:
    case WM_TYPE_AFFINE:
    default: {
      const int x = bx4 * 4 + bw4 * 2 - 1;
      const int y = by4 * 4 + bh4 * 2 - 1;
      const int xc = wrap_mul_add(gmv.matrix[2] - (1 << 16), x, gmv.matrix[3], y, gmv.matrix[0]);
      const int yc = wrap_mul_add(gmv.matrix[5] - (1 << 16), y, gmv.matrix[4], x, gmv.matrix[1]);
      const int shift = 16 - (3 - !hdr.hp);
      const int round = (1 << shift) >> 1;
      const int64_t ay = yc < 0 ? -static_cast<int64_t>(yc) : yc;
      const int64_t ax = xc < 0 ? -static_cast<int64_t>(xc) : xc;
      Mv res = make_mv(apply_sign(static_cast<int>(((ay + round) >> shift) << !hdr.hp), yc),
                       apply_sign(static_cast<int>(((ax + round) >> shift) << !hdr.hp), xc));
      if (hdr.force_integer_mv) fix_int_mv_precision(res);
      return res;
    }
  }
}

Mv mv_projection(Mv mv, int num, int den) {
  static const uint16_t div_mult[32] = {0,    16384, 8192, 5461, 4096, 3276, 2730, 2340, 2048, 1820, 1638, 1489, 1365, 1260, 1170, 1092,
                                        1024, 963,   910,  862,  819,  780,  744,  712,  682,  655,  630,  606,  585,  564,  546,  528};
  const int frac = num * static_cast<int>(at(div_mult, den));
  const int y = mv.y * frac, x = mv.x * frac;
  // Round and clip according to AV1 spec section 7.9.3 (0x3fff == (1 << 14) - 1)
  return make_mv(iclip((y + 8192 + (y >> 31)) >> 14, -0x3fff, 0x3fff), iclip((x + 8192 + (x >> 31)) >> 14, -0x3fff, 0x3fff));
}

// ---- spatial candidates ---------------------------------------------------------------------------------------------------

void add_spatial_candidate(ArrayView<RefmvsCandidate> mvstack, int& cnt, int weight, const RefmvsBlock& b, RefmvsRefPair ref,
                           const Mv gmv[2], int& have_newmv_match, int& have_refmv_match) {
  if (b.mv.mv[0].n == INVALID_MV) return;  // intra block, no intrabc

  if (ref.ref[1] == -1) {
    for (int n = 0; n < 2; n++) {
      if (b.ref.ref[n] == ref.ref[0]) {
        const Mv cand_mv = ((b.mf & 1) && gmv[0].n != INVALID_MV) ? gmv[0] : b.mv.mv[n];

        have_refmv_match = 1;
        have_newmv_match |= b.mf >> 1;

        const int last = cnt;
        for (int m = 0; m < last; m++) {
          if (mvstack[m].mv.mv[0].n == cand_mv.n) {
            mvstack[m].weight += weight;
            return;
          }
        }
        if (last < 8) {
          mvstack[last].mv.mv[0] = cand_mv;
          mvstack[last].weight = weight;
          cnt = last + 1;
        }
        return;
      }
    }
  } else if (b.ref == ref) {
    RefmvsMvPair cand_mv;
    cand_mv.mv[0] = ((b.mf & 1) && gmv[0].n != INVALID_MV) ? gmv[0] : b.mv.mv[0];
    cand_mv.mv[1] = ((b.mf & 1) && gmv[1].n != INVALID_MV) ? gmv[1] : b.mv.mv[1];

    have_refmv_match = 1;
    have_newmv_match |= b.mf >> 1;

    const int last = cnt;
    for (int n = 0; n < last; n++) {
      if (mvstack[n].mv == cand_mv) {
        mvstack[n].weight += weight;
        return;
      }
    }
    if (last < 8) {
      mvstack[last].mv = cand_mv;
      mvstack[last].weight = weight;
      cnt = last + 1;
    }
  }
}

// `row` is one row of the block buffer and `base` the column of the first candidate (dav1d's pointer b).
int scan_row(ArrayView<RefmvsCandidate> mvstack, int& cnt, RefmvsRefPair ref, const Mv gmv[2], ArrayView<const RefmvsBlock> row, int base,
             int bw4, int w4, int max_rows, int step, int& have_newmv_match, int& have_refmv_match) {
  const RefmvsBlock* cand_b = &row[static_cast<uint64_t>(base)];
  const uint8_t* const first_cand_b_dim = k_block_dimensions[cand_b->bs < N_BS_SIZES ? cand_b->bs : 0];
  int cand_bw4 = first_cand_b_dim[0];
  int len = imax(step, imin(bw4, cand_bw4));

  if (bw4 <= cand_bw4) {
    const int weight = bw4 == 1 ? 2 : imax(2, imin(2 * max_rows, first_cand_b_dim[1]));
    add_spatial_candidate(mvstack, cnt, len * weight, *cand_b, ref, gmv, have_newmv_match, have_refmv_match);
    return weight >> 1;
  }

  for (int x = 0;;) {
    add_spatial_candidate(mvstack, cnt, len * 2, *cand_b, ref, gmv, have_newmv_match, have_refmv_match);
    x += len;
    if (x >= w4) return 1;
    cand_b = &row[static_cast<uint64_t>(base + x)];
    cand_bw4 = k_block_dimensions[cand_b->bs < N_BS_SIZES ? cand_b->bs : 0][0];
    len = imax(step, cand_bw4);
  }
}

// `slot0` is the first block-buffer slot of the column scan (dav1d's b), `col` the column.
int scan_col(const RefmvsFrame& rf, const RefmvsTile& rt, ArrayView<RefmvsCandidate> mvstack, int& cnt, RefmvsRefPair ref,
             const Mv gmv[2], int slot0, int bh4, int h4, int col, int max_cols, int step, int& have_newmv_match,
             int& have_refmv_match) {
  const RefmvsBlock* cand_b = &rf.row(rt, slot0)[static_cast<uint64_t>(col)];
  const uint8_t* const first_cand_b_dim = k_block_dimensions[cand_b->bs < N_BS_SIZES ? cand_b->bs : 0];
  int cand_bh4 = first_cand_b_dim[1];
  int len = imax(step, imin(bh4, cand_bh4));

  if (bh4 <= cand_bh4) {
    const int weight = bh4 == 1 ? 2 : imax(2, imin(2 * max_cols, first_cand_b_dim[0]));
    add_spatial_candidate(mvstack, cnt, len * weight, *cand_b, ref, gmv, have_newmv_match, have_refmv_match);
    return weight >> 1;
  }

  for (int y = 0;;) {
    add_spatial_candidate(mvstack, cnt, len * 2, *cand_b, ref, gmv, have_newmv_match, have_refmv_match);
    y += len;
    if (y >= h4) return 1;
    cand_b = &rf.row(rt, slot0 + y)[static_cast<uint64_t>(col)];
    cand_bh4 = k_block_dimensions[cand_b->bs < N_BS_SIZES ? cand_b->bs : 0][1];
    len = imax(step, cand_bh4);
  }
}

// ---- temporal candidates --------------------------------------------------------------------------------------------------

// `pocdiff` are the 7 current-to-reference distances; gmv0/globalmv_ctx are only used (non-null ctx) for the first, direct candidate.
void add_temporal_candidate(const FrameHeader& hdr, const int8_t (&pocdiff)[7], ArrayView<RefmvsCandidate> mvstack, int& cnt,
                            const RefmvsTemporalBlock& rb, RefmvsRefPair ref, int* globalmv_ctx, Mv gmv0, int weight) {
  if (rb.mv.n == INVALID_MV) return;

  Mv mv = mv_projection(rb.mv, at(pocdiff, ref.ref[0] - 1), rb.ref);
  fix_mv_precision(hdr, mv);

  const int last = cnt;
  if (ref.ref[1] == -1) {
    if (globalmv_ctx) *globalmv_ctx = (iabs(mv.x - gmv0.x) | iabs(mv.y - gmv0.y)) >= 16;

    for (int n = 0; n < last; n++) {
      if (mvstack[n].mv.mv[0].n == mv.n) {
        mvstack[n].weight += weight;
        return;
      }
    }
    if (last < 8) {
      mvstack[last].mv.mv[0] = mv;
      mvstack[last].weight = weight;
      cnt = last + 1;
    }
  } else {
    RefmvsMvPair mvp;
    mvp.mv[0] = mv;
    mvp.mv[1] = mv_projection(rb.mv, at(pocdiff, ref.ref[1] - 1), rb.ref);
    fix_mv_precision(hdr, mvp.mv[1]);

    for (int n = 0; n < last; n++) {
      if (mvstack[n].mv == mvp) {
        mvstack[n].weight += weight;
        return;
      }
    }
    if (last < 8) {
      mvstack[last].mv = mvp;
      mvstack[last].weight = weight;
      cnt = last + 1;
    }
  }
}

// ---- extended candidates --------------------------------------------------------------------------------------------------

// `same` is mvstack[cnt..]: same[0..1] collect the candidates that use the reference, same[2..3] (diff) the others.
void add_compound_extended_candidate(ArrayView<RefmvsCandidate> same, int (&same_count)[4], const RefmvsBlock& cand_b, int sign0, int sign1,
                                     RefmvsRefPair ref, const uint8_t (&sign_bias)[7]) {
  for (int n = 0; n < 2; n++) {
    const int cand_ref = cand_b.ref.ref[n];
    if (cand_ref <= 0) break;

    Mv cand_mv = cand_b.mv.mv[n];
    if (cand_ref == ref.ref[0]) {
      if (same_count[0] < 2) same[same_count[0]++].mv.mv[0] = cand_mv;
      if (same_count[3] < 2) {
        if (sign1 ^ at(sign_bias, cand_ref - 1)) cand_mv = make_mv(-cand_mv.y, -cand_mv.x);
        same[2 + same_count[3]++].mv.mv[1] = cand_mv;
      }
    } else if (cand_ref == ref.ref[1]) {
      if (same_count[1] < 2) same[same_count[1]++].mv.mv[1] = cand_mv;
      if (same_count[2] < 2) {
        if (sign0 ^ at(sign_bias, cand_ref - 1)) cand_mv = make_mv(-cand_mv.y, -cand_mv.x);
        same[2 + same_count[2]++].mv.mv[0] = cand_mv;
      }
    } else {
      const Mv i_cand_mv = make_mv(-cand_mv.y, -cand_mv.x);
      if (same_count[2] < 2) {
        same[2 + same_count[2]++].mv.mv[0] = (sign0 ^ at(sign_bias, cand_ref - 1)) ? i_cand_mv : cand_mv;
      }
      if (same_count[3] < 2) {
        same[2 + same_count[3]++].mv.mv[1] = (sign1 ^ at(sign_bias, cand_ref - 1)) ? i_cand_mv : cand_mv;
      }
    }
  }
}

void add_single_extended_candidate(ArrayView<RefmvsCandidate> mvstack, int& cnt, const RefmvsBlock& cand_b, int sign,
                                   const uint8_t (&sign_bias)[7]) {
  for (int n = 0; n < 2; n++) {
    const int cand_ref = cand_b.ref.ref[n];
    if (cand_ref <= 0) break;

    Mv cand_mv = cand_b.mv.mv[n];
    if (sign ^ at(sign_bias, cand_ref - 1)) cand_mv = make_mv(-cand_mv.y, -cand_mv.x);

    int m;
    const int last = cnt;
    for (m = 0; m < last; m++) {
      if (cand_mv.n == mvstack[m].mv.mv[0].n) break;
    }
    if (m == last) {
      mvstack[m].mv.mv[0] = cand_mv;
      mvstack[m].weight = 2;  // "minimal"
      cnt = last + 1;
    }
  }
}

}  // namespace

// ---- MotionVectorStore ----------------------------------------------------------------------------------------------------

MotionVectorStore::MotionVectorStore(MotionVectorStore&& other)
    : allocator_(other.allocator_), blocks_(other.blocks_), iw8_(other.iw8_), ih8_(other.ih8_), stride_(other.stride_),
      frame_offset_(other.frame_offset_) {
  for (int i = 0; i < 7; i++) at(ref_poc_, i) = at(other.ref_poc_, i);
  other.blocks_ = ArrayView<RefmvsTemporalBlock>();
}

MotionVectorStore& MotionVectorStore::operator=(MotionVectorStore&& other) {
  if (this != &other) {
    release();
    blocks_ = other.blocks_;
    iw8_ = other.iw8_;
    ih8_ = other.ih8_;
    stride_ = other.stride_;
    frame_offset_ = other.frame_offset_;
    for (int i = 0; i < 7; i++) at(ref_poc_, i) = at(other.ref_poc_, i);
    other.blocks_ = ArrayView<RefmvsTemporalBlock>();
  }
  return *this;
}

void MotionVectorStore::set_order_hints(uint8_t frame_offset, ArrayView<const uint8_t> ref_poc) {
  frame_offset_ = frame_offset;
  for (int i = 0; i < 7; i++) at(ref_poc_, i) = ref_poc[static_cast<uint64_t>(i)];
}

void MotionVectorStore::release() {
  release_view(allocator_, blocks_);
  iw8_ = ih8_ = stride_ = 0;
}

bool MotionVectorStore::allocate(int width, int height) {
  release();
  if (width <= 0 || height <= 0 || width > 65536 || height > 65536) return false;
  const int64_t stride = ((static_cast<int64_t>(width) + 127) & ~int64_t{127}) >> 3;
  const int iw8 = (width + 7) >> 3, ih8 = (height + 7) >> 3;
  const int64_t rows = (((static_cast<int64_t>(ih8) * 2 + 31) >> 5)) * 16;  // sb128h * 16
  blocks_ = allocate_view<RefmvsTemporalBlock>(allocator_, static_cast<uint64_t>(stride * rows));
  if (blocks_.empty()) return false;
  blocks_.fill(RefmvsTemporalBlock{});
  iw8_ = iw8;
  ih8_ = ih8;
  stride_ = static_cast<int>(stride);
  return true;
}

// ---- RefmvsFrame ----------------------------------------------------------------------------------------------------------

void RefmvsFrame::release() {
  release_view(allocator_, r_);
  release_view(allocator_, rp_proj_);
  n_blocks_ = 0;
}

bool RefmvsFrame::init_frame(const SequenceHeader& seq_hdr, const FrameHeader& frm_hdr, ArrayView<const uint8_t> ref_poc,
                             MotionVectorStore* cur, ArrayView<const MotionVectorStore* const> refs) {
  if (frm_hdr.width[0] <= 0 || frm_hdr.height <= 0 || frm_hdr.width[0] > 65536 || frm_hdr.height > 65536) return false;
  if (ref_poc.size() < 7 || refs.size() < 7) return false;
  const int rp_stride = ((frm_hdr.width[0] + 127) & ~127) >> 3;
  const int n_blocks = rp_stride;

  sbsz_ = 16 << seq_hdr.sb128;
  frm_hdr_ = &frm_hdr;
  iw8_ = (frm_hdr.width[0] + 7) >> 3;
  ih8_ = (frm_hdr.height + 7) >> 3;
  iw4_ = iw8_ << 1;
  ih4_ = ih8_ << 1;
  rp_stride_ = rp_stride;
  cur_ = cur;

  if (n_blocks != n_blocks_ || r_.empty()) {
    release();
    r_ = allocate_view<RefmvsBlock>(allocator_, static_cast<uint64_t>(35) * 2 * static_cast<uint64_t>(n_blocks));
    rp_proj_ = allocate_view<RefmvsTemporalBlock>(allocator_, static_cast<uint64_t>(16) * static_cast<uint64_t>(n_blocks));
    if (r_.empty() || rp_proj_.empty()) {
      release();
      return false;
    }
    r_.fill(RefmvsBlock{});
    rp_proj_.fill(RefmvsTemporalBlock{});
    n_blocks_ = n_blocks;
  }

  // The references' own references, and the references that can be projected (same 8x8 size, saved vectors present).
  uint8_t ref_ref_poc[7][7] = {};
  for (int i = 0; i < 7; i++) {
    const MotionVectorStore* ref_store = refs[static_cast<uint64_t>(i)];
    const bool usable = frm_hdr.use_ref_frame_mvs && ref_store && ref_store->valid() && ref_store->iw8() == iw8_ &&
                        ref_store->ih8() == ih8_;
    rp_ref_[i] = usable ? ref_store : nullptr;
    if (usable) {
      for (int m = 0; m < 7; m++) at(ref_ref_poc[i], m) = at(ref_store->ref_poc_, m);
    }
  }

  const int poc = frm_hdr.frame_offset;
  const int bits = seq_hdr.order_hint_n_bits;
  for (int i = 0; i < 7; i++) {
    const int rpoc = ref_poc[static_cast<uint64_t>(i)];
    const int poc_diff = get_poc_diff(bits, rpoc, poc);
    at(sign_bias_, i) = poc_diff > 0;
    at(mfmv_sign_, i) = poc_diff < 0;
    at(pocdiff_, i) = static_cast<int8_t>(iclip(get_poc_diff(bits, poc, rpoc), -31, 31));
  }
  if (cur) {
    cur->frame_offset_ = frm_hdr.frame_offset;
    for (int i = 0; i < 7; i++) at(cur->ref_poc_, i) = ref_poc[static_cast<uint64_t>(i)];
  }

  // temporal MV setup
  n_mfmvs_ = 0;
  if (frm_hdr.use_ref_frame_mvs && bits) {
    int total = 2;
    if (rp_ref_[0] && ref_ref_poc[0][6] != ref_poc[3] /* alt-of-last != gold */) {
      at(mfmv_ref_, n_mfmvs_++) = 0;  // last
      total = 3;
    }
    if (rp_ref_[4] && get_poc_diff(bits, ref_poc[4], frm_hdr.frame_offset) > 0) at(mfmv_ref_, n_mfmvs_++) = 4;  // bwd
    if (rp_ref_[5] && get_poc_diff(bits, ref_poc[5], frm_hdr.frame_offset) > 0) at(mfmv_ref_, n_mfmvs_++) = 5;  // altref2
    if (n_mfmvs_ < total && rp_ref_[6] && get_poc_diff(bits, ref_poc[6], frm_hdr.frame_offset) > 0) {
      at(mfmv_ref_, n_mfmvs_++) = 6;  // altref
    }
    if (n_mfmvs_ < total && rp_ref_[1]) at(mfmv_ref_, n_mfmvs_++) = 1;  // last2

    for (int n = 0; n < n_mfmvs_; n++) {
      const int mref = at(mfmv_ref_, n);
      const int rpoc = ref_poc[static_cast<uint64_t>(mref)];
      const int diff1 = get_poc_diff(bits, rpoc, frm_hdr.frame_offset);
      if (iabs(diff1) > 31) {
        at(mfmv_ref2cur_, n) = INVALID_REF2CUR;
      } else {
        at(mfmv_ref2cur_, n) = static_cast<int8_t>(mref < 4 ? -diff1 : diff1);
        for (int m = 0; m < 7; m++) {
          const int rrpoc = at(at(ref_ref_poc, mref), m);
          const int diff2 = get_poc_diff(bits, rpoc, rrpoc);
          // unsigned comparison also catches the < 0 case
          at(at(mfmv_ref2ref_, n), m) = static_cast<uint8_t>(static_cast<unsigned>(diff2) > 31U ? 0 : diff2);
        }
      }
    }
  }
  use_ref_frame_mvs_ = n_mfmvs_ > 0;
  return true;
}

ArrayView<RefmvsBlock> RefmvsFrame::row(const RefmvsTile& rt, int slot) const {
  const int row_index = at(rt.r, slot);
  if (row_index < 0) bounds_violation("RefmvsFrame::row", static_cast<uint64_t>(slot), 37);
  const uint64_t stride = static_cast<uint64_t>(rp_stride_) * 2;
  return r_.subview(static_cast<uint64_t>(row_index) * stride, stride);
}

void RefmvsFrame::tile_sbrow_init(RefmvsTile& rt, int tile_col_start4, int tile_col_end4, int tile_row_start4, int tile_row_end4,
                                  int sby) const {
  int32_t row = 0;
  const int sbsz = sbsz_;
  const int off = (sbsz * sby) & 16;
  for (int i = 0; i < sbsz; i++, row++) at(rt.r, off + 5 + i) = row;
  at(rt.r, off + 0) = row;
  row++;
  at(rt.r, off + 1) = -1;
  at(rt.r, off + 2) = row;
  row++;
  at(rt.r, off + 3) = -1;
  at(rt.r, off + 4) = row;
  if (sby & 1) {
    for (int k = 0; k <= 4; k += 2) {
      const int32_t tmp = at(rt.r, off + k);
      at(rt.r, off + k) = at(rt.r, off + sbsz + k);
      at(rt.r, off + sbsz + k) = tmp;
    }
  }

  rt.rf = this;
  rt.tile_row.start = tile_row_start4;
  rt.tile_row.end = imin(tile_row_end4, ih4_);
  rt.tile_col.start = tile_col_start4;
  rt.tile_col.end = imin(tile_col_end4, iw4_);
}

void RefmvsFrame::splat_mv(const RefmvsTile& rt, const RefmvsBlock& rmv, int by4, int bx4, int bw4, int bh4) const {
  for (int y = 0; y < bh4; y++) {
    const ArrayView<RefmvsBlock> r = row(rt, (by4 & 31) + 5 + y).subview(static_cast<uint64_t>(bx4), static_cast<uint64_t>(bw4));
    r.fill(rmv);
  }
}

void RefmvsFrame::load_tmvs(int col_start8, int col_end8, int row_start8, int row_end8) {
  if (row_start8 < 0 || row_end8 - row_start8 > 16) bounds_violation("RefmvsFrame::load_tmvs", static_cast<uint64_t>(row_end8), 16);
  row_end8 = imin(row_end8, ih8_);
  const int col_start8i = imax(col_start8 - 8, 0);
  const int col_end8i = imin(col_end8 + 8, iw8_);

  const int64_t stride = rp_stride_;
  int64_t clear = static_cast<int64_t>(row_start8 & 15) * stride;
  for (int y = row_start8; y < row_end8; y++) {
    for (int x = col_start8; x < col_end8; x++) rp_proj_[static_cast<uint64_t>(clear + x)].mv.n = INVALID_MV;
    clear += stride;
  }

  for (int n = 0; n < n_mfmvs_; n++) {
    const int ref2cur = at(mfmv_ref2cur_, n);
    if (ref2cur == INVALID_REF2CUR) continue;

    const int ref = at(mfmv_ref_, n);
    const int ref_sign = ref - 4;
    const ArrayView<RefmvsTemporalBlock> rp_ref = at(rp_ref_, ref)->blocks();
    int64_t r = static_cast<int64_t>(row_start8) * stride;
    for (int y = row_start8; y < row_end8; y++) {
      const int y_sb_align = y & ~7;
      const int y_proj_start = imax(y_sb_align, row_start8);
      const int y_proj_end = imin(y_sb_align + 8, row_end8);
      for (int x = col_start8i; x < col_end8i; x++) {
        const RefmvsTemporalBlock* rb = &rp_ref[static_cast<uint64_t>(r + x)];
        const int b_ref = rb->ref;
        if (!b_ref) continue;
        const int ref2ref = at(at(mfmv_ref2ref_, n), b_ref - 1);
        if (!ref2ref) continue;
        const Mv b_mv = rb->mv;
        const Mv offset = mv_projection(b_mv, ref2cur, ref2ref);
        int pos_x = x + apply_sign(iabs(offset.x) >> 6, offset.x ^ ref_sign);
        const int pos_y = y + apply_sign(iabs(offset.y) >> 6, offset.y ^ ref_sign);
        if (pos_y >= y_proj_start && pos_y < y_proj_end) {
          const int64_t pos = static_cast<int64_t>(pos_y & 15) * stride;
          for (;;) {
            const int x_sb_align = x & ~7;
            if (pos_x >= imax(x_sb_align - 8, col_start8) && pos_x < imin(x_sb_align + 16, col_end8)) {
              RefmvsTemporalBlock& dst = rp_proj_[static_cast<uint64_t>(pos + pos_x)];
              dst.mv = rb->mv;
              dst.ref = static_cast<uint8_t>(ref2ref);
            }
            if (++x >= col_end8i) break;
            rb = &rp_ref[static_cast<uint64_t>(r + x)];
            if (rb->ref != b_ref || rb->mv.n != b_mv.n) break;
            pos_x++;
          }
        } else {
          for (;;) {
            if (++x >= col_end8i) break;
            rb = &rp_ref[static_cast<uint64_t>(r + x)];
            if (rb->ref != b_ref || rb->mv.n != b_mv.n) break;
          }
        }
        x--;
      }
      r += stride;
    }
  }
}

void RefmvsFrame::save_tmvs(const RefmvsTile& rt, int col_start8, int col_end8, int row_start8, int row_end8) {
  if (!cur_ || row_start8 < 0 || row_end8 - row_start8 > 16) bounds_violation("RefmvsFrame::save_tmvs", static_cast<uint64_t>(row_end8), 16);
  row_end8 = imin(row_end8, ih8_);
  col_end8 = imin(col_end8, iw8_);

  const int64_t stride = rp_stride_;
  const ArrayView<RefmvsTemporalBlock> rp = cur_->blocks_;
  int64_t rp_row = static_cast<int64_t>(row_start8) * stride;
  for (int y = row_start8; y < row_end8; y++) {
    const ArrayView<RefmvsBlock> b = row(rt, 6 + (y & 15) * 2);

    for (int x = col_start8; x < col_end8;) {
      const RefmvsBlock& cand_b = b[static_cast<uint64_t>(x * 2 + 1)];
      const int bw8 = (k_block_dimensions[cand_b.bs < N_BS_SIZES ? cand_b.bs : 0][0] + 1) >> 1;

      RefmvsTemporalBlock tmv = {};
      if (cand_b.ref.ref[1] > 0 && at(mfmv_sign_, cand_b.ref.ref[1] - 1) &&
          (iabs(cand_b.mv.mv[1].y) | iabs(cand_b.mv.mv[1].x)) < 4096) {
        tmv.mv = cand_b.mv.mv[1];
        tmv.ref = static_cast<uint8_t>(cand_b.ref.ref[1]);
      } else if (cand_b.ref.ref[0] > 0 && at(mfmv_sign_, cand_b.ref.ref[0] - 1) &&
                 (iabs(cand_b.mv.mv[0].y) | iabs(cand_b.mv.mv[0].x)) < 4096) {
        tmv.mv = cand_b.mv.mv[0];
        tmv.ref = static_cast<uint8_t>(cand_b.ref.ref[0]);
      }
      // otherwise tmv stays all zero (no vector)
      for (int n = 0; n < bw8; n++, x++) rp[static_cast<uint64_t>(rp_row + x)] = tmv;
    }
    rp_row += stride;
  }
}

void RefmvsFrame::find(const RefmvsTile& rt, ArrayView<RefmvsCandidate> mvstack, int& cnt, int& ctx, RefmvsRefPair ref, BlockSize bs,
                       bool top_has_right, int by4, int bx4) const {
  const FrameHeader& hdr = *frm_hdr_;
  const uint8_t* const b_dim = k_block_dimensions[bs < N_BS_SIZES ? bs : 0];
  const int bw4 = b_dim[0], w4 = imin(imin(bw4, 16), rt.tile_col.end - bx4);
  const int bh4 = b_dim[1], h4 = imin(imin(bh4, 16), rt.tile_row.end - by4);
  if (mvstack.size() < 8 || ref.ref[0] < 0 || ref.ref[0] > 8 || ref.ref[1] < -1 || ref.ref[1] > 8) bounds_violation("RefmvsFrame::find", 8, mvstack.size());
  Mv gmv[2], tgmv[2];

  cnt = 0;
  if (ref.ref[0] > 0) {
    const WarpedMotionParams& wm = hdr.gmv[ref.ref[0] - 1];
    tgmv[0] = get_gmv_2d(wm, bx4, by4, bw4, bh4, hdr);
    gmv[0] = wm.type > WM_TYPE_TRANSLATION ? tgmv[0] : make_mv_n(INVALID_MV);
  } else {
    tgmv[0] = make_mv_n(0);
    gmv[0] = make_mv_n(INVALID_MV);
  }
  tgmv[1] = make_mv_n(0);
  gmv[1] = make_mv_n(INVALID_MV);
  if (ref.ref[1] > 0) {
    const WarpedMotionParams& wm = hdr.gmv[ref.ref[1] - 1];
    tgmv[1] = get_gmv_2d(wm, bx4, by4, bw4, bh4, hdr);
    gmv[1] = wm.type > WM_TYPE_TRANSLATION ? tgmv[1] : make_mv_n(INVALID_MV);
  }

  const int slot_cur = (by4 & 31) + 5;

  // top
  int have_newmv = 0, have_col_mvs = 0, have_row_mvs = 0;
  uint32_t max_rows = 0, n_rows = ~0U;
  ArrayView<const RefmvsBlock> b_top;
  if (by4 > rt.tile_row.start) {
    max_rows = static_cast<uint32_t>(imin((by4 - rt.tile_row.start + 1) >> 1, 2 + (bh4 > 1)));
    b_top = row(rt, slot_cur - 1);
    n_rows = static_cast<uint32_t>(scan_row(mvstack, cnt, ref, gmv, b_top, bx4, bw4, w4, static_cast<int>(max_rows), bw4 >= 16 ? 4 : 1,
                                            have_newmv, have_row_mvs));
  }

  // left
  uint32_t max_cols = 0, n_cols = ~0U;
  if (bx4 > rt.tile_col.start) {
    max_cols = static_cast<uint32_t>(imin((bx4 - rt.tile_col.start + 1) >> 1, 2 + (bw4 > 1)));
    n_cols = static_cast<uint32_t>(scan_col(*this, rt, mvstack, cnt, ref, gmv, slot_cur, bh4, h4, bx4 - 1, static_cast<int>(max_cols),
                                            bh4 >= 16 ? 4 : 1, have_newmv, have_col_mvs));
  }

  // top/right
  if (n_rows != ~0U && top_has_right && imax(bw4, bh4) <= 16 && bw4 + bx4 < rt.tile_col.end) {
    add_spatial_candidate(mvstack, cnt, 4, b_top[static_cast<uint64_t>(bx4 + bw4)], ref, gmv, have_newmv, have_row_mvs);
  }

  const int nearest_match = have_col_mvs + have_row_mvs;
  const int nearest_cnt = cnt;
  for (int n = 0; n < nearest_cnt; n++) mvstack[n].weight += 640;

  // temporal
  int globalmv_ctx = hdr.use_ref_frame_mvs;
  if (use_ref_frame_mvs_) {
    const int64_t stride = rp_stride_;
    const int by8 = by4 >> 1, bx8 = bx4 >> 1;
    const int64_t rbi = static_cast<int64_t>(by8 & 15) * stride + bx8;
    int64_t rb = rbi;
    const int step_h = bw4 >= 16 ? 2 : 1, step_v = bh4 >= 16 ? 2 : 1;
    const int w8 = imin((w4 + 1) >> 1, 8), h8 = imin((h4 + 1) >> 1, 8);
    // Runs of bit-identical records are merged into one call of weight 2 * length (see dav1d's comment: add_temporal_candidate is
    // idempotent apart from the weight). Two INVALID_MV records are identical whatever their .ref holds.
    const auto cell = [&](int64_t index) -> const RefmvsTemporalBlock& { return rp_proj_[static_cast<uint64_t>(index)]; };
    int64_t run = rb;
    int run_len = 1;
    int* run_gmv_ctx = &globalmv_ctx;
    for (int y = 0; y < h8; y += step_v) {
      for (int x = y ? 0 : step_h; x < w8; x += step_h) {
        const uint32_t mvn = cell(rb + x).mv.n;
        if (mvn == cell(run).mv.n && (mvn == INVALID_MV || cell(rb + x).ref == cell(run).ref)) {
          run_len++;
          continue;
        }
        add_temporal_candidate(hdr, pocdiff_, mvstack, cnt, cell(run), ref, run_gmv_ctx, tgmv[0], 2 * run_len);
        run_gmv_ctx = nullptr;
        run = rb + x;
        run_len = 1;
      }
      rb += stride * step_v;
    }
    add_temporal_candidate(hdr, pocdiff_, mvstack, cnt, cell(run), ref, run_gmv_ctx, tgmv[0], 2 * run_len);
    if (imin(bw4, bh4) >= 2 && imax(bw4, bh4) < 16) {
      const int bh8 = bh4 >> 1, bw8 = bw4 >> 1;
      rb = rbi + bh8 * stride;
      const int has_bottom = by8 + bh8 < imin(rt.tile_row.end >> 1, (by8 & ~7) + 8);
      if (has_bottom && bx8 - 1 >= imax(rt.tile_col.start >> 1, bx8 & ~7)) {
        add_temporal_candidate(hdr, pocdiff_, mvstack, cnt, cell(rb - 1), ref, nullptr, tgmv[0], 2);
      }
      if (bx8 + bw8 < imin(rt.tile_col.end >> 1, (bx8 & ~7) + 8)) {
        if (has_bottom) add_temporal_candidate(hdr, pocdiff_, mvstack, cnt, cell(rb + bw8), ref, nullptr, tgmv[0], 2);
        if (by8 + bh8 - 1 < imin(rt.tile_row.end >> 1, (by8 & ~7) + 8)) {
          add_temporal_candidate(hdr, pocdiff_, mvstack, cnt, cell(rb + bw8 - stride), ref, nullptr, tgmv[0], 2);
        }
      }
    }
  }

  // top/left (which, confusingly, is part of "secondary" references)
  int have_dummy_newmv_match;
  if ((n_rows | n_cols) != ~0U) {
    add_spatial_candidate(mvstack, cnt, 4, b_top[static_cast<uint64_t>(bx4 - 1)], ref, gmv, have_dummy_newmv_match, have_row_mvs);
  }

  // "secondary" (non-direct neighbour) top & left edges
  // what is different about secondary is that everything is now in 8x8 resolution
  for (int n = 2; n <= 3; n++) {
    if (static_cast<uint32_t>(n) > n_rows && static_cast<uint32_t>(n) <= max_rows) {
      n_rows += static_cast<uint32_t>(scan_row(mvstack, cnt, ref, gmv, row(rt, (((by4 & 31) - 2 * n + 1) | 1) + 5), bx4 | 1, bw4, w4,
                                               1 + static_cast<int>(max_rows) - n, bw4 >= 16 ? 4 : 2, have_dummy_newmv_match,
                                               have_row_mvs));
    }
    if (static_cast<uint32_t>(n) > n_cols && static_cast<uint32_t>(n) <= max_cols) {
      n_cols += static_cast<uint32_t>(scan_col(*this, rt, mvstack, cnt, ref, gmv, ((by4 & 31) | 1) + 5, bh4, h4, (bx4 - n * 2 + 1) | 1,
                                               1 + static_cast<int>(max_cols) - n, bh4 >= 16 ? 4 : 2, have_dummy_newmv_match,
                                               have_col_mvs));
    }
  }

  const int ref_match_count = have_col_mvs + have_row_mvs;

  // context build-up
  int refmv_ctx = 0, newmv_ctx = 0;
  switch (nearest_match) {
    case 0:
      refmv_ctx = imin(2, ref_match_count);
      newmv_ctx = ref_match_count > 0;
      break;
    case 1:
      refmv_ctx = imin(ref_match_count * 3, 4);
      newmv_ctx = 3 - have_newmv;
      break;
    case 2:
      refmv_ctx = 5;
      newmv_ctx = 5 - have_newmv;
      break;
  }

  // sorting (nearest, then "secondary")
  int len = nearest_cnt;
  while (len) {
    int last = 0;
    for (int n = 1; n < len; n++) {
      if (mvstack[n - 1].weight < mvstack[n].weight) {
        const RefmvsCandidate tmp = mvstack[n - 1];
        mvstack[n - 1] = mvstack[n];
        mvstack[n] = tmp;
        last = n;
      }
    }
    len = last;
  }
  len = cnt;
  while (len > nearest_cnt) {
    int last = nearest_cnt;
    for (int n = nearest_cnt + 1; n < len; n++) {
      if (mvstack[n - 1].weight < mvstack[n].weight) {
        const RefmvsCandidate tmp = mvstack[n - 1];
        mvstack[n - 1] = mvstack[n];
        mvstack[n] = tmp;
        last = n;
      }
    }
    len = last;
  }

  const int left = -(bx4 + bw4 + 4) * 4 * 8;
  const int right = (iw4_ - bx4 + 4) * 4 * 8;
  const int top = -(by4 + bh4 + 4) * 4 * 8;
  const int bottom = (ih4_ - by4 + 4) * 4 * 8;

  if (ref.ref[1] > 0) {
    if (cnt < 2) {
      const int sign0 = at(sign_bias_, ref.ref[0] - 1);
      const int sign1 = at(sign_bias_, ref.ref[1] - 1);
      const int sz4 = imin(w4, h4);
      const ArrayView<RefmvsCandidate> same = mvstack.subview(static_cast<uint64_t>(cnt));
      int same_count[4] = {0, 0, 0, 0};

      // non-self references in top
      if (n_rows != ~0U) {
        for (int x = 0; x < sz4;) {
          const RefmvsBlock& cand_b = b_top[static_cast<uint64_t>(bx4 + x)];
          add_compound_extended_candidate(same, same_count, cand_b, sign0, sign1, ref, sign_bias_);
          x += k_block_dimensions[cand_b.bs < N_BS_SIZES ? cand_b.bs : 0][0];
        }
      }

      // non-self references in left
      if (n_cols != ~0U) {
        for (int y = 0; y < sz4;) {
          const RefmvsBlock& cand_b = row(rt, slot_cur + y)[static_cast<uint64_t>(bx4 - 1)];
          add_compound_extended_candidate(same, same_count, cand_b, sign0, sign1, ref, sign_bias_);
          y += k_block_dimensions[cand_b.bs < N_BS_SIZES ? cand_b.bs : 0][1];
        }
      }

      // merge together
      for (int n = 0; n < 2; n++) {
        int m = same_count[n];
        if (m >= 2) continue;

        const int l = same_count[2 + n];
        if (l) {
          same[m].mv.mv[n] = same[2].mv.mv[n];
          if (++m == 2) continue;
          if (l == 2) {
            same[1].mv.mv[n] = same[3].mv.mv[n];
            continue;
          }
        }
        do {
          same[m].mv.mv[n] = tgmv[n];
        } while (++m < 2);
      }

      // if the first extended was the same as the non-extended one, then replace it with the second extended one
      int n = cnt;
      if (n == 1 && mvstack[0].mv == same[0].mv) mvstack[1].mv = mvstack[2].mv;
      do {
        mvstack[n].weight = 2;
      } while (++n < 2);
      cnt = 2;
    }

    // clamping
    const int n_refmvs = cnt;
    int n = 0;
    do {
      RefmvsCandidate& c = mvstack[n];
      c.mv.mv[0] = make_mv(iclip(c.mv.mv[0].y, top, bottom), iclip(c.mv.mv[0].x, left, right));
      c.mv.mv[1] = make_mv(iclip(c.mv.mv[1].y, top, bottom), iclip(c.mv.mv[1].x, left, right));
    } while (++n < n_refmvs);

    switch (refmv_ctx >> 1) {
      case 0:
        ctx = imin(newmv_ctx, 1);
        break;
      case 1:
        ctx = 1 + imin(newmv_ctx, 3);
        break;
      case 2:
        ctx = iclip(3 + newmv_ctx, 4, 7);
        break;
    }
    return;
  } else if (cnt < 2 && ref.ref[0] > 0) {
    const int sign = at(sign_bias_, ref.ref[0] - 1);
    const int sz4 = imin(w4, h4);

    // non-self references in top
    if (n_rows != ~0U) {
      for (int x = 0; x < sz4 && cnt < 2;) {
        const RefmvsBlock& cand_b = b_top[static_cast<uint64_t>(bx4 + x)];
        add_single_extended_candidate(mvstack, cnt, cand_b, sign, sign_bias_);
        x += k_block_dimensions[cand_b.bs < N_BS_SIZES ? cand_b.bs : 0][0];
      }
    }

    // non-self references in left
    if (n_cols != ~0U) {
      for (int y = 0; y < sz4 && cnt < 2;) {
        const RefmvsBlock& cand_b = row(rt, slot_cur + y)[static_cast<uint64_t>(bx4 - 1)];
        add_single_extended_candidate(mvstack, cnt, cand_b, sign, sign_bias_);
        y += k_block_dimensions[cand_b.bs < N_BS_SIZES ? cand_b.bs : 0][1];
      }
    }
  }

  // clamping
  const int n_refmvs = cnt;
  for (int n = 0; n < n_refmvs; n++) {
    Mv& m = mvstack[n].mv.mv[0];
    m = make_mv(iclip(m.y, top, bottom), iclip(m.x, left, right));
  }

  for (int n = cnt; n < 2; n++) mvstack[n].mv.mv[0] = tgmv[0];

  ctx = (refmv_ctx << 4) | (globalmv_ctx << 3) | newmv_ctx;
}

}  // namespace av1
}  // namespace hardware
