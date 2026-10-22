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

// The VP9 decoder's internal state and the functions its parts share. See Vp9.hpp for what the decoder covers.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/Software/Vp9Data.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {
namespace vp9 {

// ---- readers ------------------------------------------------------------------------------------------------------------------

// The bit reader of the uncompressed header: most significant bit first. Reading past the end sets error().
class BitReader {
 public:
  explicit BitReader(ConstByteView data) : data_(data) {}
  int bit() {
    const uint64_t byte = pos_ >> 3;
    if (byte >= data_.size()) {
      error_ = true;
      return 0;
    }
    const int b = (data_[byte] >> (7 - (pos_ & 7))) & 1;
    ++pos_;
    return b;
  }
  int literal(int bits) {
    int v = 0;
    for (int i = 0; i < bits; ++i) v = (v << 1) | bit();
    return v;
  }
  int signed_literal(int bits) {
    const int value = literal(bits);
    return bit() ? -value : value;
  }
  uint64_t bytes_read() const { return (pos_ + 7) >> 3; }
  bool error() const { return error_; }

 private:
  ConstByteView data_;
  uint64_t pos_ = 0;
  bool error_ = false;
};

// The boolean decoder of the compressed header and the tiles (arithmetic coding with 8-bit probabilities).
class BoolDecoder {
 public:
  // False if the marker bit is not zero (or there is no data).
  bool init(ConstByteView data) {
    data_ = data;
    pos_ = 0;
    value_ = 0;
    count_ = -8;
    range_ = 255;
    if (data.empty()) return false;
    fill();
    return read(128) == 0;
  }
  int read(int prob) {
    const unsigned split = (range_ * static_cast<unsigned>(prob) + (256 - static_cast<unsigned>(prob))) >> 8;
    if (count_ < 0) fill();
    const uint64_t bigsplit = static_cast<uint64_t>(split) << 56;
    int bit = 0;
    unsigned range = split;
    uint64_t value = value_;
    if (value >= bigsplit) {
      range = range_ - split;
      value -= bigsplit;
      bit = 1;
    }
    const int shift = vpx_norm[range];
    range_ = range << shift;
    value_ = value << shift;
    count_ -= shift;
    return bit;
  }
  int bit() { return read(128); }
  int literal(int bits) {
    int v = 0;
    for (int i = bits - 1; i >= 0; --i) v |= bit() << i;
    return v;
  }
  int tree(const int8_t* tree, const uint8_t* probs) {
    int i = 0;
    while ((i = tree[i + read(probs[i >> 1])]) > 0) {}
    return -i;
  }
  // The data ran out before the decoder had what it needed (a damaged or truncated tile).
  bool error() const { return count_ > 64 && count_ < kLotsOfBits; }

