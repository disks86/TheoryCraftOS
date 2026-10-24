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
#include "Hardware/VideoCodec/Software/Av1FilmGrain.hpp"
#include "Hardware/VideoCodec/Software/Av1SuperRes.hpp"
#include "Hardware/VideoCodec/Software/Av1Warp.hpp"

// Frame level decoding of the AV1 decoder: picture allocation, reference slots, the state a frame inherits from its references (CDFs,
// segmentation map, motion vectors), the order of the in-loop filters and the film grain at output. Follows dav1d's decode.c
// (dav1d_submit_frame, dav1d_decode_frame_*), lib.c, picture.c, ref.c and lf_apply_tmpl.c / lr_apply_tmpl.c (BSD-2-Clause, Copyright (c)
// VideoLAN and dav1d authors, Two Orioles LLC), run on whole frames.

namespace hardware {
namespace av1 {
namespace {

inline int imin(int a, int b) { return a < b ? a : b; }

inline int iabs(int v) { return v < 0 ? -v : v; }

SharedPointer<Picture> make_picture(allocators::IAllocator& allocator, PixelLayout layout, int bpc, int width, int height) {
  SharedPointer<Picture> pic = SharedPointer<Picture>::make(allocator, allocator);
  if (!pic || !pic->allocate(layout, bpc, width, height)) return SharedPointer<Picture>();
  return pic;
}

bool has_grain(const FilmGrainData& fg) {
  return fg.num_y_points || fg.num_uv_points[0] || fg.num_uv_points[1] || (fg.clip_to_restricted_range && fg.chroma_scaling_from_luma);
}

}  // namespace

// ---- Picture ----------------------------------------------------------------------------------------------------------------------

bool Picture::allocate(PixelLayout layout, int bpc, int width, int height) {
  release_view(allocator_, memory_);
  for (ArrayView<Pixel>& p : planes_) p = ArrayView<Pixel>();
  if (width < 1 || height < 1 || width > 65536 || height > 65536 || (bpc != 8 && bpc != 10 && bpc != 12)) return false;
  layout_ = layout;
  bpc_ = bpc;
  width_ = width;
  height_ = height;
  const uint64_t aw = (static_cast<uint64_t>(width) + 127) & ~uint64_t{127};
  const uint64_t ah = (static_cast<uint64_t>(height) + 127) & ~uint64_t{127};
  const uint64_t luma = aw * ah;
  const bool chroma = layout != PIXEL_LAYOUT_I400;
  const uint64_t cw = aw >> ss_hor(), ch = ah >> ss_ver();
  const uint64_t chroma_size = chroma ? cw * ch : 0;
  memory_ = allocate_view<Pixel>(allocator_, luma + 2 * chroma_size, 64);
  if (memory_.empty()) return false;
  memory_.fill(0);
  stride_[0] = static_cast<int64_t>(aw);
  stride_[1] = static_cast<int64_t>(cw);
  planes_[0] = memory_.subview(0, luma);
  if (chroma) {
    planes_[1] = memory_.subview(luma, chroma_size);
    planes_[2] = memory_.subview(luma + chroma_size, chroma_size);
  }
  return true;
}

bool Picture::swap_storage(Picture& other) {
  if (width_ != other.width_ || height_ != other.height_ || bpc_ != other.bpc_ || layout_ != other.layout_) return false;
  ArrayView<Pixel> m = memory_;
  memory_ = other.memory_;
  other.memory_ = m;
  for (int i = 0; i < 3; i++) {
    ArrayView<Pixel> p = planes_[i];
    planes_[i] = other.planes_[i];
    other.planes_[i] = p;
  }
  return true;
}

// ---- FrameContext / Core -------------------------------------------------------------------------------------------------------------

FrameContext::FrameContext(allocators::IAllocator& allocator)
    : a(allocator),
      ipred_edge{OwnedArray<Pixel>(allocator), OwnedArray<Pixel>(allocator), OwnedArray<Pixel>(allocator)},
      rf(allocator),
      lfm(allocator),
      lr_units{OwnedArray<RestorationUnit>(allocator), OwnedArray<RestorationUnit>(allocator), OwnedArray<RestorationUnit>(allocator)} {}

Core::Core(allocators::IAllocator& allocator) : allocator_(allocator), parser_(allocator), f_(allocator), wedges_(allocator) {}

Core::~Core() {}

void Core::drop_slot(unsigned i) {
  refs_[i].pic.reset();
  refs_[i].segmap.reset();
  refs_[i].mvs.reset();
  refs_[i].cdf.reset();
}

void Core::finish_frame(bool ok) {
  FrameContext& f = f_;
  f.cur.reset();
  f.sr_cur.reset();
  for (SharedPointer<Picture>& p : f.refp) p.reset();
  f.mvs.reset();
  f.cur_segmap.reset();
  f.prev_segmap.reset();
  f.in_cdf.reset();
  f.out_cdf.reset();
}

void Core::new_sequence() {
  if (offload_) offload_->new_sequence();
  for (unsigned i = 0; i < NUM_REF_FRAMES; i++) drop_slot(i);
  out_.reset();
  grain_.reset();
  grain_valid_ = false;
}

void Core::reset() {
  parser_.reset();
  new_sequence();
  finish_frame(false);
}

// ---- frame set-up --------------------------------------------------------------------------------------------------------------------

bool Core::init_frame(const ObuParser& parser) {
  FrameContext& f = f_;
  const SequenceHeader* seq_in = parser.sequence_header();
  const FrameHeader* hdr_in = parser.frame_header();
  if (!seq_in || !hdr_in) return false;
  f.seq_hdr_store = *seq_in;
  f.frame_hdr_store = *hdr_in;
  f.seq_hdr = &f.seq_hdr_store;
  f.frame_hdr = &f.frame_hdr_store;
  const SequenceHeader& seq = f.seq_hdr_store;
  FrameHeader& hdr = f.frame_hdr_store;
  const int bpc = 8 + 2 * seq.hbd;
  const bool inter = is_inter_or_switch(hdr);

  f.layout = seq.layout;
  f.ss_hor = seq.layout != PIXEL_LAYOUT_I444;
  f.ss_ver = seq.layout == PIXEL_LAYOUT_I420;
  f.bitdepth_max = (1 << bpc) - 1;
  if (!wedges_.ready() && !wedges_.init()) return false;
  if (!t_) t_ = UniquePointer<TaskContext>::make(allocator_);
  if (!ts_) ts_ = UniquePointer<TileState>::make(allocator_);
  if (!t_ || !ts_) return false;

  // references
  for (int i = 0; i < 7; i++) {
    f.svc[i][0] = f.svc[i][1] = ScalableMotionParams{0, 0};
    f.gmv_warp_allowed[i] = 0;
    f.refpoc[i] = 0;
  }
  if (inter) {
    if (hdr.primary_ref_frame != PRIMARY_REF_NONE) {
      const int pri_ref = hdr.refidx[hdr.primary_ref_frame];
      if (!refs_[pri_ref].pic) return false;
    }
    for (int i = 0; i < 7; i++) {
      const int refidx = hdr.refidx[i];
      const SharedPointer<Picture>& rp = refs_[refidx].pic;
      if (!rp || hdr.width[0] * 2 < rp->width() || hdr.height * 2 < rp->height() || hdr.width[0] > rp->width() * 16 ||
          hdr.height > rp->height() * 16 || rp->layout() != seq.layout || rp->bpc() != bpc)
        return false;
      f.refp[i] = rp;
      if (hdr.width[0] != rp->width() || hdr.height != rp->height()) {
        const int rw = rp->width(), rh = rp->height();
        f.svc[i][0].scale = static_cast<int>(((static_cast<int64_t>(rw) << 14) + (hdr.width[0] >> 1)) / hdr.width[0]);
        f.svc[i][1].scale = static_cast<int>(((static_cast<int64_t>(rh) << 14) + (hdr.height >> 1)) / hdr.height);
        f.svc[i][0].step = (f.svc[i][0].scale + 8) >> 4;
        f.svc[i][1].step = (f.svc[i][1].scale + 8) >> 4;
      }
      f.gmv_warp_allowed[i] = hdr.gmv[i].type > WM_TYPE_TRANSLATION && !hdr.force_integer_mv && !get_shear_params(&hdr.gmv[i]) && !f.svc[i][0].scale;
    }
  }

  // entropy
  if (hdr.primary_ref_frame == PRIMARY_REF_NONE) {
    f.in_cdf = SharedPointer<CdfContext>::make(allocator_);
    if (!f.in_cdf) return false;
    cdf_load_default(*f.in_cdf, cdf_qcat(hdr.quant.yac));
  } else {
    f.in_cdf = refs_[hdr.refidx[hdr.primary_ref_frame]].cdf;
    if (!f.in_cdf) return false;
  }
  if (hdr.refresh_context) {
    f.out_cdf = SharedPointer<CdfContext>::make(allocator_);
    if (!f.out_cdf) return false;
    *f.out_cdf = *f.in_cdf;
  }

  // pictures
  f.sr_cur = make_picture(allocator_, seq.layout, bpc, hdr.width[1], hdr.height);
  if (!f.sr_cur) return false;
  if (hdr.width[0] != hdr.width[1]) {
    f.cur = make_picture(allocator_, seq.layout, bpc, hdr.width[0], hdr.height);
    if (!f.cur) return false;
  } else {
    f.cur = f.sr_cur;
  }

  f.w4 = (hdr.width[0] + 3) >> 2;
  f.h4 = (hdr.height + 3) >> 2;
  f.bw = ((hdr.width[0] + 7) >> 3) << 1;
  f.bh = ((hdr.height + 7) >> 3) << 1;
  f.sb128w = (f.bw + 31) >> 5;
  f.sb128h = (f.bh + 31) >> 5;
  f.sb_shift = 4 + seq.sb128;
  f.sb_step = 16 << seq.sb128;
  f.sbh = (f.bh + f.sb_step - 1) >> f.sb_shift;
  f.b4_stride = (f.bw + 31) & ~31;
  f.sr_sb128w = (hdr.width[1] + 127) >> 7;

  // motion vectors
  f.mvs.reset();
  if (inter || hdr.allow_intrabc) {
    f.mvs = SharedPointer<MotionVectorStore>::make(allocator_, allocator_);
    if (!f.mvs || !f.mvs->allocate(hdr.width[0], hdr.height)) return false;
    if (!hdr.allow_intrabc)
      for (int i = 0; i < 7; i++) f.refpoc[i] = f.refp[i]->hdr.frame_offset;
    const MotionVectorStore* ref_stores[7] = {};
    if (hdr.use_ref_frame_mvs && inter) {
      for (int i = 0; i < 7; i++) {
        const SharedPointer<MotionVectorStore>& m = refs_[hdr.refidx[i]].mvs;
        ref_stores[i] = m ? m.get() : nullptr;
      }
    }
    if (!f.rf.init_frame(seq, hdr, ArrayView<const uint8_t>(f.refpoc), f.mvs.get(), ArrayView<const MotionVectorStore* const>(ref_stores)))
      return false;
  }

  // segmentation map
  f.cur_segmap.reset();
  f.prev_segmap.reset();
  if (hdr.segmentation.enabled) {
    if ((hdr.segmentation.temporal || !hdr.segmentation.update_map) && hdr.primary_ref_frame != PRIMARY_REF_NONE && inter) {
      const int pri_ref = hdr.primary_ref_frame;
      const Picture& rp = *f.refp[pri_ref];
      const int ref_w = ((rp.hdr.width[0] + 7) >> 3) << 1;
      const int ref_h = ((rp.height() + 7) >> 3) << 1;
      if (ref_w == f.bw && ref_h == f.bh) f.prev_segmap = refs_[hdr.refidx[pri_ref]].segmap;
    }
    if (hdr.segmentation.update_map || !f.prev_segmap) {
      f.cur_segmap = SharedPointer<SegmentMap>::make(allocator_, allocator_);
      if (!f.cur_segmap || !f.cur_segmap->data.reset(static_cast<uint64_t>(f.b4_stride) * 32 * static_cast<uint64_t>(f.sb128h))) return false;
    } else {
      f.cur_segmap = f.prev_segmap;
    }
  }

  // contexts and per frame buffers
  if (!f.a.reset(static_cast<uint64_t>(f.sb128w) * hdr.tiling.rows)) return false;
  for (BlockContext& c : f.a.view()) reset_block_context(c, is_key_or_intra(hdr));
  const uint64_t edge_size = static_cast<uint64_t>(f.sbh) * static_cast<uint64_t>(f.sb128w) * 128;
  for (int p = 0; p < (seq.layout == PIXEL_LAYOUT_I400 ? 1 : 3); p++)
    if (!f.ipred_edge[p].reset(edge_size)) return false;
  if (!f.lfm.init(seq, hdr)) return false;

  f.restore_planes = 0;
  for (int p = 0; p < 3; p++) {
    f.lr_rows[p] = f.lr_cols[p] = 0;
    if (hdr.restoration.type[p] == RESTORATION_NONE || (p && seq.layout == PIXEL_LAYOUT_I400)) continue;
    f.restore_planes |= 1 << p;
    const int ss_h = p && f.ss_hor, ss_v = p && f.ss_ver;
    const int pw = (hdr.width[1] + ss_h) >> ss_h, ph = (hdr.height + ss_v) >> ss_v;
    const int log2 = hdr.restoration.unit_size[p != 0];
    f.lr_rows[p] = lr_unit_count(ph, log2);
    f.lr_cols[p] = lr_unit_count(pw, log2);
    if (!f.lr_units[p].reset(static_cast<uint64_t>(f.lr_rows[p]) * static_cast<uint64_t>(f.lr_cols[p]))) return false;
  }

  // dequantizers
  init_quant_tables(seq, hdr, hdr.quant.yac, f.dq);
  for (int i = 0; i < N_RECT_TX_SIZES; i++) {
    if (hdr.quant.qm) {
      f.qm[i][0] = qm_table(hdr.quant.qm_y, 0, i);
      f.qm[i][1] = qm_table(hdr.quant.qm_u, 1, i);
      f.qm[i][2] = qm_table(hdr.quant.qm_v, 1, i);
    } else {
      for (int pl = 0; pl < 3; pl++) f.qm[i][pl] = ArrayView<const uint8_t>();
    }
  }

  // jnt_comp weights
  if (hdr.switchable_comp_refs && inter) {
    for (int i = 0; i < 7; i++) {
      const unsigned ref0poc = f.refp[i]->hdr.frame_offset;
      for (int j = i + 1; j < 7; j++) {
        const unsigned ref1poc = f.refp[j]->hdr.frame_offset;
        const unsigned d1 = static_cast<unsigned>(imin(iabs(get_poc_diff(seq.order_hint_n_bits, static_cast<int>(ref0poc), hdr.frame_offset)), 31));
        const unsigned d0 = static_cast<unsigned>(imin(iabs(get_poc_diff(seq.order_hint_n_bits, static_cast<int>(ref1poc), hdr.frame_offset)), 31));
        const int order = d0 <= d1;
        static const uint8_t quant_dist_weight[3][2] = {{2, 3}, {2, 5}, {2, 7}};
        static const uint8_t quant_dist_lookup_table[4][2] = {{9, 7}, {11, 5}, {12, 4}, {13, 3}};
        int k;
        for (k = 0; k < 3; k++) {
          const int c0 = quant_dist_weight[k][order];
          const int c1 = quant_dist_weight[k][!order];
          const int d0_c0 = static_cast<int>(d0) * c0;
          const int d1_c1 = static_cast<int>(d1) * c1;
          if ((d0 > d1 && d0_c0 < d1_c1) || (d0 <= d1 && d0_c0 > d1_c1)) break;
        }
        f.jnt_weights[i][j] = quant_dist_lookup_table[k][order];
      }
    }
  }

  // the pictures remember the (completed) header
  f.cur->hdr = hdr;
  f.cur->matrix_identity = seq.mtrx == MC_IDENTITY;
  f.sr_cur->hdr = hdr;
  f.sr_cur->matrix_identity = seq.mtrx == MC_IDENTITY;
  return true;
}

// ---- tiles ---------------------------------------------------------------------------------------------------------------------------

bool Core::decode_tiles(const ObuParser& parser) {
  FrameContext& f = f_;
  const FrameHeader& hdr = *f.frame_hdr;
  const ArrayView<const TileData> tiles = parser.tiles();
  const int cols = hdr.tiling.cols, rows = hdr.tiling.rows;
  if (tiles.size() < static_cast<uint64_t>(cols) * static_cast<uint64_t>(rows)) return false;
  TaskContext& t = *t_;
  TileState& ts = *ts_;
  t.f = &f;
  t.ts = &ts;
  for (int tile_row = 0; tile_row < rows; tile_row++) {
    for (int tile_col = 0; tile_col < cols; tile_col++) {
      const int idx = tile_row * cols + tile_col;
      if (!setup_tile(ts, tile_row, tile_col, tiles[static_cast<uint64_t>(idx)].data)) return false;
      if (!decode_tile(t, tile_row, tile_col)) return false;
      if (idx == hdr.tiling.update && hdr.refresh_context) cdf_update(hdr, *f.out_cdf, ts.cdf);
    }
  }
  return true;
}

// ---- in-loop filters --------------------------------------------------------------------------------------------------------------

bool Core::post_filter() {
  FrameContext& f = f_;
  const SequenceHeader& seq = *f.seq_hdr;
  const FrameHeader& hdr = *f.frame_hdr;
  const int bitdepth = 8 + 2 * seq.hbd;
  const int planes = seq.layout == PIXEL_LAYOUT_I400 ? 1 : 3;
  const bool superres = hdr.width[0] != hdr.width[1];
  const bool lr = f.restore_planes != 0;
  Picture& D = *f.cur;  // the decoded frame, deblocked in place

  PixelView dv[3] = {D.plane(0), D.plane(1), D.plane(2)};
  f.lfm.loop_filter_frame(dv[0], dv[1], dv[2], bitdepth);

  bool cdef_active = false;
  if (seq.cdef) {
    for (int i = 0; i < (1 << hdr.cdef.n_bits); i++) cdef_active |= hdr.cdef.y_strength[i] || hdr.cdef.uv_strength[i];
  }

  SharedPointer<Picture> cdef_pic;  // the CDEF output at the coded size
  if (cdef_active) {
    cdef_pic = make_picture(allocator_, seq.layout, 8 + 2 * seq.hbd, hdr.width[0], hdr.height);
    if (!cdef_pic) return false;
    PixelView cv[3] = {cdef_pic->plane(0), cdef_pic->plane(1), cdef_pic->plane(2)};
    for (int p = 0; p < planes; p++) cv[p].plane.copy_from(dv[p].plane);
    cdef_frame(f.lfm, seq, hdr, dv, cv, bitdepth);
  }

  Picture& final_pic = *f.sr_cur;
  auto lr_plane = [&](int p, PixelView dst, PixelView src, PixelView deblocked) {
    LrPlane lp;
    lp.width = final_pic.plane_width(p);
    lp.height = final_pic.plane_height(p);
    lp.ss_ver = p && seq.layout == PIXEL_LAYOUT_I420;
    lp.unit_size_log2 = hdr.restoration.unit_size[p != 0];
    lp.bitdepth_max = f.bitdepth_max;
    lp.units = ArrayView<const RestorationUnit>(f.lr_units[p].view());
    return loop_restoration_plane(allocator_, dst, src, deblocked, lp);
  };

  if (superres) {
    SharedPointer<Picture> deblocked_up;
    if (lr) {
      deblocked_up = make_picture(allocator_, seq.layout, bitdepth, hdr.width[1], hdr.height);
      if (!deblocked_up) return false;
    }
    for (int p = 0; p < planes; p++) {
      const int ss_h = p && f.ss_hor;
      const int in_w = D.plane_width(p), out_w = final_pic.plane_width(p);
      const int src_w = super_res_src_width(hdr.width[0], ss_h);
      const SuperResStep step = super_res_step(in_w, out_w);
      const Picture& c = cdef_active ? *cdef_pic : D;
      if (!upscale_plane(final_pic.plane(p), c.plane(p), out_w, src_w, final_pic.plane_height(p), step, f.bitdepth_max)) return false;
      if (lr && ((f.restore_planes >> p) & 1)) {
        if (!upscale_plane(deblocked_up->plane(p), D.plane(p), out_w, src_w, final_pic.plane_height(p), step, f.bitdepth_max)) return false;
        const PixelView src = cdef_active ? final_pic.plane(p) : deblocked_up->plane(p);
        if (!lr_plane(p, final_pic.plane(p), src, deblocked_up->plane(p))) return false;
      }
    }
    return true;
  }

  // No super-resolution: final_pic is D (decoded in place).
  if (cdef_active) {
    // sr_cur takes the CDEF output; cdef_pic keeps the deblocked samples for the stripe boundaries of the restoration.
    if (!final_pic.swap_storage(*cdef_pic)) return false;
    if (lr)
      for (int p = 0; p < planes; p++)
        if ((f.restore_planes >> p) & 1)
          if (!lr_plane(p, final_pic.plane(p), final_pic.plane(p), cdef_pic->plane(p))) return false;
    return true;
  }
  if (lr) {
    SharedPointer<Picture> restored = make_picture(allocator_, seq.layout, bitdepth, hdr.width[0], hdr.height);
    if (!restored) return false;
    for (int p = 0; p < planes; p++) restored->plane(p).plane.copy_from(final_pic.plane(p).plane);
    for (int p = 0; p < planes; p++)
      if ((f.restore_planes >> p) & 1)
        if (!lr_plane(p, restored->plane(p), final_pic.plane(p), final_pic.plane(p))) return false;
    return final_pic.swap_storage(*restored);
  }
  return true;
}

// ---- frames ---------------------------------------------------------------------------------------------------------------------------

// The frame goes to the offload: the picture is made here (all the output and the references need of it is its samples), the engine fills it.
bool Core::decode_frame_offloaded() {
  const SequenceHeader* seq = parser_.sequence_header();
  const FrameHeader* hdr_in = parser_.frame_header();
  if (!seq || !hdr_in) return false;
  f_.seq_hdr_store = *seq;
  f_.frame_hdr_store = *hdr_in;
  f_.seq_hdr = &f_.seq_hdr_store;
  f_.frame_hdr = &f_.frame_hdr_store;
  const FrameHeader& hdr = f_.frame_hdr_store;
  const int bpc = 8 + 2 * seq->hbd;
  f_.sr_cur = make_picture(allocator_, seq->layout, bpc, hdr.width[1], hdr.height);
  bool ok = static_cast<bool>(f_.sr_cur);
  if (ok) {
    f_.sr_cur->hdr = hdr;
    f_.sr_cur->matrix_identity = seq->mtrx == MC_IDENTITY;
    OffloadFrame o;
    o.seq = &f_.seq_hdr_store;
    o.hdr = &f_.frame_hdr_store;
    o.tiles = parser_.tiles();
    o.width = hdr.width[1];
    o.height = hdr.height;
    for (int p = 0; p < (seq->layout == PIXEL_LAYOUT_I400 ? 1 : 3); ++p) o.plane[p] = f_.sr_cur->plane(p);
    if (!offload_->decode_frame(o)) {
      // Damaged or failed: mid-grey, as the CPU shows what it cannot decode.
      for (int p = 0; p < (seq->layout == PIXEL_LAYOUT_I400 ? 1 : 3); ++p)
        for (int y = 0; y < f_.sr_cur->plane_height(p); ++y)
          for (int x = 0; x < f_.sr_cur->plane_width(p); ++x) o.plane[p].at(x, y) = static_cast<Pixel>(1 << (bpc - 1));
    }
    for (unsigned i = 0; i < NUM_REF_FRAMES; i++) {
      if (!(hdr.refresh_frame_flags & (1u << i))) continue;
      RefSlot& slot = refs_[i];
      slot.pic = f_.sr_cur;
      slot.segmap.reset();   // (what the CPU decoder keeps for the next frame, which the engine keeps itself)
      slot.mvs.reset();
      slot.cdf.reset();
    }
    parser_.ref_update(hdr.refresh_frame_flags, hdr);
    if (hdr.show_frame) {
      out_ = f_.sr_cur;
      grain_valid_ = false;
    }
  }
  finish_frame(ok);
  return ok;
}

bool Core::decode_frame() {
  if (offload_ && parser_.sequence_header() && parser_.frame_header() && offload_->accepts(*parser_.sequence_header(), *parser_.frame_header()))
    return decode_frame_offloaded();
  bool ok = init_frame(parser_) && decode_tiles(parser_) && post_filter();
  const FrameHeader& hdr = f_.frame_hdr_store;
  if (ok) {
    for (unsigned i = 0; i < NUM_REF_FRAMES; i++) {
      if (!(hdr.refresh_frame_flags & (1u << i))) continue;
      RefSlot& slot = refs_[i];
      slot.pic = f_.sr_cur;
      slot.segmap = f_.cur_segmap;
      slot.mvs = hdr.allow_intrabc ? SharedPointer<MotionVectorStore>() : f_.mvs;
      slot.cdf = hdr.refresh_context ? f_.out_cdf : f_.in_cdf;
    }
    parser_.ref_update(hdr.refresh_frame_flags, hdr);
    if (hdr.show_frame) {
      out_ = f_.sr_cur;
      grain_valid_ = false;
    }
    if (observer_) observer_->frame_decoded(f_.seq_hdr_store, hdr, parser_.tiles(), *f_.sr_cur);
  } else {
    for (unsigned i = 0; i < NUM_REF_FRAMES; i++)
      if (hdr.refresh_frame_flags & (1u << i)) drop_slot(i);
  }
  finish_frame(ok);
  return ok;
}

bool Core::show_existing_frame(unsigned idx, bool refresh_all) {
  if (idx >= NUM_REF_FRAMES || !refs_[idx].pic) return false;
  if (offload_) offload_->show_existing(idx, refresh_all);
  out_ = refs_[idx].pic;
  grain_valid_ = false;
  if (refresh_all) {
    for (unsigned i = 0; i < NUM_REF_FRAMES; i++) {
      if (i == idx) continue;
      refs_[i].pic = refs_[idx].pic;
      refs_[i].segmap = refs_[idx].segmap;
      refs_[i].cdf = refs_[idx].cdf;
      refs_[i].mvs.reset();
    }
  }
  return true;
}

const Picture* Core::output() {
  if (!out_) return nullptr;
  const Picture& pic = *out_;
  const FilmGrainData& fg = pic.hdr.film_grain.data;
  if (!pic.hdr.film_grain.present || !has_grain(fg)) return out_.get();
  if (grain_valid_ && grain_) return grain_.get();
  if (!grain_ || grain_->width() != pic.width() || grain_->height() != pic.height() || grain_->layout() != pic.layout() || grain_->bpc() != pic.bpc()) {
    grain_ = make_picture(allocator_, pic.layout(), pic.bpc(), pic.width(), pic.height());
    if (!grain_) return nullptr;
  }
  FilmGrainPicture src, dst;
  for (int p = 0; p < (pic.layout() == PIXEL_LAYOUT_I400 ? 1 : 3); p++) {
    src.plane[p] = pic.plane(p);
    dst.plane[p] = grain_->plane(p);
  }
  src.width = dst.width = pic.width();
  src.height = dst.height = pic.height();
  src.layout = dst.layout = pic.layout();
  src.bitdepth = dst.bitdepth = pic.bpc();
  if (!apply_film_grain(fg, pic.matrix_identity, src, dst)) return nullptr;
  grain_->hdr = pic.hdr;
  grain_valid_ = true;
  return grain_.get();
}

}  // namespace av1
}  // namespace hardware
