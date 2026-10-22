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


// VP9 decoding, part 3: the loop filter. A port of libvpx's per-block path (BSD-3-Clause, Copyright (c) The WebM Project authors) that works
// for every chroma subsampling; the filters themselves are written once for all bit depths.
#include "Hardware/VideoCodec/Software/Vp9Core.hpp"

namespace hardware {
namespace vp9 {

namespace {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int iabs(int v) { return v < 0 ? -v : v; }

// Filters `count` positions along an edge. `across` is the distance between samples on the two sides of the edge, `along` the step to the
// next position. taps is 4, 8 or 16 (the widest filter the edge may use).
void filter_edge(uint16_t* s, int across, int along, int count, int taps, int lim, int mblim, int hev_thr, int bd) {
  const int shift = bd - 8;
  const int limit = lim << shift, blimit = mblim << shift, thresh = hev_thr << shift, one = 1 << shift;
  const int lo = -(128 << shift), hi = (128 << shift) - 1, offset = 0x80 << shift;
  auto clampS = [&](int t) { return clampi(t, lo, hi); };
  for (int i = 0; i < count; ++i, s += along) {
    const int p3 = s[-4 * across], p2 = s[-3 * across], p1 = s[-2 * across], p0 = s[-across];
    const int q0 = s[0], q1 = s[across], q2 = s[2 * across], q3 = s[3 * across];
    const bool mask = !(iabs(p3 - p2) > limit || iabs(p2 - p1) > limit || iabs(p1 - p0) > limit || iabs(q1 - q0) > limit ||
                        iabs(q2 - q1) > limit || iabs(q3 - q2) > limit || iabs(p0 - q0) * 2 + iabs(p1 - q1) / 2 > blimit);
    if (!mask) continue;
    bool flat = false, flat2 = false;
    if (taps >= 8) {
      flat = !(iabs(p1 - p0) > one || iabs(q1 - q0) > one || iabs(p2 - p0) > one || iabs(q2 - q0) > one || iabs(p3 - p0) > one ||
               iabs(q3 - q0) > one);
      if (taps == 16 && flat) {
        const int p4 = s[-5 * across], p5 = s[-6 * across], p6 = s[-7 * across], p7 = s[-8 * across];
        const int q4 = s[4 * across], q5 = s[5 * across], q6 = s[6 * across], q7 = s[7 * across];
        flat2 = !(iabs(p4 - p0) > one || iabs(q4 - q0) > one || iabs(p5 - p0) > one || iabs(q5 - q0) > one || iabs(p6 - p0) > one ||
                  iabs(q6 - q0) > one || iabs(p7 - p0) > one || iabs(q7 - q0) > one);
      }
    }
    if (flat2) {
      int v[16];
      for (int k = 0; k < 8; ++k) {
        v[k] = s[(k - 8) * across];
        v[8 + k] = s[k * across];
      }
      // v[0..7] = p7..p0, v[8..15] = q0..q7: every output is a 15-tap window with the centre weighted twice and the ends repeated.
      auto sample = [&](int idx) { return v[idx < 0 ? 0 : idx > 15 ? 15 : idx]; };
      for (int k = 1; k <= 14; ++k) {
        int sum = 0;
        for (int t = -7; t <= 7; ++t) sum += sample(k + t);
        sum += v[k];
        s[(k - 8) * across] = static_cast<uint16_t>((sum + 8) >> 4);
      }
    } else if (flat) {
      s[-3 * across] = static_cast<uint16_t>((p3 + p3 + p3 + 2 * p2 + p1 + p0 + q0 + 4) >> 3);
      s[-2 * across] = static_cast<uint16_t>((p3 + p3 + p2 + 2 * p1 + p0 + q0 + q1 + 4) >> 3);
      s[-across] = static_cast<uint16_t>((p3 + p2 + p1 + 2 * p0 + q0 + q1 + q2 + 4) >> 3);
      s[0] = static_cast<uint16_t>((p2 + p1 + p0 + 2 * q0 + q1 + q2 + q3 + 4) >> 3);
      s[across] = static_cast<uint16_t>((p1 + p0 + q0 + 2 * q1 + q2 + q3 + q3 + 4) >> 3);
      s[2 * across] = static_cast<uint16_t>((p0 + q0 + q1 + 2 * q2 + q3 + q3 + q3 + 4) >> 3);
    } else {
      const int ps1 = p1 - offset, ps0 = p0 - offset, qs0 = q0 - offset, qs1 = q1 - offset;
      const bool hev = iabs(p1 - p0) > thresh || iabs(q1 - q0) > thresh;
      int filter = hev ? clampS(ps1 - qs1) : 0;
      filter = clampS(filter + 3 * (qs0 - ps0));
      const int filter1 = clampS(filter + 4) >> 3, filter2 = clampS(filter + 3) >> 3;
      s[0] = static_cast<uint16_t>(clampS(qs0 - filter1) + offset);
      s[-across] = static_cast<uint16_t>(clampS(ps0 + filter2) + offset);
      filter = hev ? 0 : (filter1 + 1) >> 1;
      s[across] = static_cast<uint16_t>(clampS(qs1 - filter) + offset);
      s[-2 * across] = static_cast<uint16_t>(clampS(ps1 + filter) + offset);
    }
  }
}

}  // namespace

void Core::loop_filter_frame_init() {
  const int default_filt_lvl = lf_.filter_level;
  const int scale = 1 << (default_filt_lvl >> 5);
  for (int lvl = 0; lvl <= MAX_LOOP_FILTER; lvl++) {
    int block_inside_limit = lvl >> ((lf_.sharpness_level > 0) + (lf_.sharpness_level > 4));
    if (lf_.sharpness_level > 0 && block_inside_limit > (9 - lf_.sharpness_level)) block_inside_limit = 9 - lf_.sharpness_level;
    if (block_inside_limit < 1) block_inside_limit = 1;
    lf_lim_[lvl] = static_cast<uint8_t>(block_inside_limit);
    lf_mblim_[lvl] = static_cast<uint8_t>(2 * (lvl + 2) + block_inside_limit);
  }
  lf_.last_sharpness_level = lf_.sharpness_level;
  for (int seg_id = 0; seg_id < MAX_SEGMENTS; seg_id++) {
    int lvl_seg = default_filt_lvl;
    if (seg_.active(seg_id, SEG_LVL_ALT_LF)) {
      const int data = seg_.feature_data[seg_id][SEG_LVL_ALT_LF];
      lvl_seg = clampi(seg_.abs_delta ? data : default_filt_lvl + data, 0, MAX_LOOP_FILTER);
    }
    if (!lf_.mode_ref_delta_enabled) {
      for (auto& ref : lf_lvl_[seg_id])
        for (uint8_t& v : ref) v = static_cast<uint8_t>(lvl_seg);
    } else {
      lf_lvl_[seg_id][INTRA_FRAME][0] = static_cast<uint8_t>(clampi(lvl_seg + lf_.ref_deltas[INTRA_FRAME] * scale, 0, MAX_LOOP_FILTER));
      for (int ref = LAST_FRAME; ref < MAX_REF_FRAMES; ++ref)
        for (int mode = 0; mode < 2; ++mode)
          lf_lvl_[seg_id][ref][mode] =
              static_cast<uint8_t>(clampi(lvl_seg + lf_.ref_deltas[ref] * scale + lf_.mode_deltas[mode] * scale, 0, MAX_LOOP_FILTER));
    }
  }
}

void Core::filter_block_plane(int plane, int mi_row, int mi_col) {
  Frame& f = frames_[new_fb_idx_];
  const int ss_x = plane ? ss_x_ : 0, ss_y = plane ? ss_y_ : 0;
  const int row_step = 1 << ss_y, col_step = 1 << ss_x;
  const int stride = static_cast<int>(f.stride[plane]);
  ArrayView<uint16_t> pix = f.plane[plane];
  const int64_t origin = int64_t{(mi_row * 8) >> ss_y} * stride + ((mi_col * 8) >> ss_x);
  unsigned mask_16x16[8] = {}, mask_8x8[8] = {}, mask_4x4[8] = {}, mask_4x4_int[8] = {};
  uint8_t lfl[64] = {};
  const int bd = bit_depth_;
  // A pointer to the sample at `index` with `before`/`after` samples of room around it, checked against the plane.
  auto at = [&](int64_t index, int64_t before, int64_t after) -> uint16_t* {
    const int64_t first = index - before;
    const int64_t total = before + after + 1;
    return pix.subview(static_cast<uint64_t>(first), static_cast<uint64_t>(total)).data() + before;
  };
  for (int r = 0; r < 8 && mi_row + r < mi_rows_; r += row_step) {
    unsigned mask_16x16_c = 0, mask_8x8_c = 0, mask_4x4_c = 0;
    for (int c = 0; c < 8 && mi_col + c < mi_cols_; c += col_step) {
      const ModeInfo* mi = mi_at(mi_row + r, mi_col + c);
      const int sb_type = mi->sb_type;
      const bool skip_this = mi->skip && mi->is_inter();
      const bool block_edge_left = (num_4x4_blocks_wide_lookup[sb_type] > 1) ? !(c & (num_8x8_blocks_wide_lookup[sb_type] - 1)) : true;
      const bool skip_this_c = skip_this && !block_edge_left;
      const bool block_edge_above = (num_4x4_blocks_high_lookup[sb_type] > 1) ? !(r & (num_8x8_blocks_high_lookup[sb_type] - 1)) : true;
      const bool skip_this_r = skip_this && !block_edge_above;
      const int tx_size = plane ? uv_txsize_lookup[sb_type][mi->tx_size][ss_x][ss_y] : mi->tx_size;
      const bool skip_border_4x4_c = ss_x && mi_col + c == mi_cols_ - 1;
      const bool skip_border_4x4_r = ss_y && mi_row + r == mi_rows_ - 1;
      const int level = lf_lvl_[mi->segment_id][mi->ref_frame[0]][mode_lf_lut[mi->mode]];
      lfl[(r << 3) + (c >> ss_x)] = static_cast<uint8_t>(level);
      if (!level) continue;
      const unsigned bit = 1u << (c >> ss_x);
      if (tx_size == TX_32X32) {
        if (!skip_this_c && ((c >> ss_x) & 3) == 0) {
          if (!skip_border_4x4_c) mask_16x16_c |= bit;
          else mask_8x8_c |= bit;
        }
        if (!skip_this_r && ((r >> ss_y) & 3) == 0) {
          if (!skip_border_4x4_r) mask_16x16[r] |= bit;
          else mask_8x8[r] |= bit;
        }
      } else if (tx_size == TX_16X16) {
        if (!skip_this_c && ((c >> ss_x) & 1) == 0) {
          if (!skip_border_4x4_c) mask_16x16_c |= bit;
          else mask_8x8_c |= bit;
        }
        if (!skip_this_r && ((r >> ss_y) & 1) == 0) {
          if (!skip_border_4x4_r) mask_16x16[r] |= bit;
          else mask_8x8[r] |= bit;
        }
      } else {
        if (!skip_this_c) {
          if (tx_size == TX_8X8 || ((c >> ss_x) & 3) == 0) mask_8x8_c |= bit;
          else mask_4x4_c |= bit;
        }
        if (!skip_this_r) {
          if (tx_size == TX_8X8 || ((r >> ss_y) & 3) == 0) mask_8x8[r] |= bit;
          else mask_4x4[r] |= bit;
        }
        if (!skip_this && tx_size < TX_8X8 && !skip_border_4x4_c) mask_4x4_int[r] |= bit;
      }
    }
    // Vertical edges of this row of 8-sample strips.
    const unsigned border_mask = ~(mi_col == 0 ? 1u : 0u);
    mask_16x16_c &= border_mask;
    mask_8x8_c &= border_mask;
    mask_4x4_c &= border_mask;
    const int64_t row_index = origin + int64_t{(r >> ss_y) * 8} * stride;
    for (int u = 0; u < 8; ++u) {
      const unsigned bit = 1u << u;
      const int level = lfl[(r << 3) + u];
      const int64_t x = row_index + int64_t{8} * u;
      if (mask_16x16_c & bit) {
        filter_edge(at(x, 8, 7 + 7 * int64_t{stride}), 1, stride, 8, 16, lf_lim_[level], lf_mblim_[level], level >> 4, bd);
      } else if (mask_8x8_c & bit) {
        filter_edge(at(x, 4, 3 + 7 * int64_t{stride}), 1, stride, 8, 8, lf_lim_[level], lf_mblim_[level], level >> 4, bd);
      } else if (mask_4x4_c & bit) {
        filter_edge(at(x, 4, 3 + 7 * int64_t{stride}), 1, stride, 8, 4, lf_lim_[level], lf_mblim_[level], level >> 4, bd);
      }
      if (mask_4x4_int[r] & bit)
        filter_edge(at(x + 4, 4, 3 + 7 * int64_t{stride}), 1, stride, 8, 4, lf_lim_[level], lf_mblim_[level], level >> 4, bd);
    }
  }
  // Horizontal edges.
  for (int r = 0; r < 8 && mi_row + r < mi_rows_; r += row_step) {
    const bool skip_border_4x4_r = ss_y && mi_row + r == mi_rows_ - 1;
    const unsigned m4_int = skip_border_4x4_r ? 0 : mask_4x4_int[r];
    unsigned m16 = 0, m8 = 0, m4 = 0;
    if (mi_row + r != 0) {
      m16 = mask_16x16[r];
      m8 = mask_8x8[r];
      m4 = mask_4x4[r];
    }
    const int64_t row_index = origin + int64_t{(r >> ss_y) * 8} * stride;
    for (int u = 0; u < 8; ++u) {
      const unsigned bit = 1u << u;
      const int level = lfl[(r << 3) + u];
      const int64_t x = row_index + int64_t{8} * u;
      if (!((m16 | m8 | m4 | m4_int) & bit)) continue;
      if (m16 & bit) {
        filter_edge(at(x, 8 * int64_t{stride}, 7 * int64_t{stride} + 7), stride, 1, 8, 16, lf_lim_[level], lf_mblim_[level], level >> 4, bd);
      } else if (m8 & bit) {
        filter_edge(at(x, 4 * int64_t{stride}, 3 * int64_t{stride} + 7), stride, 1, 8, 8, lf_lim_[level], lf_mblim_[level], level >> 4, bd);
        if (m4_int & bit)
          filter_edge(at(x + 4 * int64_t{stride}, 4 * int64_t{stride}, 3 * int64_t{stride} + 7), stride, 1, 8, 4, lf_lim_[level],
                      lf_mblim_[level], level >> 4, bd);
      } else if (m4 & bit) {
        filter_edge(at(x, 4 * int64_t{stride}, 3 * int64_t{stride} + 7), stride, 1, 8, 4, lf_lim_[level], lf_mblim_[level], level >> 4, bd);
        if (m4_int & bit)
          filter_edge(at(x + 4 * int64_t{stride}, 4 * int64_t{stride}, 3 * int64_t{stride} + 7), stride, 1, 8, 4, lf_lim_[level],
                      lf_mblim_[level], level >> 4, bd);
      } else {
        filter_edge(at(x + 4 * int64_t{stride}, 4 * int64_t{stride}, 3 * int64_t{stride} + 7), stride, 1, 8, 4, lf_lim_[level],
                    lf_mblim_[level], level >> 4, bd);
      }
    }
  }
}

void Core::loop_filter_frame() {
  for (int mi_row = 0; mi_row < mi_rows_; mi_row += 8)
    for (int mi_col = 0; mi_col < mi_cols_; mi_col += 8)
      for (int plane = 0; plane < 3; ++plane) filter_block_plane(plane, mi_row, mi_col);
}

}  // namespace vp9
}  // namespace hardware