 private:
  static constexpr int kLotsOfBits = 0x40000000;
  void fill() {
    int shift = 64 - 8 - (count_ + 8);
    while (shift >= 0) {
      if (pos_ >= data_.size()) {
        count_ += kLotsOfBits;
        break;
      }
      count_ += 8;
      value_ |= static_cast<uint64_t>(data_[pos_++]) << shift;
      shift -= 8;
    }
  }
  ConstByteView data_;
  uint64_t pos_ = 0, value_ = 0;
  int count_ = 0;
  unsigned range_ = 255;
};

// ---- probabilities and their counts -------------------------------------------------------------------------------------------

struct FrameContext {
  uint8_t y_mode_prob[BLOCK_SIZE_GROUPS][INTRA_MODES - 1];
  uint8_t uv_mode_prob[INTRA_MODES][INTRA_MODES - 1];
  uint8_t partition_prob[PARTITION_CONTEXTS][PARTITION_TYPES - 1];
  uint8_t coef_probs[TX_SIZES][PLANE_TYPES][REF_TYPES][COEF_BANDS][COEFF_CONTEXTS][UNCONSTRAINED_NODES];
  uint8_t switchable_interp_prob[SWITCHABLE_FILTER_CONTEXTS][SWITCHABLE_FILTERS - 1];
  uint8_t inter_mode_probs[INTER_MODE_CONTEXTS][INTER_MODES - 1];
  uint8_t intra_inter_prob[INTRA_INTER_CONTEXTS];
  uint8_t comp_inter_prob[COMP_INTER_CONTEXTS];
  uint8_t single_ref_prob[REF_CONTEXTS][2];
  uint8_t comp_ref_prob[REF_CONTEXTS];
  TxProbs tx_probs;
  uint8_t skip_probs[SKIP_CONTEXTS];
  NmvContext nmvc;
  bool initialized;
};

struct NmvComponentCounts {
  unsigned sign[2];
  unsigned classes[MV_CLASSES];
  unsigned class0[CLASS0_SIZE];
  unsigned bits[MV_OFFSET_BITS][2];
  unsigned class0_fp[CLASS0_SIZE][MV_FP_SIZE];
  unsigned fp[MV_FP_SIZE];
  unsigned class0_hp[2];
  unsigned hp[2];
};

struct FrameCounts {
  unsigned y_mode[BLOCK_SIZE_GROUPS][INTRA_MODES];
  unsigned uv_mode[INTRA_MODES][INTRA_MODES];
  unsigned partition[PARTITION_CONTEXTS][PARTITION_TYPES];
  unsigned coef[TX_SIZES][PLANE_TYPES][REF_TYPES][COEF_BANDS][COEFF_CONTEXTS][UNCONSTRAINED_NODES + 1];
  unsigned eob_branch[TX_SIZES][PLANE_TYPES][REF_TYPES][COEF_BANDS][COEFF_CONTEXTS];
  unsigned switchable_interp[SWITCHABLE_FILTER_CONTEXTS][SWITCHABLE_FILTERS];
  unsigned inter_mode[INTER_MODE_CONTEXTS][INTER_MODES];
  unsigned intra_inter[INTRA_INTER_CONTEXTS][2];
  unsigned comp_inter[COMP_INTER_CONTEXTS][2];
  unsigned single_ref[REF_CONTEXTS][2][2];
  unsigned comp_ref[REF_CONTEXTS][2];
  struct {
    unsigned p32x32[TX_SIZE_CONTEXTS][TX_SIZES];
    unsigned p16x16[TX_SIZE_CONTEXTS][TX_SIZES - 1];
    unsigned p8x8[TX_SIZE_CONTEXTS][TX_SIZES - 2];
  } tx;
  unsigned skip[SKIP_CONTEXTS][2];
  unsigned mv_joints[MV_JOINTS];
  NmvComponentCounts mv_comps[2];
};

// ---- blocks ----------------------------------------------------------------------------------------------------------------------

struct SubBlockInfo {
  uint8_t mode = 0;
  Mv mv[2];
};

// What is known about one block (the mode info of an 8x8 unit that starts it; the other units of the block point to it).
struct ModeInfo {
  uint8_t sb_type = 0, mode = 0, tx_size = 0, uv_mode = 0, interp_filter = 0;
  int8_t skip = 0, segment_id = 0, seg_id_predicted = 0;
  int8_t ref_frame[2] = {0, -1};
  Mv mv[2];
  SubBlockInfo bmi[4];
  bool is_inter() const { return ref_frame[0] > INTRA_FRAME; }
  bool has_second_ref() const { return ref_frame[1] > INTRA_FRAME; }
};

struct MvRef {
  int8_t ref_frame[2] = {0, 0};
  Mv mv[2];
};

struct Segmentation {
  bool enabled = false, update_map = false, update_data = false, abs_delta = false, temporal_update = false;
  uint8_t tree_probs[SEG_TREE_PROBS];
  uint8_t pred_probs[PREDICTION_PROBS];
  int16_t feature_data[MAX_SEGMENTS][SEG_LVL_MAX] = {};
  uint32_t feature_mask[MAX_SEGMENTS] = {};
  bool active(int segment, int feature) const { return enabled && (feature_mask[segment] >> feature & 1); }
};

struct LoopFilterParams {
  int filter_level = 0, sharpness_level = 0, last_sharpness_level = 0;
  bool mode_ref_delta_enabled = false, mode_ref_delta_update = false;
  int8_t ref_deltas[MAX_REF_FRAMES] = {};
  int8_t mode_deltas[2] = {};
};

// One decoded picture with everything later pictures want from it.
struct Frame {
  ByteView memory;  // not used: planes are 16-bit
  ArrayView<uint16_t> pixels;
  ArrayView<uint16_t> plane[3];
  uint32_t stride[3] = {0, 0, 0};
  uint32_t plane_rows[3] = {0, 0, 0};
  int width = 0, height = 0;                // luma, as coded
  int crop_w[3] = {0, 0, 0}, crop_h[3] = {0, 0, 0};
  int aligned_w[3] = {0, 0, 0}, aligned_h[3] = {0, 0, 0};  // rounded up to 8 luma pixels
  int ss_x = 1, ss_y = 1, bit_depth = 8;
  int color_space = 0;
  bool full_range = false;
  ArrayView<MvRef> mvs;
  int mi_rows = 0, mi_cols = 0;
  int ref_count = 0;
  bool corrupted = false;
};

struct ScaleFactors {
  int x_scale_fp = 0, y_scale_fp = 0, x_step_q4 = 16, y_step_q4 = 16;
  bool valid() const { return x_scale_fp != -1 && y_scale_fp != -1; }
  bool scaled() const { return valid() && (x_scale_fp != (1 << 14) || y_scale_fp != (1 << 14)); }
};

struct RefBuffer {
  int idx = -1;
  ScaleFactors sf;
};

struct TileInfo {
  int mi_row_start = 0, mi_row_end = 0, mi_col_start = 0, mi_col_end = 0;
};

// What a decoded frame's header says (for the caller and the tests).
struct FrameHeaderInfo {
  bool show_frame = false, show_existing_frame = false, key_frame = false, intra_only = false;
  int width = 0, height = 0, bit_depth = 8, profile = 0;
};

class Core {
 public:
  static constexpr int kPoolSize = 12;

