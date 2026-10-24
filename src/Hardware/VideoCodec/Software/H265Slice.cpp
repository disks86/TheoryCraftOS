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


// H.265 decoding, part 3: slice segment data - the coding tree, the CABAC syntax of coding units, prediction units and transform trees, residual coding, and the
// prediction of motion vectors (clauses 7.3.8, 9.3, 8.4 and 8.5.3 of ITU-T H.265).
#include "Hardware/VideoCodec/Software/H265Core.hpp"
#include "Hardware/VideoCodec/Software/H265Trace.hpp"

namespace hardware {
namespace h265 {

namespace {
constexpr int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
constexpr int imin(int a, int b) { return a < b ? a : b; }
constexpr int imax(int a, int b) { return a > b ? a : b; }
constexpr int iabs(int a) { return a < 0 ? -a : a; }

enum PartMode : int { kPart2Nx2N = 0, kPart2NxN = 1, kPartNx2N = 2, kPartNxN = 3, kPart2NxnU = 4, kPart2NxnD = 5, kPartnLx2N = 6, kPartnRx2N = 7 };
enum { kPredL0 = 0, kPredL1 = 1, kPredBi = 2 };

// 6.5.3 - 6.5.5: scan orders of blocks of 1x1 to 8x8 positions. kScan[type][log2][i] is (x, y); type 0 up-right diagonal, 1 horizontal, 2 vertical.
struct ScanTable {
  uint8_t x[3][4][64];
  uint8_t y[3][4][64];
};
constexpr ScanTable make_scans() {
  ScanTable t{};
  for (int log2 = 0; log2 < 4; ++log2) {
    const int n = 1 << log2;
    int i = 0, x = 0, y = 0;
    bool stop = false;
    while (!stop) {
      while (y >= 0) {
        if (x < n && y < n) {
          t.x[0][log2][i] = static_cast<uint8_t>(x);
          t.y[0][log2][i] = static_cast<uint8_t>(y);
          ++i;
        }
        --y;
        ++x;
      }
      y = x;
      x = 0;
      if (i >= n * n) stop = true;
    }
    for (int k = 0; k < n * n; ++k) {
      t.x[1][log2][k] = static_cast<uint8_t>(k % n);
      t.y[1][log2][k] = static_cast<uint8_t>(k / n);
      t.x[2][log2][k] = static_cast<uint8_t>(k / n);
      t.y[2][log2][k] = static_cast<uint8_t>(k % n);
    }
  }
  return t;
}
constexpr ScanTable kScan = make_scans();

constexpr uint8_t kCtxIdxMap4x4[16] = {0, 1, 4, 5, 2, 3, 4, 5, 6, 6, 8, 8, 7, 7, 8, 8};
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Availability (6.4)

// 6.4.1: is the block at (x_n, y_n) available for the block at (x_cur, y_cur)? It must be in the picture, already decoded, in the same slice and the same tile.
bool Core::available(int x_cur, int y_cur, int x_n, int y_n) const {
  const Sps& s = *sp_;
  if (x_n < 0 || y_n < 0 || x_n >= s.width || y_n >= s.height) return false;
  const uint64_t n = static_cast<uint64_t>(y_n >> 2) * w4_ + (x_n >> 2);
  const uint64_t c = static_cast<uint64_t>(y_cur >> 2) * w4_ + (x_cur >> 2);
  if (zscan_[n] > zscan_[c]) return false;
  const int ctb_n = (y_n >> s.log2_ctb) * ctb_w_ + (x_n >> s.log2_ctb);
  const int si = ctb_slice_[ctb_n];
  if (si < 0) return false;
  if (slices_[static_cast<uint64_t>(si)].slice_addr_rs != slice_addr_rs_) return false;
  return tile_id_ts_[rs_to_ts_[ctb_n]] == tile_id_ts_[ctb_addr_ts_];
}

// 6.4.2: availability of a neighbouring prediction block.
bool Core::available_pb(int x_cb, int y_cb, int n_cb, int x_pb, int y_pb, int w, int h, int part_idx, int x_n, int y_n) const {
  bool same_cb = x_cb <= x_n && y_cb <= y_n && x_cb + n_cb > x_n && y_cb + n_cb > y_n;
  bool avail;
  if (!same_cb) {
    avail = available(x_pb, y_pb, x_n, y_n);
  } else if ((w << 1) == n_cb && (h << 1) == n_cb && part_idx == 1 && y_cb + h <= y_n && x_cb + w > x_n) {
    avail = false;
  } else {
    avail = true;
  }
  if (avail && (pred_mode_[static_cast<uint64_t>(y_n >> 2) * w4_ + (x_n >> 2)] & 3) != 2) avail = false;   // intra blocks have no motion
  return avail;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Slice segment data (7.3.8.1)

bool Core::decode_slice_data(const SliceHeader& sh, BitReader& br) {
  const Sps& sps = *sp_;
  const Pps& pps = *pp_;
  if (sh.pps_id != pp_->id || &sps_[pps.sps_id] != sp_) {
    // A slice of the picture that names another parameter set than the first slice did: it must be the same one.
    if (&pps_[sh.pps_id] != pp_ && pps_[sh.pps_id].sps_id != pps.sps_id) return H265_FALSE;
    pp_ = &pps_[sh.pps_id];
  }
  sh_ = sh;
  SliceInfo info;
  if (!sh.dependent_slice_segment) {
    info.slice_addr_rs = sh.segment_address;
    info.deblocking_disabled = sh.deblocking_disabled;
    info.beta_offset_div2 = sh.beta_offset_div2;
    info.tc_offset_div2 = sh.tc_offset_div2;
    info.loop_filter_across_slices = sh.loop_filter_across_slices;
    info.cb_qp_offset = sh.cb_qp_offset;
    info.cr_qp_offset = sh.cr_qp_offset;
    info.sao_luma = sh.sao_luma;
    info.sao_chroma = sh.sao_chroma;
    if (!build_ref_lists(sh, &info)) return H265_FALSE;
    first_header_ = sh;
    slice_addr_rs_ = sh.segment_address;
    qp_y_prev_ = sh.slice_qp;
  } else {
    if (slices_.count() == 0) return H265_FALSE;
    info = slices_[slices_.count() - 1];
  }
  if (!slices_.push_back(info)) return H265_FALSE;
  slice_idx_ = static_cast<int>(slices_.count()) - 1;
  slice_ = &slices_[static_cast<uint64_t>(slice_idx_)];
  // NoBackwardPredFlag (8.5.3.2.9): no reference picture follows the current picture in output order.
  no_backward_pred_ = true;
  for (int l = 0; l < 2; ++l)
    for (int i = 0; i < sh.num_ref_idx[l]; ++i)
      if (slice_->ref_poc[l][i] > poc_) no_backward_pred_ = false;

  br_ = &br;
  br.seek(sh.data_bit_offset);
  cabac_.attach(&br);
  const int init_type = sh.type == kSliceI ? 0 : sh.type == kSliceP ? (sh.cabac_init_flag ? 2 : 1) : (sh.cabac_init_flag ? 1 : 2);
  const int total_ctbs = ctb_w_ * ctb_h_;
  int ts = rs_to_ts_[sh.segment_address];
  bool first = true;
  cabac_.init_engine();
  for (;;) {
    ctb_addr_ts_ = ts;
    ctb_addr_rs_ = ts_to_rs_[ts];
    const int rx = ctb_addr_rs_ % ctb_w_, ry = ctb_addr_rs_ / ctb_w_;
    const int x_ctb = rx << sps.log2_ctb, y_ctb = ry << sps.log2_ctb;
    ctb_slice_[ctb_addr_rs_] = slice_idx_;
    const bool tile_start = ts == 0 || tile_id_ts_[ts] != tile_id_ts_[ts - 1];
    int tile_col = 0;
    for (int i = 0; i < pps.num_tile_cols; ++i)
      if (rx >= col_bd_[i]) tile_col = i;
    const bool row_start = pps.entropy_coding_sync && rx == col_bd_[tile_col];
    if (first || tile_start || row_start) {
      bool fresh = true;
      if (tile_start) {
        fresh = true;
      } else if (row_start) {
        if (available(x_ctb, y_ctb, x_ctb + sps.ctb_size, y_ctb - sps.ctb_size) && wpp_saved_valid_) {
          cabac_.restore(wpp_saved_);
          fresh = false;
        } else if (first && sh.dependent_slice_segment && ctx_end_valid_) {
          cabac_.restore(ctx_end_of_segment_);
          fresh = false;
        }
      } else if (first && sh.dependent_slice_segment && ctx_end_valid_) {
        cabac_.restore(ctx_end_of_segment_);
        fresh = false;
      }
      if (fresh) cabac_.init_contexts(init_type, sh.slice_qp);
      if (tile_start || row_start) qp_y_prev_ = sh.slice_qp;
    }
    first = false;

    // coding_tree_unit()
    if (sh.sao_luma || sh.sao_chroma) read_sao(rx, ry);
    coding_quadtree(x_ctb, y_ctb, sps.log2_ctb, 0);
    const bool end_of_segment = cabac_.terminate() != 0;
    if (br.error()) return H265_FALSE;
    if (pps.entropy_coding_sync && rx == col_bd_[tile_col] + 1) {
      cabac_.save(wpp_saved_);
      wpp_saved_valid_ = true;
    }
    ++ts;
    if (end_of_segment) {
      cabac_.save(ctx_end_of_segment_);
      ctx_end_valid_ = true;
      return true;
    }
    if (ts >= total_ctbs) return H265_FALSE;
    const int next_rs = ts_to_rs_[ts];
    const int next_rx = next_rs % ctb_w_;
    int next_tile_col = 0;
    for (int i = 0; i < pps.num_tile_cols; ++i)
      if (next_rx >= col_bd_[i]) next_tile_col = i;
    const bool new_tile = pps.tiles_enabled && tile_id_ts_[ts] != tile_id_ts_[ts - 1];
    const bool new_row = pps.entropy_coding_sync && next_rx == col_bd_[next_tile_col];
    if (new_tile || new_row) {
      if (!cabac_.terminate()) return H265_FALSE;   // end_of_subset_one_bit
      br.align();
      cabac_.init_engine();
    }
  }
}

void Cabac::init_contexts(int init_type, int slice_qp) {
  static const uint8_t kInit[3][kContextCount] =
#include "Hardware/VideoCodec/Software/H265CabacInit.inc"
      ;
  const int qp = clip3(0, 51, slice_qp);
  for (int i = 0; i < kContextCount; ++i) {
    const int v = kInit[init_type][i];
    const int slope = v >> 4, offset = v & 15;
    const int m = slope * 5 - 45, n = (offset << 3) - 16;
    const int pre = clip3(1, 126, ((m * qp) >> 4) + n);
    if (pre <= 63) state_[i] = static_cast<uint8_t>((63 - pre) << 1);
    else state_[i] = static_cast<uint8_t>(((pre - 64) << 1) | 1);
  }
}

// 7.3.8.3
void Core::read_sao(int rx, int ry) {
  const Sps& sps = *sp_;
  SaoParams& p = sao_[ctb_addr_rs_];
  p = SaoParams();
  bool merge_left = false, merge_up = false;
  if (rx > 0) {
    const bool left_in_slice = ctb_addr_rs_ > slice_addr_rs_;
    const bool left_in_tile = tile_id_ts_[ctb_addr_ts_] == tile_id_ts_[rs_to_ts_[ctb_addr_rs_ - 1]];
    if (left_in_slice && left_in_tile) merge_left = cabac_.decision(kCtxSaoMerge) != 0;
  }
  if (ry > 0 && !merge_left) {
    const bool up_in_slice = (ctb_addr_rs_ - ctb_w_) >= slice_addr_rs_;
    const bool up_in_tile = tile_id_ts_[ctb_addr_ts_] == tile_id_ts_[rs_to_ts_[ctb_addr_rs_ - ctb_w_]];
    if (up_in_slice && up_in_tile) merge_up = cabac_.decision(kCtxSaoMerge) != 0;
  }
  if (merge_left) {
    p = sao_[ctb_addr_rs_ - 1];
    return;
  }
  if (merge_up) {
    p = sao_[ctb_addr_rs_ - ctb_w_];
    return;
  }
  for (int c = 0; c < 3; ++c) {
    if (!((sh_.sao_luma && c == 0) || (sh_.sao_chroma && c > 0))) continue;
    if (c == 0 || c == 1) {
      int type = 0;
      if (cabac_.decision(kCtxSaoType)) type = cabac_.bypass() ? 2 : 1;
      p.type[c] = static_cast<uint8_t>(type);
    } else {
      p.type[2] = p.type[1];
    }
    if (p.type[c] == 0) continue;
    int abs_offset[4];
    for (int i = 0; i < 4; ++i) {
      int v = 0;
      while (v < 7 && cabac_.bypass()) ++v;   // TR with cMax (1 << (min(bitDepth, 10) - 5)) - 1 = 7
      abs_offset[i] = v;
    }
    if (p.type[c] == 1) {
      int sign[4] = {0, 0, 0, 0};
      for (int i = 0; i < 4; ++i)
        if (abs_offset[i] != 0) sign[i] = cabac_.bypass();
      p.band_position[c] = static_cast<uint8_t>(cabac_.bypass_bits(5));
      for (int i = 0; i < 4; ++i) p.offset[c][i] = static_cast<int8_t>(sign[i] ? -abs_offset[i] : abs_offset[i]);
    } else {
      if (c == 0) p.eo_class[0] = static_cast<uint8_t>(cabac_.bypass_bits(2));
      else if (c == 1) p.eo_class[1] = static_cast<uint8_t>(cabac_.bypass_bits(2));
      else p.eo_class[2] = p.eo_class[1];
      p.offset[c][0] = static_cast<int8_t>(abs_offset[0]);
      p.offset[c][1] = static_cast<int8_t>(abs_offset[1]);
      p.offset[c][2] = static_cast<int8_t>(-abs_offset[2]);
      p.offset[c][3] = static_cast<int8_t>(-abs_offset[3]);
    }
  }
  (void)sps;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Coding quadtree and coding unit

void Core::coding_quadtree(int x0, int y0, int log2_size, int depth) {
  const Sps& sps = *sp_;
  const Pps& pps = *pp_;
  const int size = 1 << log2_size;
  bool split;
  if (x0 + size <= sps.width && y0 + size <= sps.height && log2_size > sps.log2_min_cb) {
    int inc = 0;
    if (available(x0, y0, x0 - 1, y0) && ct_depth_[static_cast<uint64_t>(y0 >> 2) * w4_ + ((x0 - 1) >> 2)] > depth) ++inc;
    if (available(x0, y0, x0, y0 - 1) && ct_depth_[static_cast<uint64_t>((y0 - 1) >> 2) * w4_ + (x0 >> 2)] > depth) ++inc;
    split = cabac_.decision(kCtxSplitCu + inc) != 0;
  } else {
    split = log2_size > sps.log2_min_cb;
  }
  if (pps.cu_qp_delta_enabled && log2_size >= sps.log2_ctb - pps.diff_cu_qp_delta_depth) {
    is_cu_qp_delta_coded_ = false;
    cu_qp_delta_val_ = 0;
    qg_x_ = x0;
    qg_y_ = y0;
    // 8.6.1: the predicted quantiser of the group
    int a = qp_y_prev_, b = qp_y_prev_;
    const int ctb_mask = ~(sps.ctb_size - 1);
    if (available(x0, y0, x0 - 1, y0) && ((x0 - 1) & ctb_mask) == (x0 & ctb_mask)) a = qp_map_[static_cast<uint64_t>(y0 >> 2) * w4_ + ((x0 - 1) >> 2)];
    if (available(x0, y0, x0, y0 - 1) && ((y0 - 1) & ctb_mask) == (y0 & ctb_mask)) b = qp_map_[static_cast<uint64_t>((y0 - 1) >> 2) * w4_ + (x0 >> 2)];
    qg_pred_ = (a + b + 1) >> 1;
  } else if (!pps.cu_qp_delta_enabled && log2_size == sps.log2_ctb) {
    qg_pred_ = sh_.slice_qp;
    cu_qp_delta_val_ = 0;
  }
  if (split) {
    const int half = size >> 1;
    const int x1 = x0 + half, y1 = y0 + half;
    coding_quadtree(x0, y0, log2_size - 1, depth + 1);
    if (x1 < sps.width) coding_quadtree(x1, y0, log2_size - 1, depth + 1);
    if (y1 < sps.height) coding_quadtree(x0, y1, log2_size - 1, depth + 1);
    if (x1 < sps.width && y1 < sps.height) coding_quadtree(x1, y1, log2_size - 1, depth + 1);
  } else {
    // remember the depth over the block, for the split flags of later neighbours
    for (int y = y0 >> 2; y < (y0 + size) >> 2; ++y)
      for (int x = x0 >> 2; x < (x0 + size) >> 2; ++x) ct_depth_[static_cast<uint64_t>(y) * w4_ + x] = static_cast<uint8_t>(depth);
    coding_unit(x0, y0, log2_size);
  }
}

void Core::set_qp_block(int x0, int y0, int w, int h, int qp) {
  for (int y = y0 >> 2; y < (y0 + h) >> 2; ++y)
    for (int x = x0 >> 2; x < (x0 + w) >> 2; ++x) qp_map_[static_cast<uint64_t>(y) * w4_ + x] = static_cast<int8_t>(qp);
}

// QpY of the coding unit (8-283), from the group's prediction and the delta decoded so far.
void Core::update_qp(int x_cb, int y_cb) {
  qp_y_ = ((qg_pred_ + cu_qp_delta_val_ + 52) % 52);
  set_qp_block(x_cb, y_cb, 1 << cur_cb_log2_, 1 << cur_cb_log2_, qp_y_);
}

// 8.6.1: the quantiser of a chroma block
int Core::chroma_qp(int c_idx) const {
  static const int kQpC[14] = {29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37};
  const int offset = c_idx == 1 ? pp_->cb_qp_offset + slice_->cb_qp_offset : pp_->cr_qp_offset + slice_->cr_qp_offset;
  const int qpi = clip3(0, 57, qp_y_ + offset);
  if (qpi < 30) return qpi;
  if (qpi >= 43) return qpi - 6;
  return kQpC[qpi - 30];
}

void Core::coding_unit(int x0, int y0, int log2_size) {
  const Sps& sps = *sp_;
  const Pps& pps = *pp_;
  const int n_cb = 1 << log2_size;
  cur_cb_x_ = x0;
  cur_cb_y_ = y0;
  cur_cb_log2_ = log2_size;
  cu_transquant_bypass_ = false;
  if (pps.transquant_bypass) cu_transquant_bypass_ = cabac_.decision(kCtxTransquantBypass) != 0;
  bool skip = false;
  if (sh_.type != kSliceI) {
    int inc = 0;
    if (available(x0, y0, x0 - 1, y0) && (pred_mode_[static_cast<uint64_t>(y0 >> 2) * w4_ + ((x0 - 1) >> 2)] & 4)) ++inc;
    if (available(x0, y0, x0, y0 - 1) && (pred_mode_[static_cast<uint64_t>((y0 - 1) >> 2) * w4_ + (x0 >> 2)] & 4)) ++inc;
    skip = cabac_.decision(kCtxSkip + inc) != 0;
  }
  cu_skip_ = skip;
  update_qp(x0, y0);
  auto fill_mode = [&](int mode_bits) {
    for (int y = y0 >> 2; y < (y0 + n_cb) >> 2; ++y)
      for (int x = x0 >> 2; x < (x0 + n_cb) >> 2; ++x) {
        const uint64_t i = static_cast<uint64_t>(y) * w4_ + x;
        pred_mode_[i] = static_cast<uint8_t>(mode_bits | (cu_transquant_bypass_ ? 8 : 0));
        nz_[i] = 0;
        edge_[i] = 0;
      }
  };
  // The edges of the coding block are transform block and prediction block edges.
  auto mark_cb_edges = [&]() {
    for (int i = 0; i < n_cb >> 2; ++i) {
      edge_[static_cast<uint64_t>((y0 >> 2) + i) * w4_ + (x0 >> 2)] |= 3;
      edge_[static_cast<uint64_t>(y0 >> 2) * w4_ + (x0 >> 2) + i] |= 12;
    }
  };
  if (skip) {
    fill_mode(2 | 4);
    for (int y = y0 >> 2; y < (y0 + n_cb) >> 2; ++y)
      for (int x = x0 >> 2; x < (x0 + n_cb) >> 2; ++x) intra_mode_[static_cast<uint64_t>(y) * w4_ + x] = 1;
    mark_cb_edges();
    cur_intra_ = false;
    cur_part_mode_ = kPart2Nx2N;
    prediction_unit(x0, y0, n_cb, n_cb, 0, log2_size, x0, y0);
    qp_y_prev_ = qp_y_;
    return;
  }
  bool intra = true;
  if (sh_.type != kSliceI) intra = cabac_.decision(kCtxPredMode) != 0;
  cur_intra_ = intra;
  int part_mode = kPart2Nx2N;
  if (!intra || log2_size == sps.log2_min_cb) {
    // part_mode (9.3.4.2): the bins of Table 9-43
    if (cabac_.decision(kCtxPartMode)) {
      part_mode = kPart2Nx2N;
    } else if (intra) {
      part_mode = kPartNxN;
    } else if (log2_size == sps.log2_min_cb) {
      if (cabac_.decision(kCtxPartMode + 1)) part_mode = kPart2NxN;
      else if (log2_size == 3) part_mode = kPartNx2N;
      else part_mode = cabac_.decision(kCtxPartMode + 2) ? kPartNx2N : kPartNxN;
    } else if (!sps.amp) {
      part_mode = cabac_.decision(kCtxPartMode + 1) ? kPart2NxN : kPartNx2N;
    } else if (cabac_.decision(kCtxPartMode + 1)) {
      if (cabac_.decision(kCtxPartMode + 3)) part_mode = kPart2NxN;
      else part_mode = cabac_.bypass() ? kPart2NxnD : kPart2NxnU;
    } else {
      if (cabac_.decision(kCtxPartMode + 3)) part_mode = kPartNx2N;
      else part_mode = cabac_.bypass() ? kPartnRx2N : kPartnLx2N;
    }
  }
  cur_part_mode_ = part_mode;
  mark_cb_edges();
  bool pcm = false;
  if (intra) {
    fill_mode(1);
    if (part_mode == kPart2Nx2N && sps.pcm && log2_size >= sps.log2_min_pcm && log2_size <= sps.log2_max_pcm) pcm = cabac_.terminate() != 0;
    if (pcm) {
      read_pcm(x0, y0, log2_size);
      if (sps.pcm_loop_filter_disabled)
        for (int y = y0 >> 2; y < (y0 + n_cb) >> 2; ++y)
          for (int x = x0 >> 2; x < (x0 + n_cb) >> 2; ++x) pred_mode_[static_cast<uint64_t>(y) * w4_ + x] |= 8;
      for (int y = y0 >> 2; y < (y0 + n_cb) >> 2; ++y)
        for (int x = x0 >> 2; x < (x0 + n_cb) >> 2; ++x) intra_mode_[static_cast<uint64_t>(y) * w4_ + x] = 1;
      qp_y_prev_ = qp_y_;
      return;
    }
    // luma intra prediction modes (7.3.8.5, 8.4.2)
    const int parts = part_mode == kPartNxN ? 4 : 1;
    const int pb = part_mode == kPartNxN ? n_cb >> 1 : n_cb;
    int prev_flag[4], mpm_idx[4], rem_mode[4];
    for (int i = 0; i < parts; ++i) prev_flag[i] = cabac_.decision(kCtxPrevIntraLuma);
    for (int i = 0; i < parts; ++i) {
      mpm_idx[i] = rem_mode[i] = 0;
      if (prev_flag[i]) {
        if (cabac_.bypass()) mpm_idx[i] = cabac_.bypass() ? 2 : 1;
      } else {
        rem_mode[i] = cabac_.bypass_bits(5);
      }
    }
    for (int i = 0; i < parts; ++i) {
      const int xp = x0 + (i & 1) * pb, yp = y0 + (i >> 1) * pb;
      int cand_a = 1, cand_b = 1;
      if (available(xp, yp, xp - 1, yp)) {
        const uint64_t k = static_cast<uint64_t>(yp >> 2) * w4_ + ((xp - 1) >> 2);
        if ((pred_mode_[k] & 3) == 1) cand_a = intra_mode_[k];
      }
      if (available(xp, yp, xp, yp - 1) && yp - 1 >= ((yp >> sps.log2_ctb) << sps.log2_ctb)) {
        const uint64_t k = static_cast<uint64_t>((yp - 1) >> 2) * w4_ + (xp >> 2);
        if ((pred_mode_[k] & 3) == 1) cand_b = intra_mode_[k];
      }
      int list[3];
      if (cand_a == cand_b) {
        if (cand_a < 2) {
          list[0] = 0; list[1] = 1; list[2] = 26;
        } else {
          list[0] = cand_a;
          list[1] = 2 + ((cand_a - 2 - 1 + 32) & 31);
          list[2] = 2 + ((cand_a - 2 + 1) & 31);
        }
      } else {
        list[0] = cand_a;
        list[1] = cand_b;
        if (cand_a != 0 && cand_b != 0) list[2] = 0;
        else if (cand_a != 1 && cand_b != 1) list[2] = 1;
        else list[2] = 26;
      }
      int mode;
      if (prev_flag[i]) {
        mode = list[mpm_idx[i]];
      } else {
        if (list[0] > list[1]) { const int t = list[0]; list[0] = list[1]; list[1] = t; }
        if (list[0] > list[2]) { const int t = list[0]; list[0] = list[2]; list[2] = t; }
        if (list[1] > list[2]) { const int t = list[1]; list[1] = list[2]; list[2] = t; }
        mode = rem_mode[i];
        for (int k = 0; k < 3; ++k)
          if (mode >= list[k]) ++mode;
      }
      intra_luma_modes_[i] = mode;
      for (int y = yp >> 2; y < (yp + pb) >> 2; ++y)
        for (int x = xp >> 2; x < (xp + pb) >> 2; ++x) intra_mode_[static_cast<uint64_t>(y) * w4_ + x] = static_cast<uint8_t>(mode);
    }
    // chroma mode (8.4.3, 4:2:0)
    int chroma_syntax = 4;
    if (cabac_.decision(kCtxChromaPred)) chroma_syntax = cabac_.bypass_bits(2);
    static const int kChromaModes[4] = {0, 26, 10, 1};
    const int luma0 = intra_luma_modes_[0];
    int chroma_mode;
    if (chroma_syntax == 4) chroma_mode = luma0;
    else chroma_mode = kChromaModes[chroma_syntax] == luma0 ? 34 : kChromaModes[chroma_syntax];
    for (int i = 0; i < 4; ++i) intra_chroma_mode_[i] = chroma_mode;
    if (parts == 1) for (int i = 1; i < 4; ++i) intra_luma_modes_[i] = intra_luma_modes_[0];
  } else {
    fill_mode(2);
    for (int y = y0 >> 2; y < (y0 + n_cb) >> 2; ++y)
      for (int x = x0 >> 2; x < (x0 + n_cb) >> 2; ++x) intra_mode_[static_cast<uint64_t>(y) * w4_ + x] = 1;
    const int h2 = n_cb >> 1, q = n_cb >> 2;
    switch (part_mode) {
      case kPart2Nx2N: prediction_unit(x0, y0, n_cb, n_cb, 0, log2_size, x0, y0); break;
      case kPart2NxN: prediction_unit(x0, y0, n_cb, h2, 0, log2_size, x0, y0); prediction_unit(x0, y0 + h2, n_cb, h2, 1, log2_size, x0, y0); break;
      case kPartNx2N: prediction_unit(x0, y0, h2, n_cb, 0, log2_size, x0, y0); prediction_unit(x0 + h2, y0, h2, n_cb, 1, log2_size, x0, y0); break;
      case kPart2NxnU: prediction_unit(x0, y0, n_cb, q, 0, log2_size, x0, y0); prediction_unit(x0, y0 + q, n_cb, n_cb - q, 1, log2_size, x0, y0); break;
      case kPart2NxnD: prediction_unit(x0, y0, n_cb, n_cb - q, 0, log2_size, x0, y0); prediction_unit(x0, y0 + n_cb - q, n_cb, q, 1, log2_size, x0, y0); break;
      case kPartnLx2N: prediction_unit(x0, y0, q, n_cb, 0, log2_size, x0, y0); prediction_unit(x0 + q, y0, n_cb - q, n_cb, 1, log2_size, x0, y0); break;
      case kPartnRx2N: prediction_unit(x0, y0, n_cb - q, n_cb, 0, log2_size, x0, y0); prediction_unit(x0 + n_cb - q, y0, q, n_cb, 1, log2_size, x0, y0); break;
      default:
        prediction_unit(x0, y0, h2, h2, 0, log2_size, x0, y0);
        prediction_unit(x0 + h2, y0, h2, h2, 1, log2_size, x0, y0);
        prediction_unit(x0, y0 + h2, h2, h2, 2, log2_size, x0, y0);
        prediction_unit(x0 + h2, y0 + h2, h2, h2, 3, log2_size, x0, y0);
        break;
    }
  }
  // residual
  bool rqt_root_cbf = true;
  if (!intra && !(part_mode == kPart2Nx2N && merge_flag_cu_)) rqt_root_cbf = cabac_.decision(kCtxRqtRootCbf) != 0;
  if (rqt_root_cbf) {
    const int max_depth = intra ? sps.max_th_depth_intra + (part_mode == kPartNxN ? 1 : 0) : sps.max_th_depth_inter;
    max_trafo_depth_ = max_depth;
    const bool inter_split = sps.max_th_depth_inter == 0 && !intra && part_mode != kPart2Nx2N;
    bool cbf_cb[2] = {true, true}, cbf_cr[2] = {true, true};
    transform_tree(x0, y0, x0, y0, log2_size, 0, 0, cbf_cb, cbf_cr, inter_split ? 1 : 0);
  } else if (intra) {
    // (cannot happen: intra coding units always have a transform tree)
  }
  qp_y_prev_ = qp_y_;
}

void Core::read_pcm(int x0, int y0, int log2_size) {
  const Sps& sps = *sp_;
  br_->align();
  const int n = 1 << log2_size;
  const int shift_y = 8 - sps.pcm_bit_depth_luma, shift_c = 8 - sps.pcm_bit_depth_chroma;
  Picture* p = cur_;
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) p->plane[0][static_cast<uint64_t>(y0 + y) * p->stride[0] + x0 + x] = static_cast<uint8_t>(br_->u(sps.pcm_bit_depth_luma) << shift_y);
  for (int c = 1; c < 3; ++c)
    for (int y = 0; y < n / 2; ++y)
      for (int x = 0; x < n / 2; ++x) p->plane[c][static_cast<uint64_t>(y0 / 2 + y) * p->stride[c] + x0 / 2 + x] = static_cast<uint8_t>(br_->u(sps.pcm_bit_depth_chroma) << shift_c);
  cabac_.init_engine();
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Transform tree and transform unit

void Core::transform_tree(int x0, int y0, int x_base, int y_base, int log2_size, int depth, int blk_idx, bool cbf_cb_parent[2], bool cbf_cr_parent[2], int inter_split) {
  const Sps& sps = *sp_;
  const bool intra_split = cur_intra_ && cur_part_mode_ == kPartNxN;
  bool split;
  if (log2_size <= sps.log2_max_tb && log2_size > sps.log2_min_tb && depth < max_trafo_depth_ && !(intra_split && depth == 0))
    split = cabac_.decision(kCtxSplitTransform + 5 - log2_size) != 0;
  else
    split = log2_size > sps.log2_max_tb || (intra_split && depth == 0) || (inter_split && depth == 0);
  bool cbf_cb[2] = {false, false}, cbf_cr[2] = {false, false};
  if (log2_size > 2) {
    if (depth == 0 || cbf_cb_parent[0]) cbf_cb[0] = cabac_.decision(kCtxCbfChroma + depth) != 0;
    if (depth == 0 || cbf_cr_parent[0]) cbf_cr[0] = cabac_.decision(kCtxCbfChroma + depth) != 0;
  } else if (depth > 0) {
    cbf_cb[0] = cbf_cb_parent[0];   // 4x4 luma blocks share the chroma blocks of their parent
    cbf_cr[0] = cbf_cr_parent[0];
  }
  if (split) {
    const int half = 1 << (log2_size - 1);
    transform_tree(x0, y0, x0, y0, log2_size - 1, depth + 1, 0, cbf_cb, cbf_cr, inter_split);
    transform_tree(x0 + half, y0, x0, y0, log2_size - 1, depth + 1, 1, cbf_cb, cbf_cr, inter_split);
    transform_tree(x0, y0 + half, x0, y0, log2_size - 1, depth + 1, 2, cbf_cb, cbf_cr, inter_split);
    transform_tree(x0 + half, y0 + half, x0, y0, log2_size - 1, depth + 1, 3, cbf_cb, cbf_cr, inter_split);
  } else {
    bool cbf_luma = true;
    if (cur_intra_ || depth != 0 || cbf_cb[0] || cbf_cr[0]) cbf_luma = cabac_.decision(kCtxCbfLuma + (depth == 0 ? 1 : 0)) != 0;
    transform_unit(x0, y0, x_base, y_base, log2_size, depth, blk_idx, cbf_luma, cbf_cb, cbf_cr);
  }
}

void Core::transform_unit(int x0, int y0, int x_base, int y_base, int log2_size, int depth, int blk_idx, bool cbf_luma, bool cbf_cb[2], bool cbf_cr[2]) {
  const Pps& pps = *pp_;
  const int log2_c = imax(2, log2_size - 1);
  const bool chroma_here = log2_size > 2;
  const bool chroma_blk = chroma_here || blk_idx == 3;   // chroma of four 4x4 luma blocks is handled with the last
  const bool cbf_chroma = cbf_cb[0] || cbf_cr[0];
  (void)depth;
  // The edges of the transform block are edges for the loop filter.
  for (int i = 0; i < (1 << log2_size) >> 2; ++i) {
    edge_[static_cast<uint64_t>((y0 >> 2) + i) * w4_ + (x0 >> 2)] |= 1;
    edge_[static_cast<uint64_t>(y0 >> 2) * w4_ + (x0 >> 2) + i] |= 4;
  }
  if ((cbf_luma || cbf_chroma) && pps.cu_qp_delta_enabled && !is_cu_qp_delta_coded_) {
    int prefix = 0;
    while (prefix < 5 && cabac_.decision(kCtxCuQpDelta + (prefix == 0 ? 0 : 1))) ++prefix;
    int value = prefix;
    if (prefix >= 5) {
      int k = 0;
      while (k < 7 && cabac_.bypass()) { value += 1 << k; ++k; }
      while (k--) value += cabac_.bypass() << k;
    }
    int delta = value;
    if (value > 0 && cabac_.bypass()) delta = -value;
    is_cu_qp_delta_coded_ = true;
    cu_qp_delta_val_ = delta;
    update_qp(cur_cb_x_, cur_cb_y_);
  }
  const int half_cb = (1 << cur_cb_log2_) >> 1;
  const int mode_idx = cur_intra_ && cur_part_mode_ == kPartNxN ? ((y0 - cur_cb_y_ >= half_cb ? 2 : 0) + (x0 - cur_cb_x_ >= half_cb ? 1 : 0)) : 0;
  // luma
  if (cur_intra_) predict_intra(x0, y0, log2_size, 0, intra_luma_modes_[mode_idx]);
  if (cbf_luma) {
    residual_coding(x0, y0, log2_size, 0);
    reconstruct_block(x0, y0, log2_size, 0);
    for (int y = y0 >> 2; y < (y0 + (1 << log2_size)) >> 2; ++y)
      for (int x = x0 >> 2; x < (x0 + (1 << log2_size)) >> 2; ++x) nz_[static_cast<uint64_t>(y) * w4_ + x] = 1;
  }
  // chroma
  if (chroma_blk) {
    const int xc = chroma_here ? x0 / 2 : x_base / 2, yc = chroma_here ? y0 / 2 : y_base / 2;
    const int lc = chroma_here ? log2_c : 2;
    for (int c = 1; c < 3; ++c) {
      if (cur_intra_) predict_intra(xc, yc, lc, c, intra_chroma_mode_[0]);
      const bool cbf = c == 1 ? cbf_cb[0] : cbf_cr[0];
      if (cbf) {
        residual_coding(xc, yc, lc, c);
        reconstruct_block(xc, yc, lc, c);
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Prediction units and motion vectors

void Core::store_motion(int x0, int y0, int w, int h, const MvField& m) {
  for (int y = y0 >> 2; y < (y0 + h) >> 2; ++y)
    for (int x = x0 >> 2; x < (x0 + w) >> 2; ++x) mvf_[static_cast<uint64_t>(y) * w4_ + x] = m;
}

namespace {
bool same_motion(const MvField& a, const MvField& b) {
  if (a.pred_flags != b.pred_flags) return false;
  for (int l = 0; l < 2; ++l) {
    if (!(a.pred_flags & (1 << l))) continue;
    if (a.ref_idx[l] != b.ref_idx[l] || a.mv[l][0] != b.mv[l][0] || a.mv[l][1] != b.mv[l][1]) return false;
  }
  return true;
}

int16_t wrap16(int v) { return static_cast<int16_t>(static_cast<uint16_t>(v & 0xFFFF)); }

// 8-179 .. 8-182: scaling a motion vector by the ratio of two picture distances
void scale_mv(int td, int tb, const int16_t in[2], int out[2]) {
  td = clip3(-128, 127, td);
  tb = clip3(-128, 127, tb);
  const int tx = (16384 + iabs(td) / 2) / td;
  const int dist = clip3(-4096, 4095, (tb * tx + 32) >> 6);
  for (int i = 0; i < 2; ++i) {
    const int p = dist * in[i];
    out[i] = clip3(-32768, 32767, (p < 0 ? -1 : 1) * ((iabs(p) + 127) >> 8));
  }
}
}  // namespace

// 8.5.3.2.8 / 8.5.3.2.9
bool Core::col_mv(const Picture* col, int xcol, int ycol, int ref_idx, int list, int mv[2]) {
  const Sps& s = *sp_;
  const ColBlock& b = col->col[static_cast<uint64_t>(ycol >> 4) * ((s.width + 15) / 16) + (xcol >> 4)];
  if (b.pred_flags == 0) return false;
  int list_col;
  if (!(b.pred_flags & 1)) list_col = 1;
  else if (!(b.pred_flags & 2)) list_col = 0;
  else list_col = no_backward_pred_ ? list : (sh_.collocated_from_l0 ? 1 : 0);
#ifdef H265_TRACE_PU
  fprintf(stderr, "    col poc %d at %d,%d: pred %d list_col %d mv (%d,%d) refpoc %d\n", col->poc, xcol, ycol, b.pred_flags, list_col, b.mv[list_col][0], b.mv[list_col][1], b.ref_poc[list_col]);
#endif
  const bool cur_long = slice_->ref_long[list][ref_idx];
  const bool col_long = (b.long_term >> list_col) & 1;
  if (cur_long != col_long) return false;
  const int col_diff = col->poc - b.ref_poc[list_col];
  const int cur_diff = poc_ - slice_->ref_poc[list][ref_idx];
  if (cur_long || col_diff == cur_diff || col_diff == 0) {
    mv[0] = b.mv[list_col][0];
    mv[1] = b.mv[list_col][1];
  } else {
    scale_mv(col_diff, cur_diff, b.mv[list_col], mv);
  }
  return true;
}

bool Core::temporal_mv(int x_pb, int y_pb, int w, int h, int ref_idx, int list, int mv[2]) {
  const Sps& s = *sp_;
  if (!sh_.temporal_mvp) return false;
  const Picture* col = slice_->ref_pic[sh_.type == kSliceB && !sh_.collocated_from_l0 ? 1 : 0][sh_.collocated_ref_idx];
  if (!col || col->col.size() == 0 || col->width != s.width || col->height != s.height) return false;
  const int xbr = x_pb + w, ybr = y_pb + h;
  if ((y_pb >> s.log2_ctb) == (ybr >> s.log2_ctb) && ybr < s.height && xbr < s.width) {
    if (col_mv(col, (xbr >> 4) << 4, (ybr >> 4) << 4, ref_idx, list, mv)) return true;
  }
  const int xc = x_pb + (w >> 1), yc = y_pb + (h >> 1);
  return col_mv(col, (xc >> 4) << 4, (yc >> 4) << 4, ref_idx, list, mv);
}

// 8.5.3.2.2 - 8.5.3.2.5
void Core::merge_motion(int x_cb, int y_cb, int n_cb, int x_pb, int y_pb, int w, int h, int part_idx, int merge_idx, MvField* out) {
  const int orig_w = w, orig_h = h;
  int part_mode = cur_part_mode_;
  const int par = pp_->log2_parallel_merge_level;
  if (par > 2 && n_cb == 8) {
    x_pb = x_cb; y_pb = y_cb; w = h = n_cb; part_idx = 0;
    part_mode = kPart2Nx2N;
  }
  MvField cands[6];
  int count = 0;
  const int max_cand = sh_.max_num_merge_cand;
  auto at = [&](int x, int y) -> const MvField& { return mvf_[static_cast<uint64_t>(y >> 2) * w4_ + (x >> 2)]; };
  auto same_region = [&](int xn, int yn) { return (x_pb >> par) == (xn >> par) && (y_pb >> par) == (yn >> par); };
  // A neighbour is "available" (eligible for comparison) when the rules of 6.4.2 and the parallel merge level allow it; whether it is also added to the list depends on
  // the comparisons with the neighbours before it, which use eligibility, not whether those were added.
  bool el_a1 = false, el_b1 = false, el_b0 = false, el_a0 = false, el_b2 = false;
  MvField a1{}, b1{}, b0{}, a0{}, b2{};
  {
    const int xn = x_pb - 1, yn = y_pb + h - 1;
    el_a1 = !same_region(xn, yn) && !((part_mode == kPartNx2N || part_mode == kPartnLx2N || part_mode == kPartnRx2N) && part_idx == 1) &&
            available_pb(x_cb, y_cb, n_cb, x_pb, y_pb, w, h, part_idx, xn, yn);
    if (el_a1) a1 = at(xn, yn);
  }
  {
    const int xn = x_pb + w - 1, yn = y_pb - 1;
    el_b1 = !same_region(xn, yn) && !((part_mode == kPart2NxN || part_mode == kPart2NxnU || part_mode == kPart2NxnD) && part_idx == 1) &&
            available_pb(x_cb, y_cb, n_cb, x_pb, y_pb, w, h, part_idx, xn, yn);
    if (el_b1) b1 = at(xn, yn);
  }
  {
    const int xn = x_pb + w, yn = y_pb - 1;
    el_b0 = !same_region(xn, yn) && available_pb(x_cb, y_cb, n_cb, x_pb, y_pb, w, h, part_idx, xn, yn);
    if (el_b0) b0 = at(xn, yn);
  }
  {
    const int xn = x_pb - 1, yn = y_pb + h;
    el_a0 = !same_region(xn, yn) && available_pb(x_cb, y_cb, n_cb, x_pb, y_pb, w, h, part_idx, xn, yn);
    if (el_a0) a0 = at(xn, yn);
  }
  {
    const int xn = x_pb - 1, yn = y_pb - 1;
    el_b2 = !same_region(xn, yn) && available_pb(x_cb, y_cb, n_cb, x_pb, y_pb, w, h, part_idx, xn, yn);
    if (el_b2) b2 = at(xn, yn);
  }
  const bool av_a1 = el_a1;
  const bool av_b1 = el_b1 && !(el_a1 && same_motion(a1, b1));
  const bool av_b0 = el_b0 && !(el_b1 && same_motion(b1, b0));
  const bool av_a0 = el_a0 && !(el_a1 && same_motion(a1, a0));
  const bool av_b2 = el_b2 && !(el_a1 && same_motion(a1, b2)) && !(el_b1 && same_motion(b1, b2)) && !(av_a0 && av_b0 && av_a1 && av_b1);
  if (av_a1) cands[count++] = a1;
  if (av_b1) cands[count++] = b1;
  if (av_b0) cands[count++] = b0;
  if (av_a0) cands[count++] = a0;
  if (av_b2) cands[count++] = b2;
  if (count > max_cand) count = max_cand;
  // temporal candidate
  if (count < max_cand && sh_.temporal_mvp) {
    MvField t{};
    t.ref_idx[0] = t.ref_idx[1] = -1;
    int mv[2];
    if (temporal_mv(x_pb, y_pb, w, h, 0, 0, mv)) {
      t.pred_flags |= 1;
      t.ref_idx[0] = 0;
      t.mv[0][0] = static_cast<int16_t>(mv[0]);
      t.mv[0][1] = static_cast<int16_t>(mv[1]);
    }
    if (sh_.type == kSliceB && temporal_mv(x_pb, y_pb, w, h, 0, 1, mv)) {
      t.pred_flags |= 2;
      t.ref_idx[1] = 0;
      t.mv[1][0] = static_cast<int16_t>(mv[0]);
      t.mv[1][1] = static_cast<int16_t>(mv[1]);
    }
    if (t.pred_flags) cands[count++] = t;
  }
  // combined bi-predictive candidates
  if (sh_.type == kSliceB && count > 1 && count < max_cand) {
    static const int kL0[12] = {0, 1, 0, 2, 1, 2, 0, 3, 1, 3, 2, 3};
    static const int kL1[12] = {1, 0, 2, 0, 2, 1, 3, 0, 3, 1, 3, 2};
    const int orig = count;
    for (int comb = 0; comb < 12; ++comb) {
      const MvField& c0 = cands[kL0[comb]];
      const MvField& c1 = cands[kL1[comb]];
      if ((c0.pred_flags & 1) && (c1.pred_flags & 2) &&
          (slice_->ref_poc[0][c0.ref_idx[0]] != slice_->ref_poc[1][c1.ref_idx[1]] || c0.mv[0][0] != c1.mv[1][0] || c0.mv[0][1] != c1.mv[1][1])) {
        MvField m{};
        m.pred_flags = 3;
        m.ref_idx[0] = c0.ref_idx[0];
        m.ref_idx[1] = c1.ref_idx[1];
        m.mv[0][0] = c0.mv[0][0]; m.mv[0][1] = c0.mv[0][1];
        m.mv[1][0] = c1.mv[1][0]; m.mv[1][1] = c1.mv[1][1];
        cands[count++] = m;
      }
      if (comb + 1 == orig * (orig - 1) || count == max_cand) break;
    }
  }
  // zero candidates
  const int num_ref = sh_.type == kSliceP ? sh_.num_ref_idx[0] : imin(sh_.num_ref_idx[0], sh_.num_ref_idx[1]);
  for (int zero = 0; count < max_cand; ++zero) {
    MvField m{};
    const int ref = zero < num_ref ? zero : 0;
    m.ref_idx[0] = static_cast<int8_t>(ref);
    m.ref_idx[1] = sh_.type == kSliceB ? static_cast<int8_t>(ref) : static_cast<int8_t>(-1);
    m.pred_flags = sh_.type == kSliceB ? 3 : 1;
    cands[count++] = m;
  }
#ifdef H265_TRACE_PU
  fprintf(stderr, "  merge cands (idx %d, spatial A1=%d B1=%d B0=%d A0=%d B2=%d):", merge_idx, av_a1, av_b1, av_b0, av_a0, av_b2);
  for (int i = 0; i < count; ++i) fprintf(stderr, " [%d p%d r%d,%d (%d,%d)(%d,%d)]", i, cands[i].pred_flags, cands[i].ref_idx[0], cands[i].ref_idx[1], cands[i].mv[0][0], cands[i].mv[0][1], cands[i].mv[1][0], cands[i].mv[1][1]);
  fprintf(stderr, "\n");
#endif
  *out = cands[merge_idx < count ? merge_idx : 0];
  if ((out->pred_flags & 3) == 3 && orig_w + orig_h == 12) {
    out->pred_flags = 1;
    out->ref_idx[1] = -1;
  }
  for (int l = 0; l < 2; ++l)
    if (!(out->pred_flags & (1 << l))) {
      out->ref_idx[l] = -1;
      out->mv[l][0] = out->mv[l][1] = 0;
    }
}

// 8.5.3.2.6 / 8.5.3.2.7
void Core::amvp(int x_cb, int y_cb, int n_cb, int x_pb, int y_pb, int w, int h, int part_idx, int list, int ref_idx, int mvp_flag, int mv_out[2]) {
  const Picture* target = slice_->ref_pic[list][ref_idx];
  const int target_poc = slice_->ref_poc[list][ref_idx];
  const bool target_long = slice_->ref_long[list][ref_idx];
  auto at = [&](int x, int y) -> const MvField& { return mvf_[static_cast<uint64_t>(y >> 2) * w4_ + (x >> 2)]; };
  const int xa0 = x_pb - 1, ya0 = y_pb + h, xa1 = x_pb - 1, ya1 = y_pb + h - 1;
  const int xb0 = x_pb + w, yb0 = y_pb - 1, xb1 = x_pb + w - 1, yb1 = y_pb - 1, xb2 = x_pb - 1, yb2 = y_pb - 1;
  const bool av_a0 = available_pb(x_cb, y_cb, n_cb, x_pb, y_pb, w, h, part_idx, xa0, ya0);
  const bool av_a1 = available_pb(x_cb, y_cb, n_cb, x_pb, y_pb, w, h, part_idx, xa1, ya1);
  const bool is_scaled = av_a0 || av_a1;
  auto exact = [&](const MvField& n, int mv[2]) {
    for (int k = 0; k < 2; ++k) {
      const int l = k == 0 ? list : 1 - list;
      if ((n.pred_flags & (1 << l)) && slice_->ref_pic[l][n.ref_idx[l]] == target) {
        mv[0] = n.mv[l][0];
        mv[1] = n.mv[l][1];
        return true;
      }
    }
    return false;
  };
  auto scaled = [&](const MvField& n, int mv[2]) {
    for (int k = 0; k < 2; ++k) {
      const int l = k == 0 ? list : 1 - list;
      if ((n.pred_flags & (1 << l)) && slice_->ref_long[l][n.ref_idx[l]] == target_long) {
        const int ref_poc_n = slice_->ref_poc[l][n.ref_idx[l]];
        if (!target_long && !slice_->ref_long[l][n.ref_idx[l]]) {
          scale_mv(poc_ - ref_poc_n, poc_ - target_poc, n.mv[l], mv);
        } else {
          mv[0] = n.mv[l][0];
          mv[1] = n.mv[l][1];
        }
        return true;
      }
    }
    return false;
  };
  int mv_a[2] = {0, 0}, mv_b[2] = {0, 0};
  bool flag_a = false, flag_b = false;
  const int pa_x[2] = {xa0, xa1}, pa_y[2] = {ya0, ya1};
  const bool pa_av[2] = {av_a0, av_a1};
  for (int k = 0; k < 2 && !flag_a; ++k)
    if (pa_av[k]) flag_a = exact(at(pa_x[k], pa_y[k]), mv_a);
  for (int k = 0; k < 2 && !flag_a; ++k)
    if (pa_av[k]) flag_a = scaled(at(pa_x[k], pa_y[k]), mv_a);
  const int pb_x[3] = {xb0, xb1, xb2}, pb_y[3] = {yb0, yb1, yb2};
  bool pb_av[3];
  for (int k = 0; k < 3; ++k) pb_av[k] = available_pb(x_cb, y_cb, n_cb, x_pb, y_pb, w, h, part_idx, pb_x[k], pb_y[k]);
  for (int k = 0; k < 3 && !flag_b; ++k)
    if (pb_av[k]) flag_b = exact(at(pb_x[k], pb_y[k]), mv_b);
  if (!is_scaled && flag_b) {
    mv_a[0] = mv_b[0];
    mv_a[1] = mv_b[1];
    flag_a = true;
  }
  if (!is_scaled) {
    flag_b = false;
    for (int k = 0; k < 3 && !flag_b; ++k)
      if (pb_av[k]) flag_b = scaled(at(pb_x[k], pb_y[k]), mv_b);
  }
  int cand[3][2];
  int n = 0;
  if (flag_a) { cand[n][0] = mv_a[0]; cand[n][1] = mv_a[1]; ++n; }
  if (flag_b && !(flag_a && mv_a[0] == mv_b[0] && mv_a[1] == mv_b[1])) { cand[n][0] = mv_b[0]; cand[n][1] = mv_b[1]; ++n; }
  if (n < 2) {
    int mv_c[2];
    if (temporal_mv(x_pb, y_pb, w, h, ref_idx, list, mv_c)) { cand[n][0] = mv_c[0]; cand[n][1] = mv_c[1]; ++n; }
  }
  while (n < 2) { cand[n][0] = cand[n][1] = 0; ++n; }
  mv_out[0] = cand[mvp_flag][0];
  mv_out[1] = cand[mvp_flag][1];
}

void Core::prediction_unit(int x0, int y0, int w, int h, int part_idx, int log2_cb, int x_cb, int y_cb) {
  const int n_cb = 1 << log2_cb;
  MvField m{};
  m.ref_idx[0] = m.ref_idx[1] = -1;
  bool merge = cu_skip_;
  if (!merge) merge = cabac_.decision(kCtxMergeFlag) != 0;
  if (part_idx == 0) merge_flag_cu_ = merge;
  if (merge) {
    int idx = 0;
    if (sh_.max_num_merge_cand > 1) {
      if (cabac_.decision(kCtxMergeIdx)) {
        idx = 1;
        while (idx < sh_.max_num_merge_cand - 1 && cabac_.bypass()) ++idx;
      }
    }
    merge_motion(x_cb, y_cb, n_cb, x0, y0, w, h, part_idx, idx, &m);
  } else {
    int pred = kPredL0;
    if (sh_.type == kSliceB) {
      if (w + h == 12) {
        pred = cabac_.decision(kCtxInterPredIdc + 4) ? kPredL1 : kPredL0;
      } else {
        const int depth = ct_depth_[static_cast<uint64_t>(y0 >> 2) * w4_ + (x0 >> 2)];
        if (cabac_.decision(kCtxInterPredIdc + depth)) pred = kPredBi;
        else pred = cabac_.decision(kCtxInterPredIdc + 4) ? kPredL1 : kPredL0;
      }
    }
    int mvd[2][2] = {{0, 0}, {0, 0}}, mvp[2] = {0, 0}, ref[2] = {-1, -1};
    for (int l = 0; l < 2; ++l) {
      if (!(l == 0 ? pred != kPredL1 : pred != kPredL0)) continue;
      const int num = sh_.num_ref_idx[l];
      int r = 0;
      if (num > 1) {
        const int max = num - 1;
        while (r < imin(max, 2) && cabac_.decision(kCtxRefIdx + r)) ++r;
        if (r == 2) while (r < max && cabac_.bypass()) ++r;
      }
      ref[l] = r;
      if (l == 1 && sh_.mvd_l1_zero && pred == kPredBi) {
        mvd[1][0] = mvd[1][1] = 0;
      } else {
        const int g0x = cabac_.decision(kCtxMvdGreater0), g0y = cabac_.decision(kCtxMvdGreater0);
        const int g1x = g0x ? cabac_.decision(kCtxMvdGreater1) : 0, g1y = g0y ? cabac_.decision(kCtxMvdGreater1) : 0;
        int comp[2] = {0, 0};
        const int g0[2] = {g0x, g0y}, g1[2] = {g1x, g1y};
        for (int c = 0; c < 2; ++c) {
          if (!g0[c]) continue;
          int a = 1;
          if (g1[c]) {
            a = 2;
            int k = 1;
            while (k < 31 && cabac_.bypass()) { a += 1 << k; ++k; }
            while (k--) a += cabac_.bypass() << k;
          }
          comp[c] = cabac_.bypass() ? -a : a;
        }
        mvd[l][0] = comp[0];
        mvd[l][1] = comp[1];
      }
      mvp[l] = cabac_.decision(kCtxMvpFlag);
    }
    for (int l = 0; l < 2; ++l) {
      if (ref[l] < 0) continue;
      int pmv[2];
      amvp(x_cb, y_cb, n_cb, x0, y0, w, h, part_idx, l, ref[l], mvp[l], pmv);
      m.pred_flags = static_cast<uint8_t>(m.pred_flags | (1 << l));
      m.ref_idx[l] = static_cast<int8_t>(ref[l]);
      m.mv[l][0] = wrap16(pmv[0] + mvd[l][0]);
      m.mv[l][1] = wrap16(pmv[1] + mvd[l][1]);
    }
  }
#ifdef H265_TRACE_PU
  fprintf(stderr, "poc %d PU %d,%d %dx%d merge=%d pred=%d ref=%d,%d mv0=%d,%d mv1=%d,%d\n", poc_, x0, y0, w, h, merge, m.pred_flags, m.ref_idx[0], m.ref_idx[1], m.mv[0][0], m.mv[0][1], m.mv[1][0], m.mv[1][1]);
#endif
  store_motion(x0, y0, w, h, m);
  // the prediction block's left and top edges are edges for the loop filter
  for (int i = 0; i < h >> 2; ++i) edge_[static_cast<uint64_t>((y0 >> 2) + i) * w4_ + (x0 >> 2)] |= 2;
  for (int i = 0; i < w >> 2; ++i) edge_[static_cast<uint64_t>(y0 >> 2) * w4_ + (x0 >> 2) + i] |= 8;
  predict_inter(x0, y0, w, h, m);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// Residual coding (7.3.8.11)

void Core::residual_coding(int x0, int y0, int log2_size, int c_idx) {
  const Pps& pps = *pp_;
  const int n = 1 << log2_size;
  for (int i = 0; i < n * n; ++i) coeff_[i] = 0;
  ts_flag_ = false;
  if (pps.transform_skip && !cu_transquant_bypass_ && log2_size <= 2) ts_flag_ = cabac_.decision(kCtxTransformSkip + (c_idx ? 1 : 0)) != 0;

  // last significant coefficient
  int ctx_offset, ctx_shift;
  if (c_idx == 0) {
    ctx_offset = 3 * (log2_size - 2) + ((log2_size - 1) >> 2);
    ctx_shift = (log2_size + 1) >> 2;
  } else {
    ctx_offset = 15;
    ctx_shift = log2_size - 2;
  }
  const int max_prefix = (log2_size << 1) - 1;
  int px = 0, py = 0;
  while (px < max_prefix && cabac_.decision(kCtxLastX + ctx_offset + (px >> ctx_shift))) ++px;
  while (py < max_prefix && cabac_.decision(kCtxLastY + ctx_offset + (py >> ctx_shift))) ++py;
  int last_x = px, last_y = py;
  if (px > 3) last_x = (1 << ((px >> 1) - 1)) * (2 + (px & 1)) + cabac_.bypass_bits((px >> 1) - 1);
  if (py > 3) last_y = (1 << ((py >> 1) - 1)) * (2 + (py & 1)) + cabac_.bypass_bits((py >> 1) - 1);

  // scan order (7.4.9.11)
  int scan_idx = 0;
  if (cur_intra_ && (log2_size == 2 || (log2_size == 3 && c_idx == 0))) {
    const int mode = c_idx == 0 ? intra_mode_[static_cast<uint64_t>(y0 >> 2) * w4_ + (x0 >> 2)] : intra_chroma_mode_[0];
    if (mode >= 6 && mode <= 14) scan_idx = 2;
    else if (mode >= 22 && mode <= 30) scan_idx = 1;
  }
  if (scan_idx == 2) { const int t = last_x; last_x = last_y; last_y = t; }
  if (last_x >= n || last_y >= n) return;   // (damaged stream)

  const uint8_t* sbx = kScan.x[scan_idx][log2_size - 2];
  const uint8_t* sby = kScan.y[scan_idx][log2_size - 2];
  const uint8_t* px4 = kScan.x[scan_idx][2];
  const uint8_t* py4 = kScan.y[scan_idx][2];
  const int sb_count = 1 << ((log2_size - 2) * 2);
  int last_sub = 0, last_pos = 0;
  for (int i = 0; i < sb_count; ++i)
    if (sbx[i] == (last_x >> 2) && sby[i] == (last_y >> 2)) { last_sub = i; break; }
  for (int i = 0; i < 16; ++i)
    if (px4[i] == (last_x & 3) && py4[i] == (last_y & 3)) { last_pos = i; break; }

  uint8_t csbf[8][8] = {};
  const int sb_w = 1 << (log2_size - 2);
  int c1 = 1;   // greater1Ctx carried between sub-blocks
  bool first_subblock_done = false;
  (void)first_subblock_done;
  for (int i = last_sub; i >= 0; --i) {
    const int xs = sbx[i], ys = sby[i];
    bool infer_dc = false;
    bool coded = true;
    int csbf_ctx = 0;
    if (xs < sb_w - 1) csbf_ctx |= csbf[ys][xs + 1];
    if (ys < sb_w - 1) csbf_ctx |= csbf[ys + 1][xs];
    if (i < last_sub && i > 0) {
      coded = cabac_.decision(kCtxCodedSubBlock + (c_idx ? 2 : 0) + csbf_ctx) != 0;
      infer_dc = true;
    }
    csbf[ys][xs] = coded ? 1 : 0;
    if (!coded) continue;
    const int prev_csbf = (xs < sb_w - 1 ? csbf[ys][xs + 1] : 0) | ((ys < sb_w - 1 ? csbf[ys + 1][xs] : 0) << 1);
    uint8_t sig[16] = {};
    int num_sig = 0;
    int start = i == last_sub ? last_pos - 1 : 15;
    if (i == last_sub) { sig[last_pos] = 1; ++num_sig; }
    for (int p = start; p >= 0; --p) {
      const int xp = px4[p], yp = py4[p];
      if (p > 0 || !infer_dc) {
        // sig_coeff_flag context (9.3.4.2.5)
        int sig_ctx;
        const int xc = (xs << 2) + xp, yc = (ys << 2) + yp;
        if (log2_size == 2) {
          sig_ctx = kCtxIdxMap4x4[(yc << 2) + xc];
        } else if (xc + yc == 0) {
          sig_ctx = 0;
        } else {
          if (prev_csbf == 0) sig_ctx = (xp + yp == 0) ? 2 : (xp + yp < 3) ? 1 : 0;
          else if (prev_csbf == 1) sig_ctx = yp == 0 ? 2 : yp == 1 ? 1 : 0;
          else if (prev_csbf == 2) sig_ctx = xp == 0 ? 2 : xp == 1 ? 1 : 0;
          else sig_ctx = 2;
          if (c_idx == 0) {
            if (xs > 0 || ys > 0) sig_ctx += 3;
            sig_ctx += log2_size == 3 ? (scan_idx == 0 ? 9 : 15) : 21;
          } else {
            sig_ctx += log2_size == 3 ? 9 : 12;
          }
        }
        if (c_idx > 0) sig_ctx += 27;
        sig[p] = static_cast<uint8_t>(cabac_.decision(kCtxSigCoeff + sig_ctx));
        if (sig[p]) { ++num_sig; infer_dc = false; }
      } else {
        sig[0] = 1;   // inferred: a coded sub-block has at least one coefficient
        ++num_sig;
      }
    }
    if (num_sig == 0) continue;
    // levels
    uint8_t greater1[16] = {}, greater2[16] = {};
    int first_sig = 16, last_sig = -1, num_g1 = 0, last_g1_pos = -1;
    int ctx_set = (i == 0 || c_idx > 0) ? 0 : 2;
    if (c1 == 0) ++ctx_set;
    c1 = 1;
    for (int p = 15; p >= 0; --p) {
      if (!sig[p]) continue;
      if (num_g1 < 8) {
        const int ctx = ctx_set * 4 + c1 + (c_idx > 0 ? 16 : 0);
        greater1[p] = static_cast<uint8_t>(cabac_.decision(kCtxGreater1 + ctx));
        ++num_g1;
        if (greater1[p]) {
          c1 = 0;
          if (last_g1_pos == -1) last_g1_pos = p;
        } else if (c1 > 0 && c1 < 3) {
          ++c1;
        }
      }
      if (last_sig == -1) last_sig = p;
      first_sig = p;
    }
    const bool sign_hidden = pps.sign_data_hiding && !cu_transquant_bypass_ && (last_sig - first_sig > 3);
    if (last_g1_pos != -1) greater2[last_g1_pos] = static_cast<uint8_t>(cabac_.decision(kCtxGreater2 + ctx_set + (c_idx > 0 ? 4 : 0)));
    uint8_t sign[16] = {};
    for (int p = 15; p >= 0; --p)
      if (sig[p] && (!sign_hidden || p != first_sig)) sign[p] = static_cast<uint8_t>(cabac_.bypass());
    int num_sig_coeff = 0, sum_abs = 0, rice = 0;
    for (int p = 15; p >= 0; --p) {
      if (!sig[p]) continue;
      const int base = 1 + greater1[p] + greater2[p];
      int level = base;
      if (base == ((num_sig_coeff < 8) ? ((p == last_g1_pos) ? 3 : 2) : 1)) {
        int prefix = 0;
        while (prefix < 32 && cabac_.bypass()) ++prefix;
        int rem;
        if (prefix <= 3) rem = (prefix << rice) + cabac_.bypass_bits(rice);
        else rem = (((1 << (prefix - 3)) + 3 - 1) << rice) + cabac_.bypass_bits(prefix - 3 + rice);
        level = base + rem;
        if (level > 3 * (1 << rice)) rice = imin(rice + 1, 4);
      }
      int value = sign[p] ? -level : level;
      if (sign_hidden) {
        sum_abs += level;
        if (p == first_sig && (sum_abs & 1)) value = -value;
      }
      ++num_sig_coeff;
      const int xc = (xs << 2) + px4[p], yc = (ys << 2) + py4[p];
      coeff_[yc * n + xc] = value;
    }
  }
}

}  // namespace h265
}  // namespace hardware
