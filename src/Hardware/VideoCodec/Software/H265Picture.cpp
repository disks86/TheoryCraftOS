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


// H.265 decoding, part 2: the decoded picture buffer - picture order counts, reference picture sets, reference picture lists, and the order pictures are output in
// (clauses 8.1 to 8.3 and C.5 of ITU-T H.265).
#include "Hardware/VideoCodec/Software/H265Core.hpp"
#include "Hardware/VideoCodec/Software/H265Trace.hpp"
#include "Hardware/VideoCodec/Software/H265Trace.hpp"

namespace hardware {
namespace h265 {

namespace {
constexpr int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
}  // namespace

bool Core::alloc_picture(Picture* p) {
  const Sps& s = *sp_;
  const int cw = s.width / 2, ch = s.height / 2;
  if (!p->plane[0].alloc(allocator_, static_cast<uint64_t>(s.width) * s.height) || !p->plane[1].alloc(allocator_, static_cast<uint64_t>(cw) * ch) ||
      !p->plane[2].alloc(allocator_, static_cast<uint64_t>(cw) * ch) || !p->col.alloc(allocator_, static_cast<uint64_t>((s.width + 15) / 16) * ((s.height + 15) / 16)))
    return false;
  p->stride[0] = s.width;
  p->stride[1] = p->stride[2] = cw;
  p->width = s.width;
  p->height = s.height;
  p->crop_l = s.crop_l;
  p->crop_r = s.crop_r;
  p->crop_t = s.crop_t;
  p->crop_b = s.crop_b;
  return true;
}

Picture* Core::find_free_picture() {
  for (Picture& p : pics_)
    if (!p.in_use) return &p;
  return nullptr;
}

void Core::clear_dpb() {
  for (Picture& p : pics_) {
    p.in_use = p.is_ref = p.is_long = p.needed_for_output = p.force_out = p.generated = false;
  }
  returned_ = nullptr;
}

// 8.3.3: a grey picture standing in for a reference picture that is not there.
Picture* Core::generate_missing(int poc, bool long_term) {
  Picture* p = find_free_picture();
  if (!p) return nullptr;
  if (!alloc_picture(p)) return nullptr;
  for (int c = 0; c < 3; ++c)
    for (uint64_t i = 0; i < p->plane[c].size(); ++i) p->plane[c][i] = 128;
  for (uint64_t i = 0; i < p->col.size(); ++i) p->col[i].pred_flags = 0;
  p->poc = poc;
  p->in_use = true;
  p->is_ref = true;
  p->is_long = long_term;
  p->needed_for_output = false;
  p->generated = true;
  p->order = decode_counter_++;
  return p;
}

// 8.3.2: which pictures the current picture may refer to; every other picture stops being a reference.
bool Core::derive_rps(const SliceHeader& sh) {
  num_st_before_ = num_st_after_ = num_lt_curr_ = 0;
  const int max_lsb = 1 << sp_->log2_max_poc_lsb;
  if (is_irap(sh.nal_type) && no_rasl_output_) {
    for (Picture& p : pics_) {
      p.is_ref = false;
      p.is_long = false;
    }
  }
  if (is_idr(sh.nal_type)) {
    for (Picture& p : pics_) p.is_ref = p.is_long = false;
    return true;
  }
  // The pictures the sets name.
  bool keep[kMaxPics] = {};
  auto index_of = [&](const Picture* p) { return static_cast<int>(p - pics_); };
  auto find_by_poc = [&](int poc, bool lsb_only, bool need_ref) -> Picture* {
    for (Picture& p : pics_) {
      if (!p.in_use || (need_ref && !p.is_ref)) continue;
      if (lsb_only ? (p.poc & (max_lsb - 1)) == poc : p.poc == poc) return &p;
    }
    return nullptr;
  };
  Picture* lt_curr[16] = {};
  int lt_count = 0;
  struct Foll { Picture* pic; };
  // Long-term pictures first (they are looked for among all reference pictures), then the short-term ones among the short-term reference pictures.
  for (int i = 0; i < sh.num_lt; ++i) {
    int poc_lt = sh.lt_poc_lsb[i];
    if (sh.lt_msb_present[i]) poc_lt += poc_ - sh.lt_delta_poc_msb_cycle[i] * max_lsb - (poc_ & (max_lsb - 1));
    Picture* p = find_by_poc(poc_lt, !sh.lt_msb_present[i], true);
    if (sh.lt_used_by_curr[i]) {
      if (!p) p = generate_missing(poc_lt, true);
      if (!p) return false;
      if (lt_count >= 16) return false;
      lt_curr[lt_count++] = p;
    }
    if (p) {
      keep[index_of(p)] = true;
      p->is_long = true;
    }
  }
  for (int i = 0; i < sh.st_rps.num_negative; ++i) {
    const int poc = poc_ + sh.st_rps.delta_poc_s0[i];
    Picture* p = nullptr;
    for (Picture& c : pics_)
      if (c.in_use && c.is_ref && !c.is_long && c.poc == poc) p = &c;
    if (sh.st_rps.used_s0[i]) {
      if (!p) p = generate_missing(poc, false);
      if (!p) return false;
      rps_st_curr_before_[num_st_before_++] = p;
    }
    if (p) keep[index_of(p)] = true;
  }
  for (int i = 0; i < sh.st_rps.num_positive; ++i) {
    const int poc = poc_ + sh.st_rps.delta_poc_s1[i];
    Picture* p = nullptr;
    for (Picture& c : pics_)
      if (c.in_use && c.is_ref && !c.is_long && c.poc == poc) p = &c;
    if (sh.st_rps.used_s1[i]) {
      if (!p) p = generate_missing(poc, false);
      if (!p) return false;
      rps_st_curr_after_[num_st_after_++] = p;
    }
    if (p) keep[index_of(p)] = true;
  }
  for (int i = 0; i < lt_count; ++i) rps_lt_curr_[i] = lt_curr[i];
  num_lt_curr_ = lt_count;
  for (int i = 0; i < kMaxPics; ++i)
    if (!keep[i]) pics_[i].is_ref = pics_[i].is_long = false;
  return true;
}

// 8.3.4
bool Core::build_ref_lists(const SliceHeader& sh, SliceInfo* info) {
  for (int l = 0; l < 2; ++l)
    for (int i = 0; i < kMaxRefs; ++i) {
      info->ref_pic[l][i] = nullptr;
      info->ref_poc[l][i] = 0;
      info->ref_long[l][i] = false;
    }
  if (sh.type == kSliceI) return true;
  const int total = num_st_before_ + num_st_after_ + num_lt_curr_;
  if (total == 0 || total != sh.num_pic_total_curr) return false;
  for (int l = 0; l < (sh.type == kSliceB ? 2 : 1); ++l) {
    Picture* temp[48];
    bool temp_long[48];
    const int n = sh.num_ref_idx[l] > total ? sh.num_ref_idx[l] : total;
    int r = 0;
    while (r < n) {
      const int* dummy = nullptr;
      (void)dummy;
      for (int i = 0; i < (l == 0 ? num_st_before_ : num_st_after_) && r < n; ++i, ++r) {
        temp[r] = l == 0 ? rps_st_curr_before_[i] : rps_st_curr_after_[i];
        temp_long[r] = false;
      }
      for (int i = 0; i < (l == 0 ? num_st_after_ : num_st_before_) && r < n; ++i, ++r) {
        temp[r] = l == 0 ? rps_st_curr_after_[i] : rps_st_curr_before_[i];
        temp_long[r] = false;
      }
      for (int i = 0; i < num_lt_curr_ && r < n; ++i, ++r) {
        temp[r] = rps_lt_curr_[i];
        temp_long[r] = true;
      }
    }
    for (int i = 0; i < sh.num_ref_idx[l]; ++i) {
      const int k = sh.list_modification[l] ? sh.list_entry[l][i] : i;
      if (k >= n) return false;
      info->ref_pic[l][i] = temp[k];
      info->ref_poc[l][i] = temp[k]->poc;
      info->ref_long[l][i] = temp_long[k];
    }
  }
  return true;
}

// 8.3.1 and 8.1.3: order count, reference picture set and the picture to decode into.
bool Core::start_picture(const SliceHeader& sh, int64_t pts) {
  const Pps& pps = pps_[sh.pps_id];
  const Sps& sps = sps_[pps.sps_id];
  const bool irap = is_irap(sh.nal_type);
  if (irap) {
    no_rasl_output_ = is_idr(sh.nal_type) || is_bla(sh.nal_type) || first_picture_ || after_eos_;
    skip_rasl_ = no_rasl_output_;
  }
  if (first_picture_ && !irap) return H265_FALSE;      // decoding starts at a random access point
  if (is_rasl(sh.nal_type) && skip_rasl_) return H265_FALSE;   // its references are not there: it is not decoded or shown
  if (!irap && no_rasl_output_ && after_eos_) return H265_FALSE;

  const bool new_cvs = irap && no_rasl_output_;
  const bool size_change = sp_ && (sp_->width != sps.width || sp_->height != sps.height || sp_->log2_ctb != sps.log2_ctb);
  if (new_cvs) {
    // Pictures still waiting for output belong to the sequence that ends here: they go out first, unless the stream says to drop them.
    for (Picture& p : pics_) {
      if (!p.in_use) continue;
      if (sh.no_output_of_prior_pics && !first_picture_) p.needed_for_output = false;
      else if (p.needed_for_output) p.force_out = true;
    }
  }
  active_sps_ = sps_[pps.sps_id];
  active_pps_ = pps_[sh.pps_id];
  sp_ = &active_sps_;
  pp_ = &active_pps_;
  if (size_change) {
    // Pictures of another size cannot be used or shown any more.
    for (Picture& p : pics_)
      if (p.in_use && (p.width != sps.width || p.height != sps.height)) {
        p.is_ref = p.is_long = false;
        p.needed_for_output = false;
        p.force_out = false;
      }
  }

  // 8.3.1: picture order count
  const int max_lsb = 1 << sps.log2_max_poc_lsb;
  int msb = 0;
  if (!(irap && no_rasl_output_)) {
    const int lsb = sh.poc_lsb, prev_lsb = prev_tid0_poc_lsb_, prev_msb = prev_tid0_poc_msb_;
    if (lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2) msb = prev_msb + max_lsb;
    else if (lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2) msb = prev_msb - max_lsb;
    else msb = prev_msb;
  }
  poc_ = msb + sh.poc_lsb;
  if (sh.temporal_id == 0 && !is_rasl(sh.nal_type) && !is_radl(sh.nal_type) && !is_sub_layer_non_ref(sh.nal_type)) {
    prev_tid0_poc_lsb_ = sh.poc_lsb;
    prev_tid0_poc_msb_ = msb;
  }

  if (!setup_picture_state()) return H265_FALSE;
  if (!derive_rps(sh)) return H265_FALSE;
  // Free the pictures that are neither references nor waiting for output.
  for (Picture& p : pics_)
    if (p.in_use && !p.is_ref && !p.needed_for_output && &p != returned_) p.in_use = false;
  Picture* p = find_free_picture();
  if (!p) {
    // The buffer is full of pictures waiting for output: they must go out before this one can be decoded.
    for (Picture& q : pics_)
      if (q.in_use && q.needed_for_output) q.force_out = true;
    return H265_FALSE;
  }
  if (!alloc_picture(p)) return H265_FALSE;
  p->poc = poc_;
  p->pts = pts;
  p->in_use = true;
  p->is_ref = true;
  p->is_long = false;
  p->generated = false;
  p->force_out = false;
  p->latency_count = 0;
  p->needed_for_output = false;   // decided when the picture is complete
  p->order = decode_counter_++;
  p->full_range = false;
  for (uint64_t i = 0; i < p->col.size(); ++i) p->col[i].pred_flags = 0;
  cur_ = p;
  picture_open_ = true;
  first_picture_ = false;
  after_eos_ = false;
  first_header_ = sh;
  pending_hash_.present = false;
  offload_nals_.clear();
  offload_headers_.clear();
  offloaded_ = offload_ && offload_->accepts(sps, pps);
  return true;
}

void Core::build_tile_tables() {
  const Sps& s = *sp_;
  const Pps& p = *pp_;
  const int w = s.pic_w_ctb, h = s.pic_h_ctb;
  int col_w[24], row_h[24];
  if (p.uniform_spacing) {
    for (int i = 0; i < p.num_tile_cols; ++i) col_w[i] = ((i + 1) * w) / p.num_tile_cols - (i * w) / p.num_tile_cols;
    for (int i = 0; i < p.num_tile_rows; ++i) row_h[i] = ((i + 1) * h) / p.num_tile_rows - (i * h) / p.num_tile_rows;
  } else {
    int used = 0;
    for (int i = 0; i < p.num_tile_cols - 1; ++i) { col_w[i] = p.col_width_minus1[i] + 1; used += col_w[i]; }
    col_w[p.num_tile_cols - 1] = w - used;
    used = 0;
    for (int i = 0; i < p.num_tile_rows - 1; ++i) { row_h[i] = p.row_height_minus1[i] + 1; used += row_h[i]; }
    row_h[p.num_tile_rows - 1] = h - used;
  }
  col_bd_[0] = row_bd_[0] = 0;
  for (int i = 0; i < p.num_tile_cols; ++i) col_bd_[i + 1] = col_bd_[i] + col_w[i];
  for (int i = 0; i < p.num_tile_rows; ++i) row_bd_[i + 1] = row_bd_[i] + row_h[i];
  // (6-5) .. (6-7): raster scan to tile scan and back
  for (int rs = 0; rs < w * h; ++rs) {
    const int tbx = rs % w, tby = rs / w;
    int tile_x = 0, tile_y = 0;
    for (int i = 0; i < p.num_tile_cols; ++i)
      if (tbx >= col_bd_[i]) tile_x = i;
    for (int j = 0; j < p.num_tile_rows; ++j)
      if (tby >= row_bd_[j]) tile_y = j;
    int v = 0;
    for (int i = 0; i < tile_x; ++i) v += row_h[tile_y] * col_w[i];
    for (int j = 0; j < tile_y; ++j) v += w * row_h[j];
    v += (tby - row_bd_[tile_y]) * col_w[tile_x] + tbx - col_bd_[tile_x];
    rs_to_ts_[rs] = v;
    ts_to_rs_[v] = rs;
  }
  int tile = 0;
  for (int j = 0; j < p.num_tile_rows; ++j)
    for (int i = 0; i < p.num_tile_cols; ++i, ++tile)
      for (int y = row_bd_[j]; y < row_bd_[j + 1]; ++y)
        for (int x = col_bd_[i]; x < col_bd_[i + 1]; ++x) tile_id_ts_[rs_to_ts_[y * w + x]] = tile;
  // (6-10): z-scan order of 4x4 blocks
  const int shift = s.log2_ctb - 2;
  for (int y = 0; y < h4_; ++y)
    for (int x = 0; x < w4_; ++x) {
      const int rs = (y >> shift) * w + (x >> shift);
      uint32_t addr = static_cast<uint32_t>(rs_to_ts_[rs]) << (shift * 2);
      for (int i = 0; i < shift; ++i) {
        const uint32_t m = 1u << i;
        addr += ((m & static_cast<uint32_t>(x)) ? m * m : 0) + ((m & static_cast<uint32_t>(y)) ? 2 * m * m : 0);
      }
      zscan_[static_cast<uint64_t>(y) * w4_ + x] = addr;
    }
}

// Sets up the arrays that describe the picture being decoded.
bool Core::setup_picture_state() {
  const Sps& s = *sp_;
  const int w4 = s.pic_w_ctb << (s.log2_ctb - 2), h4 = s.pic_h_ctb << (s.log2_ctb - 2);   // whole CTBs, so no block is ever outside
  const uint64_t n4 = static_cast<uint64_t>(w4) * h4;
  const uint64_t nctb = static_cast<uint64_t>(s.pic_w_ctb) * s.pic_h_ctb;
  w4_ = w4;
  h4_ = h4;
  ctb_w_ = s.pic_w_ctb;
  ctb_h_ = s.pic_h_ctb;
  if (!pred_mode_.alloc(allocator_, n4) || !intra_mode_.alloc(allocator_, n4) || !qp_map_.alloc(allocator_, n4) || !ct_depth_.alloc(allocator_, n4) ||
      !mvf_.alloc(allocator_, n4) || !nz_.alloc(allocator_, n4) || !edge_.alloc(allocator_, n4) || !zscan_.alloc(allocator_, n4) || !rs_to_ts_.alloc(allocator_, nctb) ||
      !ts_to_rs_.alloc(allocator_, nctb) || !tile_id_ts_.alloc(allocator_, nctb) || !col_bd_.alloc(allocator_, 25) || !row_bd_.alloc(allocator_, 25) ||
      !ctb_slice_.alloc(allocator_, nctb) || !sao_.alloc(allocator_, nctb))
    return false;
  for (int c = 0; c < 3; ++c)
    if (!sao_plane_[c].alloc(allocator_, static_cast<uint64_t>(c == 0 ? s.width : s.width / 2) * (c == 0 ? s.height : s.height / 2))) return false;
  for (uint64_t i = 0; i < nctb; ++i) ctb_slice_[i] = -1;
  slices_.clear();
  slice_idx_ = -1;
  build_tile_tables();
  return true;
}

// Hands the picture to the offload: its parameter sets, the pictures kept as references (named by buffer and order count), and the slices as they came.
void Core::run_offload(Picture* p) {
  OffloadPicture o;
  o.sps = sp_;
  o.pps = pp_;
  o.first = &first_header_;
  o.poc = p->poc;
  o.slot = static_cast<int>(p - pics_);
  o.reference = !is_sub_layer_non_ref(first_header_.nal_type);
  int index_of[kMaxPics];
  for (int i = 0; i < kMaxPics; ++i) index_of[i] = -1;
  for (int i = 0; i < kMaxPics && o.ref_count < static_cast<uint32_t>(kMaxRefs); ++i) {
    const Picture& q = pics_[i];
    if (&q == p || !q.in_use || !q.is_ref) continue;
    index_of[i] = static_cast<int>(o.ref_count);
    o.refs[o.ref_count++] = {i, q.poc, q.is_long};
  }
  auto add = [&](Picture* const* set, int count, uint8_t* out, uint32_t* out_count) {
    for (int i = 0; i < count && *out_count < static_cast<uint32_t>(kMaxRefs); ++i) {
      if (!set[i]) continue;
      const int at = index_of[set[i] - pics_];
      if (at >= 0) out[(*out_count)++] = static_cast<uint8_t>(at);
    }
  };
  add(rps_st_curr_before_, num_st_before_, o.st_before, &o.st_before_count);
  add(rps_st_curr_after_, num_st_after_, o.st_after, &o.st_after_count);
  add(rps_lt_curr_, num_lt_curr_, o.lt_curr, &o.lt_count);
  for (auto& l : o.list)
    for (uint8_t& e : l) e = 0xFF;
  if (slices_.count() > 0) {
    const SliceInfo& first = slices_[0];
    for (int l = 0; l < 2; ++l)
      for (int i = 0; i < first_header_.num_ref_idx[l] && i < kMaxRefs - 1; ++i) {
        const Picture* r = first.ref_pic[l][i];
        if (r && index_of[r - pics_] >= 0) o.list[l][i] = static_cast<uint8_t>(index_of[r - pics_]);
      }
  }
  o.slices = offload_nals_.view();
  offload_info_.clear();
  for (uint64_t k = 0; k < offload_headers_.count() && k < slices_.count(); ++k) {
    OffloadSlice info;
    info.header = &offload_headers_[k];
    for (auto& l : info.list)
      for (uint8_t& e : l) e = 0xFF;
    for (int l = 0; l < 2; ++l)
      for (int i = 0; i < offload_headers_[k].num_ref_idx[l] && i < kMaxRefs - 1; ++i) {
        const Picture* r = slices_[k].ref_pic[l][i];
        if (r && index_of[r - pics_] >= 0) info.list[l][i] = static_cast<uint8_t>(index_of[r - pics_]);
      }
    if (!offload_info_.push_back(info)) break;
  }
  o.slice_info = offload_info_.view();
  for (int c = 0; c < 3; ++c) {
    o.plane[c] = ByteView(p->plane[c].data(), p->plane[c].size());
    o.stride[c] = p->stride[c];
  }
  if (!offload_->decode_picture(o))
    for (int c = 0; c < 3; ++c)
      for (uint64_t i = 0; i < p->plane[c].size(); ++i) p->plane[c][i] = 128;
}

// Called when the last slice of the picture has been decoded.
void Core::finish_picture() {
  picture_open_ = false;
  Picture* p = cur_;
  if (!p) return;
  if (offloaded_) {
    run_offload(p);
  } else {
    deblock_picture();
    sao_picture();
  }
  verify_picture_hash();
  // The motion the following pictures may use as collocated motion: one entry per 16x16 block (the engine keeps its own when it decodes).
  const Sps& s = *sp_;
  const int bw = (s.width + 15) / 16, bh = offloaded_ ? 0 : (s.height + 15) / 16;
  for (int by = 0; by < bh; ++by)
    for (int bx = 0; bx < bw; ++bx) {
      ColBlock& c = p->col[static_cast<uint64_t>(by) * bw + bx];
      const MvField& m = mvf_[static_cast<uint64_t>(by * 4) * w4_ + bx * 4];
      c.pred_flags = 0;
      c.long_term = 0;
      c.ref_poc[0] = c.ref_poc[1] = 0;
      c.mv[0][0] = c.mv[0][1] = c.mv[1][0] = c.mv[1][1] = 0;
      if (!(pred_mode_[static_cast<uint64_t>(by * 4) * w4_ + bx * 4] & 3) || (pred_mode_[static_cast<uint64_t>(by * 4) * w4_ + bx * 4] & 3) != 2) continue;
      const int ctb = ((by * 16) >> s.log2_ctb) * s.pic_w_ctb + ((bx * 16) >> s.log2_ctb);
      const int si = ctb_slice_[ctb];
      if (si < 0) continue;
      const SliceInfo& info = slices_[static_cast<uint64_t>(si)];
      for (int l = 0; l < 2; ++l) {
        if (!(m.pred_flags & (1 << l))) continue;
        c.pred_flags = static_cast<uint8_t>(c.pred_flags | (1 << l));
        c.mv[l][0] = m.mv[l][0];
        c.mv[l][1] = m.mv[l][1];
        c.ref_poc[l] = info.ref_poc[l][m.ref_idx[l]];
        if (info.ref_long[l][m.ref_idx[l]]) c.long_term = static_cast<uint8_t>(c.long_term | (1 << l));
      }
    }
  p->is_ref = true;
  p->is_long = false;
  p->needed_for_output = first_header_.pic_output_flag && !(is_rasl(first_header_.nal_type) && skip_rasl_);
  p->full_range = false;
  if (p->needed_for_output) {
    for (Picture& q : pics_)
      if (q.in_use && q.needed_for_output && &q != p) ++q.latency_count;
  }
  cur_ = nullptr;
}

// 7.4.3.2 / C.5.2: how many pictures may wait for output
bool Core::output_ready() const {
  int waiting = 0;
  bool forced = false, latency = false;
  const Sps* s = sp_;
  const int tid = s ? s->max_sub_layers - 1 : 0;
  for (const Picture& p : pics_) {
    if (!p.in_use || !p.needed_for_output) continue;
    ++waiting;
    if (p.force_out) forced = true;
    if (s && s->max_latency_increase_plus1[tid] != 0 && p.latency_count >= s->num_reorder[tid] + s->max_latency_increase_plus1[tid] - 1) latency = true;
  }
  if (forced || latency) return true;
  return s && waiting > s->num_reorder[tid];
}

void Core::bump_output(bool all) {
  if (!all) return;
  for (Picture& p : pics_)
    if (p.in_use && p.needed_for_output) p.force_out = true;
}

void Core::flush() {
  if (picture_open_) finish_picture();
  bump_output(true);
}

void Core::reset() {
  if (picture_open_) finish_picture();
  clear_dpb();
  first_picture_ = true;
  after_eos_ = true;
  no_rasl_output_ = true;
  skip_rasl_ = true;
  prev_tid0_poc_lsb_ = prev_tid0_poc_msb_ = 0;
}

bool Core::receive(VideoFrame* frame) {
  if (returned_) {
    // The picture handed out last time is no longer needed by the caller.
    if (!returned_->is_ref && !returned_->needed_for_output) returned_->in_use = false;
    returned_ = nullptr;
  }
  if (!output_ready()) return false;
  // The picture that goes out: among those of the oldest sequence, the one with the smallest order count.
  Picture* best = nullptr;
  bool any_forced = false;
  for (Picture& p : pics_)
    if (p.in_use && p.needed_for_output && p.force_out) any_forced = true;
  for (Picture& p : pics_) {
    if (!p.in_use || !p.needed_for_output) continue;
    if (any_forced && !p.force_out) continue;
    if (!best || p.poc < best->poc) best = &p;
  }
  if (!best) return false;
  best->needed_for_output = false;
  best->force_out = false;
  returned_ = best;
  // Cropped I420 in the output buffer.
  const int w = best->width - best->crop_l - best->crop_r, h = best->height - best->crop_t - best->crop_b;
  const int cw = w / 2, ch = h / 2;
  const uint64_t total = static_cast<uint64_t>(w) * h + 2 * static_cast<uint64_t>(cw) * ch;
  if (!out_frame_.alloc(allocator_, total)) return false;
  uint8_t* dst = out_frame_.data();
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) *dst++ = best->plane[0][static_cast<uint64_t>(y + best->crop_t) * best->stride[0] + x + best->crop_l];
  for (int c = 1; c < 3; ++c)
    for (int y = 0; y < ch; ++y)
      for (int x = 0; x < cw; ++x) *dst++ = best->plane[c][static_cast<uint64_t>(y + best->crop_t / 2) * best->stride[c] + x + best->crop_l / 2];
  out_format_ = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), PixelFormat::I420};
  VideoFrame f;
  f.format = out_format_;
  f.plane[0] = out_frame_.data();
  f.plane[1] = out_frame_.data() + static_cast<uint64_t>(w) * h;
  f.plane[2] = f.plane[1] + static_cast<uint64_t>(cw) * ch;
  f.stride[0] = static_cast<uint32_t>(w);
  f.stride[1] = f.stride[2] = static_cast<uint32_t>(cw);
  f.pts_us = best->pts;
  f.keyframe = false;
  f.limited_range = !best->full_range;
  *frame = f;
  return true;
}

VideoFormat Core::output_format() const {
  VideoFormat f;
  if (sp_) {
    f.width = static_cast<uint32_t>(sp_->width - sp_->crop_l - sp_->crop_r);
    f.height = static_cast<uint32_t>(sp_->height - sp_->crop_t - sp_->crop_b);
    f.pixel_format = PixelFormat::I420;
  }
  return f;
}

}  // namespace h265
}  // namespace hardware