  explicit Core(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Core();
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  // Decodes one frame (not a superframe). On success `*shown` is the frame to show, or -1 if this frame is not shown.
  bool decode_frame(ConstByteView data, int* shown, FrameHeaderInfo* info);
  const Frame& frame(int index) const { return frames_[index]; }
  void reset();

 private:
  friend struct Vp9Access;
  // --- Vp9Syntax.cpp: headers, probabilities, tiles, mode info, tokens
  bool read_uncompressed_header(BitReader& rb, uint32_t* header_size, int* shown, FrameHeaderInfo* info);
  bool read_color_config(BitReader& rb);
  bool setup_frame_size(BitReader& rb);
  bool setup_frame_size_with_refs(BitReader& rb);
  bool resize_context(int width, int height);
  bool allocate_frame(Frame* f, int width, int height);
  void release_frame(Frame* f);
  void setup_loopfilter(BitReader& rb);
  void setup_quantization(BitReader& rb);
  void setup_segmentation(BitReader& rb);
  void setup_segmentation_dequant();
  void setup_tile_info(BitReader& rb);
  void setup_past_independence();
  void setup_scale_factors(ScaleFactors* sf, int other_w, int other_h, int this_w, int this_h);
  bool read_compressed_header(ConstByteView data);
  void read_coef_probs(BoolDecoder& r);
  void diff_update_prob(BoolDecoder& r, uint8_t* p);
  bool decode_tiles(ConstByteView data);
  void decode_partition(BoolDecoder& r, int mi_row, int mi_col, int bsize, int n4x4_l2);
  void decode_block(BoolDecoder& r, int mi_row, int mi_col, int bsize, int bwl, int bhl);
  int read_partition(BoolDecoder& r, int mi_row, int mi_col, bool has_rows, bool has_cols, int bsl);
  void read_mode_info(BoolDecoder& r, int mi_row, int mi_col, int x_mis, int y_mis);
  void read_intra_frame_mode_info(BoolDecoder& r, int mi_row, int mi_col, int x_mis, int y_mis);
  void read_inter_frame_mode_info(BoolDecoder& r, int mi_row, int mi_col, int x_mis, int y_mis);
  void read_intra_block_mode_info(BoolDecoder& r);
  void read_inter_block_mode_info(BoolDecoder& r, int mi_row, int mi_col);
  int read_segment_id(BoolDecoder& r);
  int read_skip(BoolDecoder& r, int segment_id);
  int read_tx_size(BoolDecoder& r, bool allow_select);
  int read_intra_mode(BoolDecoder& r, const uint8_t* probs);
  int read_inter_mode(BoolDecoder& r, int ctx);
  void read_ref_frames(BoolDecoder& r, int segment_id, int8_t ref_frame[2]);
  Mv read_mv_diff(BoolDecoder& r, bool use_hp);
  int read_mv_component(BoolDecoder& r, const NmvComponent& comp, bool use_hp);
  void inc_mv(const Mv& diff);
  void find_mv_refs(int ref_frame, Mv* list, int mi_row, int mi_col, int block);
  void append_sub8x8_mvs(int block, int ref, int mi_row, int mi_col, int b_mode, Mv* best);
  const uint8_t* kf_y_probs(int block) const;
  int mode_context(int mi_row, int mi_col);
  int decode_block_tokens(BoolDecoder& r, int plane, int tx_size, int tx_type, int x, int y, int seg_id);
  void adapt_coef_probs();
  void adapt_mode_probs();
  void adapt_mv_probs();
  ModeInfo* mi_at(int row, int col) { return &mi_[static_cast<uint64_t>(grid_[static_cast<uint64_t>(row) * mi_cols_ + static_cast<uint64_t>(col)])]; }
  // --- Vp9Recon.cpp: prediction and reconstruction
    void build_inter_predictors(int mi_row, int mi_col);
  void predict_inter_block(int plane, int bw, int bh, int x, int y, int w, int h, const Mv& mv, const RefBuffer& ref_buf, int ref);
  void inverse_transform_add(int plane, int tx_size, int tx_type, int eob, uint64_t dst_index);
  void reconstruct_block(BoolDecoder& r, int mi_row, int mi_col, int bsize);
  void predict_intra_block(int plane, int tx_size, int mode, int aoff, int loff);
  // --- Vp9LoopFilter.cpp
  void loop_filter_frame();
  void filter_block_plane(int plane, int mi_row, int mi_col);
  void loop_filter_frame_init();

  struct PlaneInfo {
    int ss_x = 0, ss_y = 0;
    uint16_t n4_w = 0, n4_h = 0;
    uint8_t n4_wl = 0, n4_hl = 0;
  };

  allocators::IAllocator& allocator_;
  Frame frames_[kPoolSize];
  int ref_frame_map_[REF_FRAMES] = {-1, -1, -1, -1, -1, -1, -1, -1};
  int next_ref_frame_map_[REF_FRAMES] = {};
  int new_fb_idx_ = -1;
  RefBuffer frame_refs_[REFS_PER_FRAME];

  // frame header state (libvpx's VP9_COMMON)
  int profile_ = 0, frame_type_ = 0, last_frame_type_ = 0;
  bool show_frame_ = false, last_show_frame_ = false, error_resilient_ = false, intra_only_ = false, last_intra_only_ = false;
  int reset_frame_context_ = 0, refresh_frame_flags_ = 0;
  bool refresh_frame_context_ = false, frame_parallel_decoding_mode_ = false;
  int frame_context_idx_ = 0;
  int width_ = 0, height_ = 0, last_width_ = 0, last_height_ = 0;
  int ss_x_ = 1, ss_y_ = 1, bit_depth_ = 8, color_space_ = 0;
  bool full_range_ = false;
  bool allow_high_precision_mv_ = false;
  int interp_filter_ = 0;
  int ref_frame_sign_bias_[MAX_REF_FRAMES] = {};
  int base_qindex_ = 0, y_dc_delta_q_ = 0, uv_dc_delta_q_ = 0, uv_ac_delta_q_ = 0;
  bool lossless_ = false;
  int tx_mode_ = 0, reference_mode_ = 0;  // reference_mode: 0 single, 1 compound, 2 select
  int comp_fixed_ref_ = 0, comp_var_ref_[2] = {0, 0};
  int log2_tile_cols_ = 0, log2_tile_rows_ = 0;
  bool need_resync_ = true;
  bool use_prev_frame_mvs_ = false;
  int prev_frame_idx_ = -1;
  int current_video_frame_ = 0;
  Segmentation seg_;
  LoopFilterParams lf_;
  uint8_t lf_lvl_[MAX_SEGMENTS][MAX_REF_FRAMES][2] = {};
  uint8_t lf_lim_[MAX_LOOP_FILTER + 1] = {}, lf_mblim_[MAX_LOOP_FILTER + 1] = {};
  int16_t y_dequant_[MAX_SEGMENTS][2] = {}, uv_dequant_[MAX_SEGMENTS][2] = {};
  FrameContext fc_, frame_contexts_[FRAME_CONTEXTS];
  FrameCounts counts_;
  bool count_enabled_ = false;

  // per-frame buffers sized by the picture
  int mi_rows_ = 0, mi_cols_ = 0, sb_cols_aligned_ = 0;
  ArrayView<ModeInfo> mi_;
  ArrayView<int32_t> grid_;
  ArrayView<uint8_t> seg_map_[2];
  int seg_map_cur_ = 0;
  ArrayView<uint8_t> above_ctx_[3];
  ArrayView<int8_t> above_part_ctx_;
  ArrayView<uint8_t> lf_levels_;

  // tile / block state
  TileInfo tile_;
  uint8_t left_ctx_[3][16] = {};
  int8_t left_part_ctx_[8] = {};
  PlaneInfo plane_[3];
  ModeInfo* cur_ = nullptr;
  ModeInfo* above_mi_ = nullptr;
  ModeInfo* left_mi_ = nullptr;
  int cur_row_ = 0, cur_col_ = 0;
  int mb_to_left_edge_ = 0, mb_to_right_edge_ = 0, mb_to_top_edge_ = 0, mb_to_bottom_edge_ = 0;
  int max_blocks_wide_ = 0, max_blocks_high_ = 0;
  int a_off_[3] = {}, l_off_[3] = {};
  int bmode_blocks_wl_ = 0, bmode_blocks_hl_ = 0;
  bool corrupted_ = false;
  static constexpr int kPatchSize = 136;
  TranLow dqcoeff_[32 * 32] = {};
  uint16_t patch_[kPatchSize * kPatchSize];
  uint16_t temp_[kPatchSize * 64];
  Frame* cur_frame_ = nullptr;
};

}  // namespace vp9
}  // namespace hardware
