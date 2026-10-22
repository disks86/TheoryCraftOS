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

// VP9 decoding, part 1: the frame headers, probability contexts and their adaptation, the tile and partition syntax, block mode info
// (intra modes, reference frames, motion vectors) and the coefficient tokens. A port of the decoder side of libvpx (BSD-3-Clause,
// Copyright (c) The WebM Project authors) whose tables are generated into Vp9Tables.inc.
#include "Hardware/VideoCodec/Software/Vp9Core.hpp"
#include "Utility/Move.hpp"

namespace hardware {
namespace vp9 {

namespace {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int iabs(int v) { return v < 0 ? -v : v; }
inline int mi_cols_aligned_to_sb(int n) { return (n + 7) & ~7; }

inline int dc_quant(int qindex, int delta, int bit_depth) {
  const int i = clampi(qindex + delta, 0, MAXQ);
  return bit_depth == 8 ? dc_qlookup[i] : bit_depth == 10 ? dc_qlookup_10[i] : dc_qlookup_12[i];
}
inline int ac_quant(int qindex, int delta, int bit_depth) {
  const int i = clampi(qindex + delta, 0, MAXQ);
  return bit_depth == 8 ? ac_qlookup[i] : bit_depth == 10 ? ac_qlookup_10[i] : ac_qlookup_12[i];
}

// ---- probabilities ----------------------------------------------------------------------------------------------------------

inline uint8_t get_prob(unsigned num, unsigned den) {
  const int p = static_cast<int>((static_cast<uint64_t>(num) * 256 + (den >> 1)) / den);
  return static_cast<uint8_t>(p | ((255 - p) >> 23) | (p == 0));
}
inline uint8_t weighted_prob(int prob1, int prob2, int factor) { return static_cast<uint8_t>((prob1 * (256 - factor) + prob2 * factor + 128) >> 8); }
inline uint8_t get_binary_prob(unsigned n0, unsigned n1) {
  const unsigned den = n0 + n1;
  return den == 0 ? 128 : get_prob(n0, den);
}
inline uint8_t merge_probs(uint8_t pre_prob, const unsigned ct[2], unsigned count_sat, unsigned max_update_factor) {
  const uint8_t prob = get_binary_prob(ct[0], ct[1]);
  const unsigned count = imin(static_cast<int>(ct[0] + ct[1]), static_cast<int>(count_sat));
  const unsigned factor = max_update_factor * count / count_sat;
  return weighted_prob(pre_prob, prob, static_cast<int>(factor));
}
inline uint8_t mode_mv_merge_probs(uint8_t pre_prob, const unsigned ct[2]) {
  const unsigned den = ct[0] + ct[1];
  if (den == 0) return pre_prob;
  const unsigned count = den < static_cast<unsigned>(MODE_MV_COUNT_SAT) ? den : static_cast<unsigned>(MODE_MV_COUNT_SAT);
  const unsigned factor = count_to_update_factor[count];
  return weighted_prob(pre_prob, get_prob(ct[0], den), static_cast<int>(factor));
}
unsigned tree_merge_probs_impl(unsigned i, const int8_t* tree, const uint8_t* pre_probs, const unsigned* counts, uint8_t* probs) {
  const int l = tree[i];
  const unsigned left_count = (l <= 0) ? counts[-l] : tree_merge_probs_impl(static_cast<unsigned>(l), tree, pre_probs, counts, probs);
  const int r = tree[i + 1];
  const unsigned right_count = (r <= 0) ? counts[-r] : tree_merge_probs_impl(static_cast<unsigned>(r), tree, pre_probs, counts, probs);
  const unsigned ct[2] = {left_count, right_count};
  probs[i >> 1] = mode_mv_merge_probs(pre_probs[i >> 1], ct);
  return left_count + right_count;
}
inline void tree_merge_probs(const int8_t* tree, const uint8_t* pre_probs, const unsigned* counts, uint8_t* probs) {
  tree_merge_probs_impl(0, tree, pre_probs, counts, probs);
}

int inv_recenter_nonneg(int v, int m) {
  if (v > 2 * m) return v;
  return (v & 1) ? m - ((v + 1) >> 1) : m + (v >> 1);
}
int inv_remap_prob(int v, int m) {
  v = inv_map_table[v];
  m--;
  if ((m << 1) <= MAX_PROB) return 1 + inv_recenter_nonneg(v, m);
  return MAX_PROB - inv_recenter_nonneg(v, MAX_PROB - 1 - m);
}
int decode_uniform(BoolDecoder& r) {
  const int l = 8;
  const int m = (1 << l) - 191;
  const int v = r.literal(l - 1);
  return v < m ? v : (v << 1) - m + r.bit();
}
int decode_term_subexp(BoolDecoder& r) {
  if (!r.bit()) return r.literal(4);
  if (!r.bit()) return r.literal(4) + 16;
  if (!r.bit()) return r.literal(5) + 32;
  return decode_uniform(r) + 64;
}

void init_mode_probs(FrameContext* fc) {
  __builtin_memcpy(fc->uv_mode_prob, default_if_uv_probs, sizeof fc->uv_mode_prob);
  __builtin_memcpy(fc->y_mode_prob, default_if_y_probs, sizeof fc->y_mode_prob);
  __builtin_memcpy(fc->switchable_interp_prob, default_switchable_interp_prob, sizeof fc->switchable_interp_prob);
  __builtin_memcpy(fc->partition_prob, default_partition_probs, sizeof fc->partition_prob);
  __builtin_memcpy(fc->intra_inter_prob, default_intra_inter_p, sizeof fc->intra_inter_prob);
  __builtin_memcpy(fc->comp_inter_prob, default_comp_inter_p, sizeof fc->comp_inter_prob);
  __builtin_memcpy(fc->comp_ref_prob, default_comp_ref_p, sizeof fc->comp_ref_prob);
  __builtin_memcpy(fc->single_ref_prob, default_single_ref_p, sizeof fc->single_ref_prob);
  fc->tx_probs = default_tx_probs;
  __builtin_memcpy(fc->skip_probs, default_skip_probs, sizeof fc->skip_probs);
  __builtin_memcpy(fc->inter_mode_probs, default_inter_mode_probs, sizeof fc->inter_mode_probs);
}

void default_coef_probs(FrameContext* fc) {
  __builtin_memcpy(fc->coef_probs[TX_4X4], default_coef_probs_4x4, sizeof fc->coef_probs[0]);
  __builtin_memcpy(fc->coef_probs[TX_8X8], default_coef_probs_8x8, sizeof fc->coef_probs[0]);
  __builtin_memcpy(fc->coef_probs[TX_16X16], default_coef_probs_16x16, sizeof fc->coef_probs[0]);
  __builtin_memcpy(fc->coef_probs[TX_32X32], default_coef_probs_32x32, sizeof fc->coef_probs[0]);
}

inline int get_unsigned_bits(unsigned num_values) {
  int cat = 0;
  if (num_values <= 1) return 0;
  num_values--;
  while (num_values > 0) {
    cat++;
    num_values >>= 1;
  }
  return cat;
}

void get_scan(int tx_size, int tx_type, const int16_t** scan, const int16_t** neighbors) {
  switch (tx_size) {
    case TX_4X4:
      *scan = tx_type == ADST_DCT ? row_scan_4x4 : tx_type == DCT_ADST ? col_scan_4x4 : default_scan_4x4;
      *neighbors = tx_type == ADST_DCT ? row_scan_4x4_neighbors : tx_type == DCT_ADST ? col_scan_4x4_neighbors : default_scan_4x4_neighbors;
      break;
    case TX_8X8:
      *scan = tx_type == ADST_DCT ? row_scan_8x8 : tx_type == DCT_ADST ? col_scan_8x8 : default_scan_8x8;
      *neighbors = tx_type == ADST_DCT ? row_scan_8x8_neighbors : tx_type == DCT_ADST ? col_scan_8x8_neighbors : default_scan_8x8_neighbors;
      break;
    case TX_16X16:
      *scan = tx_type == ADST_DCT ? row_scan_16x16 : tx_type == DCT_ADST ? col_scan_16x16 : default_scan_16x16;
      *neighbors = tx_type == ADST_DCT ? row_scan_16x16_neighbors : tx_type == DCT_ADST ? col_scan_16x16_neighbors : default_scan_16x16_neighbors;
      break;
    default:
      *scan = default_scan_32x32;
      *neighbors = default_scan_32x32_neighbors;
      break;
  }
}

inline int band_coeff_contexts(int band) { return band == 0 ? 3 : 6; }

}  // namespace

Core::~Core() {
  for (Frame& f : frames_) release_frame(&f);
  release_view(allocator_, mi_);
  release_view(allocator_, grid_);
  release_view(allocator_, seg_map_[0]);
  release_view(allocator_, seg_map_[1]);
  for (auto& a : above_ctx_) release_view(allocator_, a);
  release_view(allocator_, above_part_ctx_);
}

void Core::reset() {
  for (int& i : ref_frame_map_) i = -1;
  for (Frame& f : frames_) f.ref_count = 0;
  need_resync_ = true;
  new_fb_idx_ = -1;
  prev_frame_idx_ = -1;
  last_width_ = last_height_ = 0;
  frame_type_ = 0;
}

void Core::release_frame(Frame* f) {
  release_view(allocator_, f->pixels);
  release_view(allocator_, f->mvs);
  for (auto& p : f->plane) p = ArrayView<uint16_t>();
  f->width = f->height = 0;
}

bool Core::allocate_frame(Frame* f, int width, int height) {
  const int aligned8_w = (width + 7) & ~7, aligned8_h = (height + 7) & ~7;
  const uint32_t stride_y = static_cast<uint32_t>((width + 63) & ~63), rows_y = static_cast<uint32_t>((height + 63) & ~63);
  const uint32_t stride_c = stride_y >> ss_x_, rows_c = rows_y >> ss_y_;
  const uint64_t total = uint64_t{stride_y} * rows_y + 2 * uint64_t{stride_c} * rows_c;
  const int mi_rows = (aligned8_h) >> 3, mi_cols = (aligned8_w) >> 3;
  if (f->pixels.size() != total) {
    release_view(allocator_, f->pixels);
    f->pixels = allocate_view<uint16_t>(allocator_, total, 16);
    if (f->pixels.empty()) return false;
  }
  if (f->mvs.size() != uint64_t{static_cast<uint32_t>(mi_rows)} * static_cast<uint32_t>(mi_cols)) {
    release_view(allocator_, f->mvs);
    f->mvs = allocate_view<MvRef>(allocator_, uint64_t{static_cast<uint32_t>(mi_rows)} * static_cast<uint32_t>(mi_cols));
    if (f->mvs.empty()) return false;
    for (MvRef& m : f->mvs) m = MvRef();
  }
  f->plane[0] = f->pixels.subview(0, uint64_t{stride_y} * rows_y);
  f->plane[1] = f->pixels.subview(uint64_t{stride_y} * rows_y, uint64_t{stride_c} * rows_c);
  f->plane[2] = f->pixels.subview(uint64_t{stride_y} * rows_y + uint64_t{stride_c} * rows_c, uint64_t{stride_c} * rows_c);
  f->stride[0] = stride_y;
  f->stride[1] = f->stride[2] = stride_c;
  f->plane_rows[0] = rows_y;
  f->plane_rows[1] = f->plane_rows[2] = rows_c;
  f->width = width;
  f->height = height;
  f->crop_w[0] = width;
  f->crop_h[0] = height;
  f->crop_w[1] = f->crop_w[2] = (width + ss_x_) >> ss_x_;
  f->crop_h[1] = f->crop_h[2] = (height + ss_y_) >> ss_y_;
  f->aligned_w[0] = aligned8_w;
  f->aligned_h[0] = aligned8_h;
  f->aligned_w[1] = f->aligned_w[2] = aligned8_w >> ss_x_;
  f->aligned_h[1] = f->aligned_h[2] = aligned8_h >> ss_y_;
  f->ss_x = ss_x_;
  f->ss_y = ss_y_;
  f->bit_depth = bit_depth_;
  f->color_space = color_space_;
  f->full_range = full_range_;
  f->mi_rows = mi_rows;
  f->mi_cols = mi_cols;
  f->corrupted = false;
  return true;
}

// ---- the uncompressed header --------------------------------------------------------------------------------------------------

bool Core::read_color_config(BitReader& rb) {
  if (profile_ >= 2) bit_depth_ = rb.bit() ? 12 : 10;
  else bit_depth_ = 8;
  color_space_ = rb.literal(3);
  if (color_space_ != 7) {  // not sRGB
    full_range_ = rb.bit() != 0;
    if (profile_ == 1 || profile_ == 3) {
      ss_x_ = rb.bit();
      ss_y_ = rb.bit();
      if (ss_x_ == 1 && ss_y_ == 1) return false;
      if (rb.bit()) return false;  // reserved bit
    } else {
      ss_x_ = ss_y_ = 1;
    }
  } else {
    full_range_ = true;
    if (profile_ == 1 || profile_ == 3) {
      ss_x_ = ss_y_ = 0;
      if (rb.bit()) return false;
    } else {
      return false;  // 4:4:4 needs profile 1 or 3
    }
  }
  return !rb.error();
}

bool Core::resize_context(int width, int height) {
  const int new_mi_rows = (height + 7) >> 3, new_mi_cols = (width + 7) >> 3;
  if (width_ != width || height_ != height || mi_.empty()) {
    const uint64_t cells = uint64_t{static_cast<uint32_t>(new_mi_rows)} * static_cast<uint32_t>(new_mi_cols);
    if (mi_.size() != cells) {
      release_view(allocator_, mi_);
      release_view(allocator_, grid_);
      release_view(allocator_, seg_map_[0]);
      release_view(allocator_, seg_map_[1]);
      mi_ = allocate_view<ModeInfo>(allocator_, cells);
      grid_ = allocate_view<int32_t>(allocator_, cells);
      seg_map_[0] = allocate_view<uint8_t>(allocator_, cells);
      seg_map_[1] = allocate_view<uint8_t>(allocator_, cells);
      if (mi_.empty() || grid_.empty() || seg_map_[0].empty() || seg_map_[1].empty()) return false;
    }
    const uint64_t ctx = uint64_t{static_cast<uint32_t>(mi_cols_aligned_to_sb(new_mi_cols))} * 2 + 32;
    if (above_part_ctx_.size() != uint64_t{static_cast<uint32_t>(mi_cols_aligned_to_sb(new_mi_cols))} + 8) {
      for (auto& a : above_ctx_) release_view(allocator_, a);
      release_view(allocator_, above_part_ctx_);
      for (auto& a : above_ctx_) {
        a = allocate_view<uint8_t>(allocator_, ctx);
        if (a.empty()) return false;
      }
      above_part_ctx_ = allocate_view<int8_t>(allocator_, uint64_t{static_cast<uint32_t>(mi_cols_aligned_to_sb(new_mi_cols))} + 8);
      if (above_part_ctx_.empty()) return false;
    }
    mi_rows_ = new_mi_rows;
    mi_cols_ = new_mi_cols;
    sb_cols_aligned_ = mi_cols_aligned_to_sb(new_mi_cols);
    for (uint8_t& v : seg_map_[0]) v = 0;
    for (uint8_t& v : seg_map_[1]) v = 0;
    width_ = width;
    height_ = height;
  }
  return true;
}

bool Core::setup_frame_size(BitReader& rb) {
  const int width = rb.literal(16) + 1, height = rb.literal(16) + 1;
  if (rb.error() || width > 16384 || height > 16384) return false;
  if (!resize_context(width, height)) return false;
  if (rb.bit()) {  // render size: not needed
    rb.literal(16);
    rb.literal(16);
  }
  return allocate_frame(&frames_[new_fb_idx_], width_, height_);
}

bool Core::setup_frame_size_with_refs(BitReader& rb) {
  int width = 0, height = 0;
  bool found = false;
  for (int i = 0; i < REFS_PER_FRAME; ++i) {
    if (rb.bit()) {
      const Frame& f = frames_[frame_refs_[i].idx];
      width = f.width;
      height = f.height;
      found = true;
      break;
    }
  }
  if (!found) {
    width = rb.literal(16) + 1;
    height = rb.literal(16) + 1;
  }
  if (rb.error() || width <= 0 || height <= 0 || width > 16384 || height > 16384) return false;
  bool has_valid_ref = false;
  for (int i = 0; i < REFS_PER_FRAME; ++i) {
    const Frame& f = frames_[frame_refs_[i].idx];
    has_valid_ref = has_valid_ref || (2 * width >= f.width && 2 * height >= f.height && width <= 16 * f.width && height <= 16 * f.height);
  }
  if (!has_valid_ref) return false;
  for (int i = 0; i < REFS_PER_FRAME; ++i) {
    const Frame& f = frames_[frame_refs_[i].idx];
    if (f.bit_depth != bit_depth_ || f.ss_x != ss_x_ || f.ss_y != ss_y_) return false;
  }
  if (!resize_context(width, height)) return false;
  if (rb.bit()) {
    rb.literal(16);
    rb.literal(16);
  }
  return allocate_frame(&frames_[new_fb_idx_], width_, height_);
}

void Core::setup_loopfilter(BitReader& rb) {
  lf_.filter_level = rb.literal(6);
  lf_.sharpness_level = rb.literal(3);
  lf_.mode_ref_delta_update = false;
  lf_.mode_ref_delta_enabled = rb.bit() != 0;
  if (lf_.mode_ref_delta_enabled) {
    lf_.mode_ref_delta_update = rb.bit() != 0;
    if (lf_.mode_ref_delta_update) {
      for (int i = 0; i < MAX_REF_FRAMES; i++)
        if (rb.bit()) lf_.ref_deltas[i] = static_cast<int8_t>(rb.signed_literal(6));
      for (int i = 0; i < 2; i++)
        if (rb.bit()) lf_.mode_deltas[i] = static_cast<int8_t>(rb.signed_literal(6));
    }
  }
}

void Core::setup_quantization(BitReader& rb) {
  base_qindex_ = rb.literal(8);
  auto read_delta_q = [&]() { return rb.bit() ? rb.signed_literal(4) : 0; };
  y_dc_delta_q_ = read_delta_q();
  uv_dc_delta_q_ = read_delta_q();
  uv_ac_delta_q_ = read_delta_q();
  lossless_ = base_qindex_ == 0 && y_dc_delta_q_ == 0 && uv_dc_delta_q_ == 0 && uv_ac_delta_q_ == 0;
}

void Core::setup_segmentation(BitReader& rb) {
  seg_.update_map = false;
  seg_.update_data = false;
  seg_.enabled = rb.bit() != 0;
  if (!seg_.enabled) return;
  seg_.update_map = rb.bit() != 0;
  if (seg_.update_map) {
    for (int i = 0; i < SEG_TREE_PROBS; i++) seg_.tree_probs[i] = rb.bit() ? static_cast<uint8_t>(rb.literal(8)) : MAX_PROB;
    seg_.temporal_update = rb.bit() != 0;
    if (seg_.temporal_update) {
      for (int i = 0; i < PREDICTION_PROBS; i++) seg_.pred_probs[i] = rb.bit() ? static_cast<uint8_t>(rb.literal(8)) : MAX_PROB;
    } else {
      for (int i = 0; i < PREDICTION_PROBS; i++) seg_.pred_probs[i] = MAX_PROB;
    }
  }
  seg_.update_data = rb.bit() != 0;
  if (seg_.update_data) {
    seg_.abs_delta = rb.bit() != 0;
    for (int i = 0; i < MAX_SEGMENTS; i++) {
      seg_.feature_mask[i] = 0;
      for (int j = 0; j < SEG_LVL_MAX; j++) {
        int data = 0;
        if (rb.bit()) {
          seg_.feature_mask[i] |= 1u << j;
          const int max = seg_feature_data_max[j];
          data = rb.literal(get_unsigned_bits(static_cast<unsigned>(max)));
          data = data > max ? max : data;
          if (j == SEG_LVL_ALT_Q || j == SEG_LVL_ALT_LF) data = rb.bit() ? -data : data;
        }
        seg_.feature_data[i][j] = static_cast<int16_t>(data);
      }
    }
  }
}

void Core::setup_segmentation_dequant() {
  auto qindex_of = [&](int segment) {
    if (seg_.active(segment, SEG_LVL_ALT_Q)) {
      const int data = seg_.feature_data[segment][SEG_LVL_ALT_Q];
      return clampi(seg_.abs_delta ? data : base_qindex_ + data, 0, MAXQ);
    }
    return base_qindex_;
  };
  const int segments = seg_.enabled ? MAX_SEGMENTS : 1;
  for (int i = 0; i < segments; ++i) {
    const int qindex = seg_.enabled ? qindex_of(i) : base_qindex_;
    y_dequant_[i][0] = static_cast<int16_t>(dc_quant(qindex, y_dc_delta_q_, bit_depth_));
    y_dequant_[i][1] = static_cast<int16_t>(ac_quant(qindex, 0, bit_depth_));
    uv_dequant_[i][0] = static_cast<int16_t>(dc_quant(qindex, uv_dc_delta_q_, bit_depth_));
    uv_dequant_[i][1] = static_cast<int16_t>(ac_quant(qindex, uv_ac_delta_q_, bit_depth_));
  }
}

void Core::setup_tile_info(BitReader& rb) {
  const int sb64_cols = mi_cols_aligned_to_sb(mi_cols_) >> 3;
  int min_log2 = 0;
  while ((64 << min_log2) < sb64_cols) ++min_log2;
  int max_log2 = 1;
  while ((sb64_cols >> max_log2) >= 4) ++max_log2;
  max_log2--;
  int max_ones = max_log2 - min_log2;
  log2_tile_cols_ = min_log2;
  while (max_ones-- > 0 && rb.bit()) log2_tile_cols_++;
  log2_tile_rows_ = rb.bit();
  if (log2_tile_rows_) log2_tile_rows_ += rb.bit();
}

void Core::setup_past_independence() {
  for (int i = 0; i < MAX_SEGMENTS; ++i) {
    seg_.feature_mask[i] = 0;
    for (int j = 0; j < SEG_LVL_MAX; ++j) seg_.feature_data[i][j] = 0;
  }
  seg_.abs_delta = false;
  for (uint8_t& v : seg_map_[0]) v = 0;
  for (uint8_t& v : seg_map_[1]) v = 0;
  lf_.ref_deltas[INTRA_FRAME] = 1;
  lf_.ref_deltas[LAST_FRAME] = 0;
  lf_.ref_deltas[GOLDEN_FRAME] = -1;
  lf_.ref_deltas[ALTREF_FRAME] = -1;
  lf_.mode_deltas[0] = lf_.mode_deltas[1] = 0;
  lf_.mode_ref_delta_enabled = true;
  lf_.mode_ref_delta_update = true;
  lf_.last_sharpness_level = -1;
  default_coef_probs(&fc_);
  init_mode_probs(&fc_);
  fc_.nmvc = default_nmv_context;
  fc_.initialized = true;
  if (frame_type_ == 0 || error_resilient_ || reset_frame_context_ == 3) {
    for (int i = 0; i < FRAME_CONTEXTS; ++i) frame_contexts_[i] = fc_;
  } else if (reset_frame_context_ == 2) {
    frame_contexts_[frame_context_idx_] = fc_;
  }
  for (int& v : ref_frame_sign_bias_) v = 0;
  frame_context_idx_ = 0;
}

void Core::setup_scale_factors(ScaleFactors* sf, int other_w, int other_h, int this_w, int this_h) {
  if (!(2 * this_w >= other_w && 2 * this_h >= other_h && this_w <= 16 * other_w && this_h <= 16 * other_h)) {
    sf->x_scale_fp = sf->y_scale_fp = -1;
    return;
  }
  sf->x_scale_fp = (other_w << 14) / this_w;
  sf->y_scale_fp = (other_h << 14) / this_h;
  sf->x_step_q4 = static_cast<int>((static_cast<int64_t>(16) * sf->x_scale_fp) >> 14);
  sf->y_step_q4 = static_cast<int>((static_cast<int64_t>(16) * sf->y_scale_fp) >> 14);
}

bool Core::read_uncompressed_header(BitReader& rb, uint32_t* header_size, int* shown, FrameHeaderInfo* info) {
  last_frame_type_ = frame_type_;
  last_intra_only_ = intra_only_;
  if (rb.literal(2) != 2) return false;  // frame marker
  profile_ = rb.bit();
  profile_ |= rb.bit() << 1;
  if (profile_ > 2) profile_ += rb.bit();
  if (profile_ >= 4) return false;
  info->profile = profile_;
  info->show_existing_frame = rb.bit() != 0;
  if (info->show_existing_frame) {
    const int slot = rb.literal(3);
    const int idx = ref_frame_map_[slot];
    if (idx < 0 || frames_[idx].ref_count < 1 || rb.error()) return false;
    refresh_frame_flags_ = 0;
    lf_.filter_level = 0;
    show_frame_ = true;
    *shown = idx;
    *header_size = 0;
    info->show_frame = true;
    info->width = frames_[idx].width;
    info->height = frames_[idx].height;
    info->bit_depth = frames_[idx].bit_depth;
    return true;
  }
  frame_type_ = rb.bit();
  show_frame_ = rb.bit() != 0;
  error_resilient_ = rb.bit() != 0;
  info->key_frame = frame_type_ == 0;
  info->show_frame = show_frame_;
  // The frame buffer for this picture: any that nothing refers to.
  new_fb_idx_ = -1;
  for (int i = 0; i < kPoolSize; ++i)
    if (frames_[i].ref_count == 0) {
      new_fb_idx_ = i;
      break;
    }
  if (new_fb_idx_ < 0) return false;
  frames_[new_fb_idx_].ref_count = 1;  // held by the decoding itself until the references are updated
  if (frame_type_ == 0) {
    if (rb.literal(8) != 0x49 || rb.literal(8) != 0x83 || rb.literal(8) != 0x42) return false;
    if (!read_color_config(rb)) return false;
    refresh_frame_flags_ = (1 << REF_FRAMES) - 1;
    for (RefBuffer& r : frame_refs_) r.idx = -1;
    if (!setup_frame_size(rb)) return false;
    if (need_resync_) {
      for (int& i : ref_frame_map_) i = -1;
      for (int i = 0; i < kPoolSize; ++i)
        if (i != new_fb_idx_) frames_[i].ref_count = 0;
      need_resync_ = false;
    }
  } else {
    intra_only_ = show_frame_ ? false : rb.bit() != 0;
    reset_frame_context_ = error_resilient_ ? 0 : rb.literal(2);
    if (intra_only_) {
      if (rb.literal(8) != 0x49 || rb.literal(8) != 0x83 || rb.literal(8) != 0x42) return false;
      if (profile_ > 0) {
        if (!read_color_config(rb)) return false;
      } else {
        color_space_ = 2;  // BT.601
        full_range_ = false;
        ss_x_ = ss_y_ = 1;
        bit_depth_ = 8;
      }
      refresh_frame_flags_ = rb.literal(REF_FRAMES);
      if (!setup_frame_size(rb)) return false;
      if (need_resync_) {
        for (int& i : ref_frame_map_) i = -1;
        need_resync_ = false;
      }
    } else if (!need_resync_) {
      refresh_frame_flags_ = rb.literal(REF_FRAMES);
      for (int i = 0; i < REFS_PER_FRAME; ++i) {
        const int ref = rb.literal(3);
        const int idx = ref_frame_map_[ref];
        if (idx < 0) return false;
        frame_refs_[i].idx = idx;
        ref_frame_sign_bias_[LAST_FRAME + i] = rb.bit();
      }
      if (!setup_frame_size_with_refs(rb)) return false;
      allow_high_precision_mv_ = rb.bit() != 0;
      static const int kLiteralToFilter[4] = {EIGHTTAP_SMOOTH, EIGHTTAP, EIGHTTAP_SHARP, BILINEAR};
      interp_filter_ = rb.bit() ? SWITCHABLE : kLiteralToFilter[rb.literal(2)];
      for (int i = 0; i < REFS_PER_FRAME; ++i) {
        const Frame& rf = frames_[frame_refs_[i].idx];
        setup_scale_factors(&frame_refs_[i].sf, rf.width, rf.height, width_, height_);
      }
    }
  }
  if (need_resync_) return false;  // a key frame or an intra-only frame must come first
  frames_[new_fb_idx_].color_space = color_space_;
  info->intra_only = intra_only_;
  info->width = width_;
  info->height = height_;
  info->bit_depth = bit_depth_;
  if (!error_resilient_) {
    refresh_frame_context_ = rb.bit() != 0;
    frame_parallel_decoding_mode_ = rb.bit() != 0;
  } else {
    refresh_frame_context_ = false;
    frame_parallel_decoding_mode_ = true;
  }
  frame_context_idx_ = rb.literal(2);
  const bool intra = frame_type_ == 0 || intra_only_;
  if (intra || error_resilient_) setup_past_independence();
  setup_loopfilter(rb);
  setup_quantization(rb);
  setup_segmentation(rb);
  setup_segmentation_dequant();
  setup_tile_info(rb);
  *header_size = static_cast<uint32_t>(rb.literal(16));
  *shown = show_frame_ ? new_fb_idx_ : -1;
  return !rb.error();
}

// ---- the compressed header ---------------------------------------------------------------------------------------------------

void Core::diff_update_prob(BoolDecoder& r, uint8_t* p) {
  if (r.read(252)) {  // DIFF_UPDATE_PROB
    const int delp = decode_term_subexp(r);
    if (delp < 254) *p = static_cast<uint8_t>(inv_remap_prob(delp, *p));
    else corrupted_ = true;
  }
}

void Core::read_coef_probs(BoolDecoder& r) {
  const int max_tx_size = tx_mode_to_biggest_tx_size[tx_mode_];
  for (int tx_size = TX_4X4; tx_size <= max_tx_size; ++tx_size) {
    if (r.bit()) {
      for (int i = 0; i < PLANE_TYPES; ++i)
        for (int j = 0; j < REF_TYPES; ++j)
          for (int k = 0; k < COEF_BANDS; ++k)
            for (int l = 0; l < band_coeff_contexts(k); ++l)
              for (int m = 0; m < UNCONSTRAINED_NODES; ++m) diff_update_prob(r, &fc_.coef_probs[tx_size][i][j][k][l][m]);
    }
  }
}

bool Core::read_compressed_header(ConstByteView data) {
  BoolDecoder r;
  if (!r.init(data)) return false;
  corrupted_ = false;
  if (lossless_) {
    tx_mode_ = ONLY_4X4;
  } else {
    tx_mode_ = r.literal(2);
    if (tx_mode_ == ALLOW_32X32) tx_mode_ += r.bit();
  }
  if (tx_mode_ == TX_MODE_SELECT) {
    for (int i = 0; i < TX_SIZE_CONTEXTS; ++i)
      for (int j = 0; j < TX_SIZES - 3; ++j) diff_update_prob(r, &fc_.tx_probs.p8x8[i][j]);
    for (int i = 0; i < TX_SIZE_CONTEXTS; ++i)
      for (int j = 0; j < TX_SIZES - 2; ++j) diff_update_prob(r, &fc_.tx_probs.p16x16[i][j]);
    for (int i = 0; i < TX_SIZE_CONTEXTS; ++i)
      for (int j = 0; j < TX_SIZES - 1; ++j) diff_update_prob(r, &fc_.tx_probs.p32x32[i][j]);
  }
  read_coef_probs(r);
  for (int k = 0; k < SKIP_CONTEXTS; ++k) diff_update_prob(r, &fc_.skip_probs[k]);
  if (!(frame_type_ == 0 || intra_only_)) {
    for (int i = 0; i < INTER_MODE_CONTEXTS; ++i)
      for (int j = 0; j < INTER_MODES - 1; ++j) diff_update_prob(r, &fc_.inter_mode_probs[i][j]);
    if (interp_filter_ == SWITCHABLE)
      for (int j = 0; j < SWITCHABLE_FILTER_CONTEXTS; ++j)
        for (int i = 0; i < SWITCHABLE_FILTERS - 1; ++i) diff_update_prob(r, &fc_.switchable_interp_prob[j][i]);
    for (int i = 0; i < INTRA_INTER_CONTEXTS; i++) diff_update_prob(r, &fc_.intra_inter_prob[i]);
    // reference mode
    bool compound_allowed = false;
    for (int i = 1; i < REFS_PER_FRAME; ++i)
      if (ref_frame_sign_bias_[i + 1] != ref_frame_sign_bias_[1]) compound_allowed = true;
    reference_mode_ = 0;
    if (compound_allowed) reference_mode_ = r.bit() ? (r.bit() ? 2 : 1) : 0;
    if (reference_mode_ != 0) {
      if (ref_frame_sign_bias_[LAST_FRAME] == ref_frame_sign_bias_[GOLDEN_FRAME]) {
        comp_fixed_ref_ = ALTREF_FRAME;
        comp_var_ref_[0] = LAST_FRAME;
        comp_var_ref_[1] = GOLDEN_FRAME;
      } else if (ref_frame_sign_bias_[LAST_FRAME] == ref_frame_sign_bias_[ALTREF_FRAME]) {
        comp_fixed_ref_ = GOLDEN_FRAME;
        comp_var_ref_[0] = LAST_FRAME;
        comp_var_ref_[1] = ALTREF_FRAME;
      } else {
        comp_fixed_ref_ = LAST_FRAME;
        comp_var_ref_[0] = GOLDEN_FRAME;
        comp_var_ref_[1] = ALTREF_FRAME;
      }
    }
    if (reference_mode_ == 2)
      for (int i = 0; i < COMP_INTER_CONTEXTS; ++i) diff_update_prob(r, &fc_.comp_inter_prob[i]);
    if (reference_mode_ != 1)
      for (int i = 0; i < REF_CONTEXTS; ++i) {
        diff_update_prob(r, &fc_.single_ref_prob[i][0]);
        diff_update_prob(r, &fc_.single_ref_prob[i][1]);
      }
    if (reference_mode_ != 0)
      for (int i = 0; i < REF_CONTEXTS; ++i) diff_update_prob(r, &fc_.comp_ref_prob[i]);
    for (int j = 0; j < BLOCK_SIZE_GROUPS; j++)
      for (int i = 0; i < INTRA_MODES - 1; ++i) diff_update_prob(r, &fc_.y_mode_prob[j][i]);
    for (int j = 0; j < PARTITION_CONTEXTS; ++j)
      for (int i = 0; i < PARTITION_TYPES - 1; ++i) diff_update_prob(r, &fc_.partition_prob[j][i]);
    // motion vector probabilities
    auto update_mv_probs = [&](uint8_t* p, int n) {
      for (int i = 0; i < n; ++i)
        if (r.read(252)) p[i] = static_cast<uint8_t>((r.literal(7) << 1) | 1);
    };
    NmvContext& nmvc = fc_.nmvc;
    update_mv_probs(nmvc.joints, MV_JOINTS - 1);
    for (int i = 0; i < 2; ++i) {
      NmvComponent& comp = nmvc.comps[i];
      update_mv_probs(&comp.sign, 1);
      update_mv_probs(comp.classes, MV_CLASSES - 1);
      update_mv_probs(comp.class0, CLASS0_SIZE - 1);
      update_mv_probs(comp.bits, MV_OFFSET_BITS);
    }
    for (int i = 0; i < 2; ++i) {
      NmvComponent& comp = nmvc.comps[i];
      for (int j = 0; j < CLASS0_SIZE; ++j) update_mv_probs(comp.class0_fp[j], MV_FP_SIZE - 1);
      update_mv_probs(comp.fp, 3);
    }
    if (allow_high_precision_mv_) {
      for (int i = 0; i < 2; ++i) {
        update_mv_probs(&nmvc.comps[i].class0_hp, 1);
        update_mv_probs(&nmvc.comps[i].hp, 1);
      }
    }
  }
  return !r.error() && !corrupted_;
}

// ---- adaptation after a frame -------------------------------------------------------------------------------------------------

void Core::adapt_coef_probs() {
  unsigned count_sat, update_factor;
  if (frame_type_ == 0 || intra_only_) {
    update_factor = 112;
    count_sat = 24;
  } else if (last_frame_type_ == 0) {
    update_factor = 128;
    count_sat = 24;
  } else {
    update_factor = 112;
    count_sat = 24;
  }
  const FrameContext& pre_fc = frame_contexts_[frame_context_idx_];
  for (int t = TX_4X4; t <= TX_32X32; ++t) {
    for (int i = 0; i < PLANE_TYPES; ++i)
      for (int j = 0; j < REF_TYPES; ++j)
        for (int k = 0; k < COEF_BANDS; ++k)
          for (int l = 0; l < band_coeff_contexts(k); ++l) {
            const unsigned* c = counts_.coef[t][i][j][k][l];
            const unsigned n0 = c[0], n1 = c[1], n2 = c[2], neob = c[3];
            const unsigned eob_branch = counts_.eob_branch[t][i][j][k][l];
            const unsigned branch_ct[UNCONSTRAINED_NODES][2] = {{neob, eob_branch - neob}, {n0, n1 + n2}, {n1, n2}};
            for (int m = 0; m < UNCONSTRAINED_NODES; ++m)
              fc_.coef_probs[t][i][j][k][l][m] = merge_probs(pre_fc.coef_probs[t][i][j][k][l][m], branch_ct[m], count_sat, update_factor);
          }
  }
}

void Core::adapt_mode_probs() {
  const FrameContext& pre_fc = frame_contexts_[frame_context_idx_];
  const FrameCounts& counts = counts_;
  for (int i = 0; i < INTRA_INTER_CONTEXTS; i++) fc_.intra_inter_prob[i] = mode_mv_merge_probs(pre_fc.intra_inter_prob[i], counts.intra_inter[i]);
  for (int i = 0; i < COMP_INTER_CONTEXTS; i++) fc_.comp_inter_prob[i] = mode_mv_merge_probs(pre_fc.comp_inter_prob[i], counts.comp_inter[i]);
  for (int i = 0; i < REF_CONTEXTS; i++) fc_.comp_ref_prob[i] = mode_mv_merge_probs(pre_fc.comp_ref_prob[i], counts.comp_ref[i]);
  for (int i = 0; i < REF_CONTEXTS; i++)
    for (int j = 0; j < 2; j++) fc_.single_ref_prob[i][j] = mode_mv_merge_probs(pre_fc.single_ref_prob[i][j], counts.single_ref[i][j]);
  for (int i = 0; i < INTER_MODE_CONTEXTS; i++)
    tree_merge_probs(vp9_inter_mode_tree, pre_fc.inter_mode_probs[i], counts.inter_mode[i], fc_.inter_mode_probs[i]);
  for (int i = 0; i < BLOCK_SIZE_GROUPS; i++) tree_merge_probs(vp9_intra_mode_tree, pre_fc.y_mode_prob[i], counts.y_mode[i], fc_.y_mode_prob[i]);
  for (int i = 0; i < INTRA_MODES; ++i) tree_merge_probs(vp9_intra_mode_tree, pre_fc.uv_mode_prob[i], counts.uv_mode[i], fc_.uv_mode_prob[i]);
  for (int i = 0; i < PARTITION_CONTEXTS; i++) tree_merge_probs(vp9_partition_tree, pre_fc.partition_prob[i], counts.partition[i], fc_.partition_prob[i]);
  if (interp_filter_ == SWITCHABLE)
    for (int i = 0; i < SWITCHABLE_FILTER_CONTEXTS; i++)
      tree_merge_probs(vp9_switchable_interp_tree, pre_fc.switchable_interp_prob[i], counts.switchable_interp[i], fc_.switchable_interp_prob[i]);
  if (tx_mode_ == TX_MODE_SELECT) {
    for (int i = 0; i < TX_SIZE_CONTEXTS; ++i) {
      unsigned ct8[TX_SIZES - 3][2], ct16[TX_SIZES - 2][2], ct32[TX_SIZES - 1][2];
      const unsigned* c8 = counts.tx.p8x8[i];
      ct8[0][0] = c8[TX_4X4];
      ct8[0][1] = c8[TX_8X8];
      for (int j = 0; j < TX_SIZES - 3; ++j) fc_.tx_probs.p8x8[i][j] = mode_mv_merge_probs(pre_fc.tx_probs.p8x8[i][j], ct8[j]);
      const unsigned* c16 = counts.tx.p16x16[i];
      ct16[0][0] = c16[TX_4X4];
      ct16[0][1] = c16[TX_8X8] + c16[TX_16X16];
      ct16[1][0] = c16[TX_8X8];
      ct16[1][1] = c16[TX_16X16];
      for (int j = 0; j < TX_SIZES - 2; ++j) fc_.tx_probs.p16x16[i][j] = mode_mv_merge_probs(pre_fc.tx_probs.p16x16[i][j], ct16[j]);
      const unsigned* c32 = counts.tx.p32x32[i];
      ct32[0][0] = c32[TX_4X4];
      ct32[0][1] = c32[TX_8X8] + c32[TX_16X16] + c32[TX_32X32];
      ct32[1][0] = c32[TX_8X8];
      ct32[1][1] = c32[TX_16X16] + c32[TX_32X32];
      ct32[2][0] = c32[TX_16X16];
      ct32[2][1] = c32[TX_32X32];
      for (int j = 0; j < TX_SIZES - 1; ++j) fc_.tx_probs.p32x32[i][j] = mode_mv_merge_probs(pre_fc.tx_probs.p32x32[i][j], ct32[j]);
    }
  }
  for (int i = 0; i < SKIP_CONTEXTS; ++i) fc_.skip_probs[i] = mode_mv_merge_probs(pre_fc.skip_probs[i], counts.skip[i]);
}

void Core::adapt_mv_probs() {
  NmvContext& fc = fc_.nmvc;
  const NmvContext& pre_fc = frame_contexts_[frame_context_idx_].nmvc;
  tree_merge_probs(vp9_mv_joint_tree, pre_fc.joints, counts_.mv_joints, fc.joints);
  for (int i = 0; i < 2; ++i) {
    NmvComponent& comp = fc.comps[i];
    const NmvComponent& pre_comp = pre_fc.comps[i];
    const NmvComponentCounts& c = counts_.mv_comps[i];
    comp.sign = mode_mv_merge_probs(pre_comp.sign, c.sign);
    tree_merge_probs(vp9_mv_class_tree, pre_comp.classes, c.classes, comp.classes);
    tree_merge_probs(vp9_mv_class0_tree, pre_comp.class0, c.class0, comp.class0);
    for (int j = 0; j < MV_OFFSET_BITS; ++j) comp.bits[j] = mode_mv_merge_probs(pre_comp.bits[j], c.bits[j]);
    for (int j = 0; j < CLASS0_SIZE; ++j) tree_merge_probs(vp9_mv_fp_tree, pre_comp.class0_fp[j], c.class0_fp[j], comp.class0_fp[j]);
    tree_merge_probs(vp9_mv_fp_tree, pre_comp.fp, c.fp, comp.fp);
    if (allow_high_precision_mv_) {
      comp.class0_hp = mode_mv_merge_probs(pre_comp.class0_hp, c.class0_hp);
      comp.hp = mode_mv_merge_probs(pre_comp.hp, c.hp);
    }
  }
}

// ---- mode info -----------------------------------------------------------------------------------------------------------------

namespace {

constexpr int kMvBorder = 16 << 3;
constexpr int kMvLow = -(1 << 14), kMvUpp = 1 << 14;

inline bool use_mv_hp(const Mv& mv) { return (iabs(mv.row) >> 3) < 8 && (iabs(mv.col) >> 3) < 8; }

inline void lower_mv_precision(Mv* mv, bool allow_hp) {
  const bool use_hp = allow_hp && use_mv_hp(*mv);
  if (!use_hp) {
    if (mv->row & 1) mv->row = static_cast<int16_t>(mv->row + (mv->row > 0 ? -1 : 1));
    if (mv->col & 1) mv->col = static_cast<int16_t>(mv->col + (mv->col > 0 ? -1 : 1));
  }
}

inline int get_mv_joint(const Mv& mv) {
  if (mv.row == 0) return mv.col == 0 ? MV_JOINT_ZERO : MV_JOINT_HNZVZ;
  return mv.col == 0 ? MV_JOINT_HZVNZ : MV_JOINT_HNZVNZ;
}

inline int get_mv_class(int z, int* offset) {
  const int c = (z >= CLASS0_SIZE * 4096) ? MV_CLASS_10 : log_in_base_2[z >> 3];
  *offset = z - (c ? CLASS0_SIZE << (c + 2) : 0);
  return c;
}

}  // namespace

void Core::inc_mv(const Mv& mv) {
  const int j = get_mv_joint(mv);
  ++counts_.mv_joints[j];
  auto inc_component = [](int v, NmvComponentCounts* c) {
    const int s = v < 0;
    ++c->sign[s];
    const int z = (s ? -v : v) - 1;
    int o;
    const int cls = get_mv_class(z, &o);
    ++c->classes[cls];
    const int d = o >> 3, f = (o >> 1) & 3, e = o & 1;
    if (cls == MV_CLASS_0) {
      ++c->class0[d];
      ++c->class0_fp[d][f];
      ++c->class0_hp[e];
    } else {
      const int b = cls + CLASS0_BITS - 1;
      for (int i = 0; i < b; ++i) ++c->bits[i][(d >> i) & 1];
      ++c->fp[f];
      ++c->hp[e];
    }
  };
  if (j == MV_JOINT_HZVNZ || j == MV_JOINT_HNZVNZ) inc_component(mv.row, &counts_.mv_comps[0]);
  if (j == MV_JOINT_HNZVZ || j == MV_JOINT_HNZVNZ) inc_component(mv.col, &counts_.mv_comps[1]);
}

int Core::read_mv_component(BoolDecoder& r, const NmvComponent& comp, bool use_hp) {
  const int sign = r.read(comp.sign);
  const int mv_class = r.tree(vp9_mv_class_tree, comp.classes);
  const bool class0 = mv_class == MV_CLASS_0;
  int mag, d = 0;
  if (class0) {
    d = r.read(comp.class0[0]);
    mag = 0;
  } else {
    const int n = mv_class + CLASS0_BITS - 1;
    for (int i = 0; i < n; ++i) d |= r.read(comp.bits[i]) << i;
    mag = CLASS0_SIZE << (mv_class + 2);
  }
  const int fr = r.tree(vp9_mv_fp_tree, class0 ? comp.class0_fp[d] : comp.fp);
  const int hp = use_hp ? r.read(class0 ? comp.class0_hp : comp.hp) : 1;
  mag += ((d << 3) | (fr << 1) | hp) + 1;
  return sign ? -mag : mag;
}

Mv Core::read_mv_diff(BoolDecoder& r, bool use_hp) {
  const int joint = r.tree(vp9_mv_joint_tree, fc_.nmvc.joints);
  Mv diff;
  if (joint == MV_JOINT_HZVNZ || joint == MV_JOINT_HNZVNZ) diff.row = static_cast<int16_t>(read_mv_component(r, fc_.nmvc.comps[0], use_hp));
  if (joint == MV_JOINT_HNZVZ || joint == MV_JOINT_HNZVNZ) diff.col = static_cast<int16_t>(read_mv_component(r, fc_.nmvc.comps[1], use_hp));
  if (count_enabled_) inc_mv(diff);
  return diff;
}

int Core::read_segment_id(BoolDecoder& r) { return r.tree(vp9_segment_tree, seg_.tree_probs); }

int Core::read_skip(BoolDecoder& r, int segment_id) {
  if (seg_.active(segment_id, SEG_LVL_SKIP)) return 1;
  const int ctx = (above_mi_ ? above_mi_->skip : 0) + (left_mi_ ? left_mi_->skip : 0);
  const int skip = r.read(fc_.skip_probs[ctx]);
  if (count_enabled_) ++counts_.skip[ctx][skip];
  return skip;
}

int Core::read_tx_size(BoolDecoder& r, bool allow_select) {
  const int bsize = cur_->sb_type;
  const int max_tx_size = max_txsize_lookup[bsize];
  if (allow_select && tx_mode_ == TX_MODE_SELECT && bsize >= BLOCK_8X8) {
    int above_ctx = (above_mi_ && !above_mi_->skip) ? above_mi_->tx_size : max_tx_size;
    int left_ctx = (left_mi_ && !left_mi_->skip) ? left_mi_->tx_size : max_tx_size;
    if (!left_mi_) left_ctx = above_ctx;
    if (!above_mi_) above_ctx = left_ctx;
    const int ctx = (above_ctx + left_ctx) > max_tx_size;
    const uint8_t* probs = max_tx_size == TX_8X8 ? fc_.tx_probs.p8x8[ctx] : max_tx_size == TX_16X16 ? fc_.tx_probs.p16x16[ctx] : fc_.tx_probs.p32x32[ctx];
    int tx_size = r.read(probs[0]);
    if (tx_size != TX_4X4 && max_tx_size >= TX_16X16) {
      tx_size += r.read(probs[1]);
      if (tx_size != TX_8X8 && max_tx_size >= TX_32X32) tx_size += r.read(probs[2]);
    }
    if (count_enabled_) {
      if (max_tx_size == TX_8X8) ++counts_.tx.p8x8[ctx][tx_size];
      else if (max_tx_size == TX_16X16) ++counts_.tx.p16x16[ctx][tx_size];
      else ++counts_.tx.p32x32[ctx][tx_size];
    }
    return tx_size;
  }
  return imin(max_tx_size, tx_mode_to_biggest_tx_size[tx_mode_]);
}

int Core::read_intra_mode(BoolDecoder& r, const uint8_t* probs) { return r.tree(vp9_intra_mode_tree, probs); }

int Core::read_inter_mode(BoolDecoder& r, int ctx) {
  const int mode = r.tree(vp9_inter_mode_tree, fc_.inter_mode_probs[ctx]);
  if (count_enabled_) ++counts_.inter_mode[ctx][mode];
  return NEARESTMV + mode;
}

const uint8_t* Core::kf_y_probs(int b) const {
  const ModeInfo* mi = cur_;
  auto y_mode = [](const ModeInfo* m, int block) { return m->sb_type < BLOCK_8X8 ? m->bmi[block].mode : m->mode; };
  int above, left;
  if (b == 0 || b == 1) above = (!above_mi_ || above_mi_->is_inter()) ? DC_PRED : y_mode(above_mi_, b + 2);
  else above = mi->bmi[b - 2].mode;
  if (b == 0 || b == 2) left = (!left_mi_ || left_mi_->is_inter()) ? DC_PRED : y_mode(left_mi_, b + 1);
  else left = mi->bmi[b - 1].mode;
  return vp9_kf_y_mode_prob[above][left];
}

void Core::read_intra_frame_mode_info(BoolDecoder& r, int mi_row, int mi_col, int x_mis, int y_mis) {
  ModeInfo* mi = cur_;
  const int bsize = mi->sb_type;
  const uint64_t offset = static_cast<uint64_t>(mi_row) * mi_cols_ + mi_col;
  ArrayView<uint8_t> cur_map = seg_map_[seg_map_cur_], last_map = seg_map_[seg_map_cur_ ^ 1];
  int segment_id = 0;
  if (seg_.enabled) {
    if (!seg_.update_map) {
      for (int y = 0; y < y_mis; y++)
        for (int x = 0; x < x_mis; x++) cur_map[offset + static_cast<uint64_t>(y) * mi_cols_ + x] = last_map[offset + static_cast<uint64_t>(y) * mi_cols_ + x];
    } else {
      segment_id = read_segment_id(r);
      for (int y = 0; y < y_mis; y++)
        for (int x = 0; x < x_mis; x++) cur_map[offset + static_cast<uint64_t>(y) * mi_cols_ + x] = static_cast<uint8_t>(segment_id);
    }
  }
  mi->segment_id = static_cast<int8_t>(segment_id);
  mi->skip = static_cast<int8_t>(read_skip(r, mi->segment_id));
  mi->tx_size = static_cast<uint8_t>(read_tx_size(r, true));
  mi->ref_frame[0] = INTRA_FRAME;
  mi->ref_frame[1] = NO_REF_FRAME;
  switch (bsize) {
    case BLOCK_4X4:
      for (int i = 0; i < 4; ++i) mi->bmi[i].mode = static_cast<uint8_t>(read_intra_mode(r, kf_y_probs(i)));
      mi->mode = mi->bmi[3].mode;
      break;
    case BLOCK_4X8:
      mi->bmi[0].mode = mi->bmi[2].mode = static_cast<uint8_t>(read_intra_mode(r, kf_y_probs(0)));
      mi->bmi[1].mode = mi->bmi[3].mode = mi->mode = static_cast<uint8_t>(read_intra_mode(r, kf_y_probs(1)));
      break;
    case BLOCK_8X4:
      mi->bmi[0].mode = mi->bmi[1].mode = static_cast<uint8_t>(read_intra_mode(r, kf_y_probs(0)));
      mi->bmi[2].mode = mi->bmi[3].mode = mi->mode = static_cast<uint8_t>(read_intra_mode(r, kf_y_probs(2)));
      break;
    default: mi->mode = static_cast<uint8_t>(read_intra_mode(r, kf_y_probs(0)));
  }
  mi->uv_mode = static_cast<uint8_t>(read_intra_mode(r, vp9_kf_uv_mode_prob[mi->mode]));
}

void Core::read_intra_block_mode_info(BoolDecoder& r) {
  ModeInfo* mi = cur_;
  auto y_mode = [&](int size_group) {
    const int m = read_intra_mode(r, fc_.y_mode_prob[size_group]);
    if (count_enabled_) ++counts_.y_mode[size_group][m];
    return static_cast<uint8_t>(m);
  };
  switch (mi->sb_type) {
    case BLOCK_4X4:
      for (int i = 0; i < 4; ++i) mi->bmi[i].mode = y_mode(0);
      mi->mode = mi->bmi[3].mode;
      break;
    case BLOCK_4X8:
      mi->bmi[0].mode = mi->bmi[2].mode = y_mode(0);
      mi->bmi[1].mode = mi->bmi[3].mode = mi->mode = y_mode(0);
      break;
    case BLOCK_8X4:
      mi->bmi[0].mode = mi->bmi[1].mode = y_mode(0);
      mi->bmi[2].mode = mi->bmi[3].mode = mi->mode = y_mode(0);
      break;
    default: mi->mode = y_mode(size_group_lookup[mi->sb_type]);
  }
  const int uv = read_intra_mode(r, fc_.uv_mode_prob[mi->mode]);
  if (count_enabled_) ++counts_.uv_mode[mi->mode][uv];
  mi->uv_mode = static_cast<uint8_t>(uv);
  mi->interp_filter = SWITCHABLE_FILTERS;
  mi->ref_frame[0] = INTRA_FRAME;
  mi->ref_frame[1] = NO_REF_FRAME;
}

void Core::read_ref_frames(BoolDecoder& r, int segment_id, int8_t ref_frame[2]) {
  if (seg_.active(segment_id, SEG_LVL_REF_FRAME)) {
    ref_frame[0] = static_cast<int8_t>(seg_.feature_data[segment_id][SEG_LVL_REF_FRAME]);
    ref_frame[1] = NO_REF_FRAME;
    return;
  }
  const ModeInfo* above = above_mi_;
  const ModeInfo* left = left_mi_;
  int mode = reference_mode_;
  if (reference_mode_ == 2) {
    int ctx;
    if (above && left) {
      if (!above->has_second_ref() && !left->has_second_ref()) ctx = (above->ref_frame[0] == comp_fixed_ref_) ^ (left->ref_frame[0] == comp_fixed_ref_);
      else if (!above->has_second_ref()) ctx = 2 + (above->ref_frame[0] == comp_fixed_ref_ || !above->is_inter());
      else if (!left->has_second_ref()) ctx = 2 + (left->ref_frame[0] == comp_fixed_ref_ || !left->is_inter());
      else ctx = 4;
    } else if (above || left) {
      const ModeInfo* edge = above ? above : left;
      ctx = !edge->has_second_ref() ? (edge->ref_frame[0] == comp_fixed_ref_) : 3;
    } else {
      ctx = 1;
    }
    mode = r.read(fc_.comp_inter_prob[ctx]);
    if (count_enabled_) ++counts_.comp_inter[ctx][mode];
  }
  if (mode == 1) {  // compound
    const int idx = ref_frame_sign_bias_[comp_fixed_ref_];
    const int fix_ref_idx = idx, var_ref_idx = !fix_ref_idx;
    int ctx;
    if (above && left) {
      const bool above_intra = !above->is_inter(), left_intra = !left->is_inter();
      if (above_intra && left_intra) {
        ctx = 2;
      } else if (above_intra || left_intra) {
        const ModeInfo* edge = above_intra ? left : above;
        if (!edge->has_second_ref()) ctx = 1 + 2 * (edge->ref_frame[0] != comp_var_ref_[1]);
        else ctx = 1 + 2 * (edge->ref_frame[var_ref_idx] != comp_var_ref_[1]);
      } else {
        const bool l_sg = !left->has_second_ref(), a_sg = !above->has_second_ref();
        const int vrfa = a_sg ? above->ref_frame[0] : above->ref_frame[var_ref_idx];
        const int vrfl = l_sg ? left->ref_frame[0] : left->ref_frame[var_ref_idx];
        if (vrfa == vrfl && comp_var_ref_[1] == vrfa) {
          ctx = 0;
        } else if (l_sg && a_sg) {
          if ((vrfa == comp_fixed_ref_ && vrfl == comp_var_ref_[0]) || (vrfl == comp_fixed_ref_ && vrfa == comp_var_ref_[0])) ctx = 4;
          else if (vrfa == vrfl) ctx = 3;
          else ctx = 1;
        } else if (l_sg || a_sg) {
          const int vrfc = l_sg ? vrfa : vrfl;
          const int rfs = a_sg ? vrfa : vrfl;
          if (vrfc == comp_var_ref_[1] && rfs != comp_var_ref_[1]) ctx = 1;
          else if (rfs == comp_var_ref_[1] && vrfc != comp_var_ref_[1]) ctx = 2;
          else ctx = 4;
        } else if (vrfa == vrfl) {
          ctx = 4;
        } else {
          ctx = 2;
        }
      }
    } else if (above || left) {
      const ModeInfo* edge = above ? above : left;
      if (!edge->is_inter()) ctx = 2;
      else if (edge->has_second_ref()) ctx = 4 * (edge->ref_frame[var_ref_idx] != comp_var_ref_[1]);
      else ctx = 3 * (edge->ref_frame[0] != comp_var_ref_[1]);
    } else {
      ctx = 2;
    }
    const int bit = r.read(fc_.comp_ref_prob[ctx]);
    if (count_enabled_) ++counts_.comp_ref[ctx][bit];
    ref_frame[idx] = static_cast<int8_t>(comp_fixed_ref_);
    ref_frame[!idx] = static_cast<int8_t>(comp_var_ref_[bit]);
    return;
  }
  // single reference
  int ctx0;
  if (above && left) {
    const bool above_intra = !above->is_inter(), left_intra = !left->is_inter();
    if (above_intra && left_intra) {
      ctx0 = 2;
    } else if (above_intra || left_intra) {
      const ModeInfo* edge = above_intra ? left : above;
      if (!edge->has_second_ref()) ctx0 = 4 * (edge->ref_frame[0] == LAST_FRAME);
      else ctx0 = 1 + (edge->ref_frame[0] == LAST_FRAME || edge->ref_frame[1] == LAST_FRAME);
    } else {
      const bool a2 = above->has_second_ref(), l2 = left->has_second_ref();
      const int above0 = above->ref_frame[0], above1 = above->ref_frame[1], left0 = left->ref_frame[0], left1 = left->ref_frame[1];
      if (a2 && l2) {
        ctx0 = 1 + (above0 == LAST_FRAME || above1 == LAST_FRAME || left0 == LAST_FRAME || left1 == LAST_FRAME);
      } else if (a2 || l2) {
        const int rfs = !a2 ? above0 : left0;
        const int crf1 = a2 ? above0 : left0;
        const int crf2 = a2 ? above1 : left1;
        if (rfs == LAST_FRAME) ctx0 = 3 + (crf1 == LAST_FRAME || crf2 == LAST_FRAME);
        else ctx0 = (crf1 == LAST_FRAME || crf2 == LAST_FRAME);
      } else {
        ctx0 = 2 * (above0 == LAST_FRAME) + 2 * (left0 == LAST_FRAME);
      }
    }
  } else if (above || left) {
    const ModeInfo* edge = above ? above : left;
    if (!edge->is_inter()) ctx0 = 2;
    else if (!edge->has_second_ref()) ctx0 = 4 * (edge->ref_frame[0] == LAST_FRAME);
    else ctx0 = 1 + (edge->ref_frame[0] == LAST_FRAME || edge->ref_frame[1] == LAST_FRAME);
  } else {
    ctx0 = 2;
  }
  const int bit0 = r.read(fc_.single_ref_prob[ctx0][0]);
  if (count_enabled_) ++counts_.single_ref[ctx0][0][bit0];
  if (bit0) {
    int ctx1;
    if (above && left) {
      const bool above_intra = !above->is_inter(), left_intra = !left->is_inter();
      if (above_intra && left_intra) {
        ctx1 = 2;
      } else if (above_intra || left_intra) {
        const ModeInfo* edge = above_intra ? left : above;
        if (!edge->has_second_ref()) {
          if (edge->ref_frame[0] == LAST_FRAME) ctx1 = 3;
          else ctx1 = 4 * (edge->ref_frame[0] == GOLDEN_FRAME);
        } else {
          ctx1 = 1 + 2 * (edge->ref_frame[0] == GOLDEN_FRAME || edge->ref_frame[1] == GOLDEN_FRAME);
        }
      } else {
        const bool a2 = above->has_second_ref(), l2 = left->has_second_ref();
        const int above0 = above->ref_frame[0], above1 = above->ref_frame[1], left0 = left->ref_frame[0], left1 = left->ref_frame[1];
        if (a2 && l2) {
          if (above0 == left0 && above1 == left1)
            ctx1 = 3 * (above0 == GOLDEN_FRAME || above1 == GOLDEN_FRAME || left0 == GOLDEN_FRAME || left1 == GOLDEN_FRAME);
          else ctx1 = 2;
        } else if (a2 || l2) {
          const int rfs = !a2 ? above0 : left0;
          const int crf1 = a2 ? above0 : left0;
          const int crf2 = a2 ? above1 : left1;
          if (rfs == GOLDEN_FRAME) ctx1 = 3 + (crf1 == GOLDEN_FRAME || crf2 == GOLDEN_FRAME);
          else if (rfs == ALTREF_FRAME) ctx1 = crf1 == GOLDEN_FRAME || crf2 == GOLDEN_FRAME;
          else ctx1 = 1 + 2 * (crf1 == GOLDEN_FRAME || crf2 == GOLDEN_FRAME);
        } else {
          if (above0 == LAST_FRAME && left0 == LAST_FRAME) {
            ctx1 = 3;
          } else if (above0 == LAST_FRAME || left0 == LAST_FRAME) {
            const int edge0 = (above0 == LAST_FRAME) ? left0 : above0;
            ctx1 = 4 * (edge0 == GOLDEN_FRAME);
          } else {
            ctx1 = 2 * (above0 == GOLDEN_FRAME) + 2 * (left0 == GOLDEN_FRAME);
          }
        }
      }
    } else if (above || left) {
      const ModeInfo* edge = above ? above : left;
      if (!edge->is_inter() || (edge->ref_frame[0] == LAST_FRAME && !edge->has_second_ref())) ctx1 = 2;
      else if (!edge->has_second_ref()) ctx1 = 4 * (edge->ref_frame[0] == GOLDEN_FRAME);
      else ctx1 = 3 * (edge->ref_frame[0] == GOLDEN_FRAME || edge->ref_frame[1] == GOLDEN_FRAME);
    } else {
      ctx1 = 2;
    }
    const int bit1 = r.read(fc_.single_ref_prob[ctx1][1]);
    if (count_enabled_) ++counts_.single_ref[ctx1][1][bit1];
    ref_frame[0] = bit1 ? ALTREF_FRAME : GOLDEN_FRAME;
  } else {
    ref_frame[0] = LAST_FRAME;
  }
  ref_frame[1] = NO_REF_FRAME;
}

// The candidate list of motion vectors for `ref_frame` (libvpx's find_mv_refs_idx without the mode context).
void Core::find_mv_refs(int ref_frame, Mv* list, int mi_row, int mi_col, int block) {
  list[0] = list[1] = Mv();
  const Position* search = mv_ref_blocks[cur_->sb_type];
  const MvRef* prev = nullptr;
  if (use_prev_frame_mvs_) prev = &frames_[prev_frame_idx_].mvs[static_cast<uint64_t>(mi_row) * mi_cols_ + mi_col];
  int count = 0;
  bool different_ref_found = false;
  auto add = [&](Mv mv) {
    if (count) {
      if (mv != list[0]) {
        list[count] = mv;
        return true;
      }
      return false;
    }
    list[count++] = mv;
    return false;
  };
  auto inside = [&](const Position& p) {
    return !(mi_row + p.row < 0 || mi_col + p.col < tile_.mi_col_start || mi_row + p.row >= mi_rows_ || mi_col + p.col >= tile_.mi_col_end);
  };
  auto scaled = [&](const Mv& mv, int other_ref) {
    Mv out = mv;
    if (ref_frame_sign_bias_[other_ref] != ref_frame_sign_bias_[ref_frame]) {
      out.row = static_cast<int16_t>(-out.row);
      out.col = static_cast<int16_t>(-out.col);
    }
    return out;
  };
  auto sub_block_mv = [&](const ModeInfo* cand, int which, int search_col) {
    return (block >= 0 && cand->sb_type < BLOCK_8X8) ? cand->bmi[idx_n_column_to_subblock[block][search_col == 0]].mv[which] : cand->mv[which];
  };
  auto search_all = [&]() {
    int i = 0;
    for (; i < 2; ++i) {
      const Position& p = search[i];
      if (!inside(p)) continue;
      const ModeInfo* cand = mi_at(mi_row + p.row, mi_col + p.col);
      different_ref_found = true;
      if (cand->ref_frame[0] == ref_frame) {
        if (add(sub_block_mv(cand, 0, p.col))) return;
      } else if (cand->ref_frame[1] == ref_frame) {
        if (add(sub_block_mv(cand, 1, p.col))) return;
      }
    }
    for (; i < MVREF_NEIGHBOURS; ++i) {
      const Position& p = search[i];
      if (!inside(p)) continue;
      const ModeInfo* cand = mi_at(mi_row + p.row, mi_col + p.col);
      different_ref_found = true;
      if (cand->ref_frame[0] == ref_frame) {
        if (add(cand->mv[0])) return;
      } else if (cand->ref_frame[1] == ref_frame) {
        if (add(cand->mv[1])) return;
      }
    }
    if (prev) {
      if (prev->ref_frame[0] == ref_frame) {
        if (add(prev->mv[0])) return;
      } else if (prev->ref_frame[1] == ref_frame) {
        if (add(prev->mv[1])) return;
      }
    }
    if (different_ref_found) {
      for (i = 0; i < MVREF_NEIGHBOURS; ++i) {
        const Position& p = search[i];
        if (!inside(p)) continue;
        const ModeInfo* cand = mi_at(mi_row + p.row, mi_col + p.col);
        if (cand->is_inter()) {
          if (cand->ref_frame[0] != ref_frame && add(scaled(cand->mv[0], cand->ref_frame[0]))) return;
          if (cand->has_second_ref() && cand->ref_frame[1] != ref_frame && cand->mv[1] != cand->mv[0] &&
              add(scaled(cand->mv[1], cand->ref_frame[1])))
            return;
        }
      }
    }
    if (prev) {
      if (prev->ref_frame[0] != ref_frame && prev->ref_frame[0] > INTRA_FRAME) {
        if (add(scaled(prev->mv[0], prev->ref_frame[0]))) return;
      }
      if (prev->ref_frame[1] > INTRA_FRAME && prev->ref_frame[1] != ref_frame && prev->mv[1] != prev->mv[0]) {
        if (add(scaled(prev->mv[1], prev->ref_frame[1]))) return;
      }
    }
  };
  search_all();
  for (int i = 0; i < 2; ++i) {
    list[i].col = static_cast<int16_t>(clampi(list[i].col, mb_to_left_edge_ - kMvBorder, mb_to_right_edge_ + kMvBorder));
    list[i].row = static_cast<int16_t>(clampi(list[i].row, mb_to_top_edge_ - kMvBorder, mb_to_bottom_edge_ + kMvBorder));
  }
}

void Core::append_sub8x8_mvs(int block, int ref, int mi_row, int mi_col, int b_mode, Mv* best) {
  Mv list[2];
  find_mv_refs(cur_->ref_frame[ref], list, mi_row, mi_col, block);
  const SubBlockInfo* bmi = cur_->bmi;
  Mv nearest, near_mv;
  switch (block) {
    case 0:
      nearest = list[0];
      near_mv = list[1];
      break;
    case 1:
    case 2:
      nearest = bmi[0].mv[ref];
      for (int n = 0; n < 2; ++n)
        if (nearest != list[n]) {
          near_mv = list[n];
          break;
        }
      break;
    default: {
      const Mv candidates[4] = {bmi[1].mv[ref], bmi[0].mv[ref], list[0], list[1]};
      nearest = bmi[2].mv[ref];
      for (const Mv& c : candidates)
        if (nearest != c) {
          near_mv = c;
          break;
        }
      break;
    }
  }
  *best = b_mode == NEARESTMV ? nearest : near_mv;
}

int Core::mode_context(int mi_row, int mi_col) {
  const Position* search = mv_ref_blocks[cur_->sb_type];
  int counter = 0;
  for (int i = 0; i < 2; ++i) {
    const Position& p = search[i];
    if (mi_row + p.row < 0 || mi_col + p.col < tile_.mi_col_start || mi_row + p.row >= mi_rows_ || mi_col + p.col >= tile_.mi_col_end) continue;
    counter += mode_2_counter[mi_at(mi_row + p.row, mi_col + p.col)->mode];
  }
  return counter_to_context[counter];
}

void Core::read_inter_block_mode_info(BoolDecoder& r, int mi_row, int mi_col) {
  ModeInfo* mi = cur_;
  const int bsize = mi->sb_type;
  const bool allow_hp = allow_high_precision_mv_;
  Mv best_ref_mvs[2];
  read_ref_frames(r, mi->segment_id, mi->ref_frame);
  const bool is_compound = mi->has_second_ref();
  const int inter_mode_ctx = mode_context(mi_row, mi_col);
  if (seg_.active(mi->segment_id, SEG_LVL_SKIP)) {
    mi->mode = ZEROMV;
    if (bsize < BLOCK_8X8) {
      corrupted_ = true;  // the segment feature is not allowed on small blocks
      return;
    }
  } else if (bsize >= BLOCK_8X8) {
    mi->mode = static_cast<uint8_t>(read_inter_mode(r, inter_mode_ctx));
  }
  if (interp_filter_ == SWITCHABLE) {
    const int left_type = left_mi_ ? left_mi_->interp_filter : SWITCHABLE_FILTERS;
    const int above_type = above_mi_ ? above_mi_->interp_filter : SWITCHABLE_FILTERS;
    int ctx;
    if (left_type == above_type) ctx = left_type;
    else if (left_type == SWITCHABLE_FILTERS) ctx = above_type;
    else if (above_type == SWITCHABLE_FILTERS) ctx = left_type;
    else ctx = SWITCHABLE_FILTERS;
    const int type = r.tree(vp9_switchable_interp_tree, fc_.switchable_interp_prob[ctx]);
    if (count_enabled_) ++counts_.switchable_interp[ctx][type];
    mi->interp_filter = static_cast<uint8_t>(type);
  } else {
    mi->interp_filter = static_cast<uint8_t>(interp_filter_);
  }
  auto assign_mv = [&](int mode, Mv mv[2], const Mv ref_mv[2], const Mv near_nearest[2]) {
    switch (mode) {
      case NEWMV:
        for (int i = 0; i < 1 + is_compound; ++i) {
          const Mv& ref = ref_mv[i];
          const Mv diff = read_mv_diff(r, allow_hp && use_mv_hp(ref));
          const int row = ref.row + diff.row, col = ref.col + diff.col;
          if (!(row > kMvLow && row < kMvUpp && col > kMvLow && col < kMvUpp)) return false;
          mv[i].row = static_cast<int16_t>(row);
          mv[i].col = static_cast<int16_t>(col);
        }
        return true;
      case NEARMV:
      case NEARESTMV:
        mv[0] = near_nearest[0];
        mv[1] = near_nearest[1];
        return true;
      case ZEROMV:
        mv[0] = mv[1] = Mv();
        return true;
      default: return false;
    }
  };
  if (bsize < BLOCK_8X8) {
    const int num_4x4_w = 1 << bmode_blocks_wl_, num_4x4_h = 1 << bmode_blocks_hl_;
    int b_mode = ZEROMV;
    bool got_mv_refs_for_new = false;
    Mv best_sub8x8[2];
    for (int idy = 0; idy < 2; idy += num_4x4_h) {
      for (int idx = 0; idx < 2; idx += num_4x4_w) {
        const int j = idy * 2 + idx;
        b_mode = read_inter_mode(r, inter_mode_ctx);
        if (b_mode == NEARESTMV || b_mode == NEARMV) {
          for (int ref = 0; ref < 1 + is_compound; ++ref) append_sub8x8_mvs(j, ref, mi_row, mi_col, b_mode, &best_sub8x8[ref]);
        } else if (b_mode == NEWMV && !got_mv_refs_for_new) {
          for (int ref = 0; ref < 1 + is_compound; ++ref) {
            Mv tmp[2];
            find_mv_refs(mi->ref_frame[ref], tmp, mi_row, mi_col, -1);
            lower_mv_precision(&tmp[0], allow_hp);
            best_ref_mvs[ref] = tmp[0];
          }
          got_mv_refs_for_new = true;
        }
        Mv mv[2];
        if (!assign_mv(b_mode, mv, best_ref_mvs, best_sub8x8)) {
          corrupted_ = true;
          return;
        }
        mi->bmi[j].mv[0] = mv[0];
        mi->bmi[j].mv[1] = mv[1];
        mi->bmi[j].mode = static_cast<uint8_t>(b_mode);
        if (num_4x4_h == 2) mi->bmi[j + 2] = mi->bmi[j];
        if (num_4x4_w == 2) mi->bmi[j + 1] = mi->bmi[j];
      }
    }
    mi->mode = static_cast<uint8_t>(b_mode);
    mi->mv[0] = mi->bmi[3].mv[0];
    mi->mv[1] = mi->bmi[3].mv[1];
  } else {
    if (mi->mode != ZEROMV) {
      for (int ref = 0; ref < 1 + is_compound; ++ref) {
        Mv tmp[2];
        find_mv_refs(mi->ref_frame[ref], tmp, mi_row, mi_col, -1);
        Mv best = tmp[mi->mode == NEARMV ? 1 : 0];
        lower_mv_precision(&best, allow_hp);
        best_ref_mvs[ref] = best;
      }
    }
    if (!assign_mv(mi->mode, mi->mv, best_ref_mvs, best_ref_mvs)) corrupted_ = true;
  }
}

void Core::read_inter_frame_mode_info(BoolDecoder& r, int mi_row, int mi_col, int x_mis, int y_mis) {
  ModeInfo* mi = cur_;
  const uint64_t offset = static_cast<uint64_t>(mi_row) * mi_cols_ + mi_col;
  ArrayView<uint8_t> cur_map = seg_map_[seg_map_cur_], last_map = seg_map_[seg_map_cur_ ^ 1];
  int segment_id = 0;
  if (seg_.enabled) {
    int predicted = MAX_SEGMENTS;
    for (int y = 0; y < y_mis; y++)
      for (int x = 0; x < x_mis; x++) predicted = imin(predicted, last_map[offset + static_cast<uint64_t>(y) * mi_cols_ + x]);
    if (!seg_.update_map) {
      for (int y = 0; y < y_mis; y++)
        for (int x = 0; x < x_mis; x++) cur_map[offset + static_cast<uint64_t>(y) * mi_cols_ + x] = last_map[offset + static_cast<uint64_t>(y) * mi_cols_ + x];
      segment_id = predicted;
    } else {
      if (seg_.temporal_update) {
        const int ctx = (above_mi_ ? above_mi_->seg_id_predicted : 0) + (left_mi_ ? left_mi_->seg_id_predicted : 0);
        mi->seg_id_predicted = static_cast<int8_t>(r.read(seg_.pred_probs[ctx]));
        segment_id = mi->seg_id_predicted ? predicted : read_segment_id(r);
      } else {
        segment_id = read_segment_id(r);
      }
      for (int y = 0; y < y_mis; y++)
        for (int x = 0; x < x_mis; x++) cur_map[offset + static_cast<uint64_t>(y) * mi_cols_ + x] = static_cast<uint8_t>(segment_id);
    }
  }
  mi->segment_id = static_cast<int8_t>(segment_id);
  mi->skip = static_cast<int8_t>(read_skip(r, segment_id));
  bool inter_block;
  if (seg_.active(segment_id, SEG_LVL_REF_FRAME)) {
    inter_block = seg_.feature_data[segment_id][SEG_LVL_REF_FRAME] != INTRA_FRAME;
  } else {
    int ctx;
    if (above_mi_ && left_mi_) {
      const bool above_intra = !above_mi_->is_inter(), left_intra = !left_mi_->is_inter();
      ctx = left_intra && above_intra ? 3 : (left_intra || above_intra);
    } else if (above_mi_ || left_mi_) {
      ctx = 2 * !(above_mi_ ? above_mi_ : left_mi_)->is_inter();
    } else {
      ctx = 0;
    }
    inter_block = r.read(fc_.intra_inter_prob[ctx]) != 0;
    if (count_enabled_) ++counts_.intra_inter[ctx][inter_block];
  }
  mi->tx_size = static_cast<uint8_t>(read_tx_size(r, !mi->skip || !inter_block));
  if (inter_block) read_inter_block_mode_info(r, mi_row, mi_col);
  else read_intra_block_mode_info(r);
}

void Core::read_mode_info(BoolDecoder& r, int mi_row, int mi_col, int x_mis, int y_mis) {
  if (frame_type_ == 0 || intra_only_) {
    read_intra_frame_mode_info(r, mi_row, mi_col, x_mis, y_mis);
    return;
  }
  read_inter_frame_mode_info(r, mi_row, mi_col, x_mis, y_mis);
  MvRef ref;
  ref.ref_frame[0] = cur_->ref_frame[0];
  ref.ref_frame[1] = cur_->ref_frame[1];
  ref.mv[0] = cur_->mv[0];
  ref.mv[1] = cur_->mv[1];
  Frame& f = frames_[new_fb_idx_];
  for (int h = 0; h < y_mis; ++h)
    for (int w = 0; w < x_mis; ++w) f.mvs[static_cast<uint64_t>(mi_row + h) * mi_cols_ + mi_col + w] = ref;
}

// ---- partitions and blocks -----------------------------------------------------------------------------------------------------

int Core::read_partition(BoolDecoder& r, int mi_row, int mi_col, bool has_rows, bool has_cols, int bsl) {
  const int above = (above_part_ctx_[mi_col] >> bsl) & 1, left = (left_part_ctx_[mi_row & 7] >> bsl) & 1;
  const int ctx = (left * 2 + above) + bsl * PARTITION_PLOFFSET;
  const uint8_t* probs = (frame_type_ == 0 || intra_only_) ? vp9_kf_partition_probs[ctx] : fc_.partition_prob[ctx];
  int p;
  if (has_rows && has_cols) p = r.tree(vp9_partition_tree, probs);
  else if (!has_rows && has_cols) p = r.read(probs[1]) ? PARTITION_SPLIT : PARTITION_HORZ;
  else if (has_rows && !has_cols) p = r.read(probs[2]) ? PARTITION_SPLIT : PARTITION_VERT;
  else p = PARTITION_SPLIT;
  if (count_enabled_) ++counts_.partition[ctx][p];
  return p;
}

void Core::decode_block(BoolDecoder& r, int mi_row, int mi_col, int bsize, int bwl, int bhl) {
  const int bw = 1 << (bwl - 1), bh = 1 << (bhl - 1);
  const int x_mis = imin(bw, mi_cols_ - mi_col), y_mis = imin(bh, mi_rows_ - mi_row);
  const uint64_t offset = static_cast<uint64_t>(mi_row) * mi_cols_ + mi_col;
  cur_ = &mi_[offset];
  *cur_ = ModeInfo();
  cur_->sb_type = static_cast<uint8_t>(bsize);
  for (int y = 0; y < y_mis; ++y)
    for (int x = 0; x < x_mis; ++x) grid_[offset + static_cast<uint64_t>(y) * mi_cols_ + x] = static_cast<int32_t>(offset);
  for (int i = 0; i < 3; ++i) {
    plane_[i].ss_x = i ? ss_x_ : 0;
    plane_[i].ss_y = i ? ss_y_ : 0;
    plane_[i].n4_w = static_cast<uint16_t>((bw << 1) >> plane_[i].ss_x);
    plane_[i].n4_h = static_cast<uint16_t>((bh << 1) >> plane_[i].ss_y);
    plane_[i].n4_wl = static_cast<uint8_t>(bwl - plane_[i].ss_x);
    plane_[i].n4_hl = static_cast<uint8_t>(bhl - plane_[i].ss_y);
    a_off_[i] = (mi_col * 2) >> plane_[i].ss_x;
    l_off_[i] = ((mi_row * 2) & 15) >> plane_[i].ss_y;
  }
  mb_to_top_edge_ = -((mi_row * 8) * 8);
  mb_to_bottom_edge_ = ((mi_rows_ - bh - mi_row) * 8) * 8;
  mb_to_left_edge_ = -((mi_col * 8) * 8);
  mb_to_right_edge_ = ((mi_cols_ - bw - mi_col) * 8) * 8;
  above_mi_ = mi_row != 0 ? mi_at(mi_row - 1, mi_col) : nullptr;
  left_mi_ = mi_col > tile_.mi_col_start ? mi_at(mi_row, mi_col - 1) : nullptr;
  cur_row_ = mi_row;
  cur_col_ = mi_col;
  if (bsize >= BLOCK_8X8 && (ss_x_ || ss_y_) && ss_size_lookup[bsize][ss_x_][ss_y_] == BLOCK_INVALID) {
    corrupted_ = true;
    return;
  }
  read_mode_info(r, mi_row, mi_col, x_mis, y_mis);
  if (corrupted_) return;
  if (cur_->skip) {
    for (int i = 0; i < 3; ++i) {
      for (int k = 0; k < plane_[i].n4_w; ++k) above_ctx_[i][static_cast<uint64_t>(a_off_[i]) + k] = 0;
      for (int k = 0; k < plane_[i].n4_h; ++k) left_ctx_[i][(l_off_[i] + k) & 15] = 0;
    }
  }
  reconstruct_block(r, mi_row, mi_col, bsize);
  if (r.error()) corrupted_ = true;
}

void Core::decode_partition(BoolDecoder& r, int mi_row, int mi_col, int bsize, int n4x4_l2) {
  const int n8x8_l2 = n4x4_l2 - 1;
  const int num_8x8_wh = 1 << n8x8_l2;
  const int hbs = num_8x8_wh >> 1;
  const bool has_rows = (mi_row + hbs) < mi_rows_, has_cols = (mi_col + hbs) < mi_cols_;
  if (mi_row >= mi_rows_ || mi_col >= mi_cols_) return;
  const int partition = read_partition(r, mi_row, mi_col, has_rows, has_cols, n8x8_l2);
  const int subsize = subsize_lookup[partition][bsize];
  if (!hbs) {
    bmode_blocks_wl_ = 1 >> !!(partition & PARTITION_VERT);
    bmode_blocks_hl_ = 1 >> !!(partition & PARTITION_HORZ);
    decode_block(r, mi_row, mi_col, subsize, 1, 1);
  } else {
    switch (partition) {
      case PARTITION_NONE: decode_block(r, mi_row, mi_col, subsize, n4x4_l2, n4x4_l2); break;
      case PARTITION_HORZ:
        decode_block(r, mi_row, mi_col, subsize, n4x4_l2, n8x8_l2);
        if (has_rows) decode_block(r, mi_row + hbs, mi_col, subsize, n4x4_l2, n8x8_l2);
        break;
      case PARTITION_VERT:
        decode_block(r, mi_row, mi_col, subsize, n8x8_l2, n4x4_l2);
        if (has_cols) decode_block(r, mi_row, mi_col + hbs, subsize, n8x8_l2, n4x4_l2);
        break;
      default:
        decode_partition(r, mi_row, mi_col, subsize, n8x8_l2);
        decode_partition(r, mi_row, mi_col + hbs, subsize, n8x8_l2);
        decode_partition(r, mi_row + hbs, mi_col, subsize, n8x8_l2);
        decode_partition(r, mi_row + hbs, mi_col + hbs, subsize, n8x8_l2);
        break;
    }
  }
  if (corrupted_) return;
  if (bsize >= BLOCK_8X8 && (bsize == BLOCK_8X8 || partition != PARTITION_SPLIT)) {
    for (int k = 0; k < num_8x8_wh; ++k) {
      above_part_ctx_[static_cast<uint64_t>(mi_col) + k] = partition_context_lookup[subsize].above;
      left_part_ctx_[(mi_row + k) & 7] = partition_context_lookup[subsize].left;
    }
  }
}

// ---- coefficient tokens --------------------------------------------------------------------------------------------------------

// Reads the tokens of one transform block into dqcoeff_ (already dequantized). Returns the end-of-block position.
int Core::decode_block_tokens(BoolDecoder& r, int plane, int tx_size, int tx_type, int x, int y, int seg_id) {
  const int16_t* dq = (plane ? uv_dequant_ : y_dequant_)[seg_id];
  uint8_t* a = &above_ctx_[plane][static_cast<uint64_t>(a_off_[plane]) + static_cast<uint64_t>(x)];
  const int n = 1 << tx_size;
  // The contexts of the edge: entries beyond the picture are kept at zero.
  int valid_a = n, valid_l = n;
  if (max_blocks_wide_ && n + x > max_blocks_wide_) valid_a = max_blocks_wide_ - x;
  if (max_blocks_high_ && n + y > max_blocks_high_) valid_l = max_blocks_high_ - y;
  int any_a = 0, any_l = 0;
  for (int i = 0; i < n; ++i) {
    any_a |= a[i];
    any_l |= left_ctx_[plane][(l_off_[plane] + y + i) & 15];
  }
  int ctx = (any_a != 0) + (any_l != 0);
  const int16_t *scan, *nb;
  get_scan(tx_size, tx_type, &scan, &nb);
  const int ref = cur_->is_inter();
  const int max_eob = 16 << (tx_size << 1);
  const int type = plane > 0;
  const uint8_t(*coef_probs)[COEFF_CONTEXTS][UNCONSTRAINED_NODES] = fc_.coef_probs[tx_size][type][ref];
  unsigned(*coef_counts)[COEFF_CONTEXTS][UNCONSTRAINED_NODES + 1] = counts_.coef[tx_size][type][ref];
  unsigned(*eob_branch)[COEFF_CONTEXTS] = counts_.eob_branch[tx_size][type][ref];
  const uint8_t* band_translate = tx_size == TX_4X4 ? vp9_coefband_trans_4x4 : vp9_coefband_trans_8x8plus;
  const int dq_shift = tx_size == TX_32X32;
  const bool counts = count_enabled_;
  const uint8_t* cat6_prob = bit_depth_ == 12 ? vp9_cat6_prob_high12 : bit_depth_ == 10 ? vp9_cat6_prob_high12 + 2 : vp9_cat6_prob_high12 + 4;
  const int cat6_bits = bit_depth_ == 12 ? 18 : bit_depth_ == 10 ? 16 : 14;
  uint8_t token_cache[32 * 32];
  int dqv = dq[0];
  int c = 0;
  auto read_coeff = [&](const uint8_t* probs, int bits) {
    int v = 0;
    for (int i = 0; i < bits; ++i) v = (v << 1) | r.read(probs[i]);
    return v;
  };
  auto coef_context = [&](int pos) { return (1 + token_cache[static_cast<unsigned>(nb[2 * pos])] + token_cache[static_cast<unsigned>(nb[2 * pos + 1])]) >> 1; };
  while (c < max_eob) {
    int val;
    int band = *band_translate++;
    const uint8_t* prob = coef_probs[band][ctx];
    if (counts) ++eob_branch[band][ctx];
    if (!r.read(prob[0])) {
      if (counts) ++coef_counts[band][ctx][3];
      break;
    }
    bool ended = false;
    while (!r.read(prob[1])) {
      if (counts) ++coef_counts[band][ctx][0];
      dqv = dq[1];
      token_cache[scan[c]] = 0;
      ++c;
      if (c >= max_eob) {
        ended = true;
        break;
      }
      ctx = coef_context(c);
      band = *band_translate++;
      prob = coef_probs[band][ctx];
    }
    if (ended) break;
    int v;
    if (r.read(prob[2])) {
      const uint8_t* p = vp9_pareto8_full[prob[2] - 1];
      if (counts) ++coef_counts[band][ctx][2];
      if (r.read(p[0])) {
        if (r.read(p[3])) {
          token_cache[scan[c]] = 5;
          if (r.read(p[5])) {
            if (r.read(p[7])) val = 67 + read_coeff(cat6_prob, cat6_bits);
            else val = 35 + read_coeff(vp9_cat5_prob, 5);
          } else if (r.read(p[6])) {
            val = 19 + read_coeff(vp9_cat4_prob, 4);
          } else {
            val = 11 + read_coeff(vp9_cat3_prob, 3);
          }
        } else {
          token_cache[scan[c]] = 4;
          if (r.read(p[4])) val = 7 + read_coeff(vp9_cat2_prob, 2);
          else val = 5 + read_coeff(vp9_cat1_prob, 1);
        }
        v = static_cast<int>((static_cast<int64_t>(val) * dqv) >> dq_shift);
      } else if (r.read(p[1])) {
        token_cache[scan[c]] = 3;
        v = ((3 + r.read(p[2])) * dqv) >> dq_shift;
      } else {
        token_cache[scan[c]] = 2;
        v = (2 * dqv) >> dq_shift;
      }
    } else {
      if (counts) ++coef_counts[band][ctx][1];
      token_cache[scan[c]] = 1;
      v = dqv >> dq_shift;
    }
    dqcoeff_[static_cast<unsigned>(scan[c])] = r.read(128) ? -v : v;
    ++c;
    ctx = coef_context(c);
    dqv = dq[1];
  }
  const uint8_t flag = c > 0;
  for (int i = 0; i < n; ++i) {
    a[i] = i < valid_a ? flag : 0;
    left_ctx_[plane][(l_off_[plane] + y + i) & 15] = i < valid_l ? flag : 0;
  }
  return c;
}

// ---- tiles and the frame -------------------------------------------------------------------------------------------------------

bool Core::decode_tiles(ConstByteView data) {
  const int tile_cols = 1 << log2_tile_cols_, tile_rows = 1 << log2_tile_rows_;
  for (auto& a : above_ctx_)
    for (uint8_t& v : a) v = 0;
  for (int8_t& v : above_part_ctx_) v = 0;
  ConstByteView rest = data;
  for (int tr = 0; tr < tile_rows; ++tr) {
    for (int tc = 0; tc < tile_cols; ++tc) {
      const bool is_last = tr == tile_rows - 1 && tc == tile_cols - 1;
      ConstByteView buf;
      if (!is_last) {
        if (rest.size() < 4) return false;
        const uint64_t size = (uint64_t{rest[0]} << 24) | (uint64_t{rest[1]} << 16) | (uint64_t{rest[2]} << 8) | rest[3];
        rest = rest.subview(4, rest.size() - 4);
        if (size > rest.size()) return false;
        buf = rest.subview(0, size);
        rest = rest.subview(size, rest.size() - size);
      } else {
        buf = rest;
      }
      auto tile_offset = [&](int idx, int mis, int log2) {
        const int sb_cols = mi_cols_aligned_to_sb(mis) >> 3;
        const int offset = ((idx * sb_cols) >> log2) << 3;
        return imin(offset, mis);
      };
      tile_.mi_row_start = tile_offset(tr, mi_rows_, log2_tile_rows_);
      tile_.mi_row_end = tile_offset(tr + 1, mi_rows_, log2_tile_rows_);
      tile_.mi_col_start = tile_offset(tc, mi_cols_, log2_tile_cols_);
      tile_.mi_col_end = tile_offset(tc + 1, mi_cols_, log2_tile_cols_);
      BoolDecoder r;
      if (!r.init(buf)) return false;
      for (int mi_row = tile_.mi_row_start; mi_row < tile_.mi_row_end; mi_row += 8) {
        for (auto& l : left_ctx_)
          for (uint8_t& v : l) v = 0;
        for (int8_t& v : left_part_ctx_) v = 0;
        for (int mi_col = tile_.mi_col_start; mi_col < tile_.mi_col_end; mi_col += 8) {
          decode_partition(r, mi_row, mi_col, BLOCK_64X64, 4);
          if (corrupted_ || r.error()) return false;
        }
      }
    }
  }
  return true;
}

bool Core::decode_frame(ConstByteView data, int* shown, FrameHeaderInfo* info) {
  *shown = -1;
  *info = FrameHeaderInfo();
  new_fb_idx_ = -1;
  auto fail = [&]() {
    if (new_fb_idx_ >= 0) frames_[new_fb_idx_].ref_count = 0;
    new_fb_idx_ = -1;
    need_resync_ = true;
    return false;
  };
  BitReader rb(data);
  uint32_t header_size = 0;
  if (!read_uncompressed_header(rb, &header_size, shown, info)) return fail();
  if (info->show_existing_frame) return true;
  if (header_size == 0) return fail();
  const uint64_t offset = rb.bytes_read();
  if (offset > data.size() || header_size > data.size() - offset) return fail();
  use_prev_frame_mvs_ = !error_resilient_ && width_ == last_width_ && height_ == last_height_ && !last_intra_only_ && last_show_frame_ &&
                        last_frame_type_ != 0 && prev_frame_idx_ >= 0 && frames_[prev_frame_idx_].mi_cols == mi_cols_ &&
                        frames_[prev_frame_idx_].mi_rows == mi_rows_;
  fc_ = frame_contexts_[frame_context_idx_];
  if (!fc_.initialized) return fail();
  corrupted_ = false;
  if (!read_compressed_header(data.subview(offset, header_size))) return fail();
  count_enabled_ = !frame_parallel_decoding_mode_;
  if (count_enabled_) __builtin_memset(&counts_, 0, sizeof counts_);
  for (ModeInfo& m : mi_) m = ModeInfo();
  for (int32_t& g : grid_) g = 0;
  if (lf_.filter_level) loop_filter_frame_init();
  cur_frame_ = &frames_[new_fb_idx_];
  if (!decode_tiles(data.subview(offset + header_size, data.size() - offset - header_size))) return fail();
  if (!error_resilient_ && !frame_parallel_decoding_mode_) {
    adapt_coef_probs();
    if (!(frame_type_ == 0 || intra_only_)) {
      adapt_mode_probs();
      adapt_mv_probs();
    }
  }
  if (refresh_frame_context_) frame_contexts_[frame_context_idx_] = fc_;
  if (lf_.filter_level) loop_filter_frame();
  // Install the picture in the reference slots it refreshes.
  for (int i = 0; i < REF_FRAMES; ++i) {
    if (!((refresh_frame_flags_ >> i) & 1)) continue;
    const int old = ref_frame_map_[i];
    if (old >= 0) --frames_[old].ref_count;
    ref_frame_map_[i] = new_fb_idx_;
    ++frames_[new_fb_idx_].ref_count;
  }
  --frames_[new_fb_idx_].ref_count;
  last_show_frame_ = show_frame_;
  prev_frame_idx_ = new_fb_idx_;
  if (seg_.enabled) seg_map_cur_ ^= 1;
  last_width_ = width_;
  last_height_ = height_;
  return true;
}

}  // namespace vp9
}  // namespace hardware
