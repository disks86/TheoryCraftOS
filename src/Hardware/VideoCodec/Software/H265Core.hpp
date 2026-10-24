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


// The internals of the H.265 decoder: parameter sets, pictures, the per-picture block state and the Core class whose methods are spread over H265Syntax.cpp (NAL
// units, parameter sets, slice headers), H265Picture.cpp (picture order, reference picture sets, output), H265Slice.cpp (slice data: coding quadtree, CABAC syntax,
// motion vector prediction), H265Predict.cpp (intra and inter prediction, inverse transforms) and H265Filter.cpp (deblocking and sample adaptive offset). Numbers in
// comments refer to ITU-T H.265.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/Software/H264Core.hpp"
#include "Hardware/VideoCodec/Software/H265.hpp"
#include "Hardware/VideoCodec/Software/H265Cabac.hpp"
#include "ObjectContainer/List.hpp"

namespace hardware {
namespace h265 {

using h264::BitReader;
using h264::Mem;

constexpr int kMaxPics = 17;        // 16 pictures in the buffer and the one being decoded
constexpr int kMaxRefs = 16;
constexpr int kMaxCtbs = 4096 * 2304 / (16 * 16);

enum NalType : int {
  kTrailN = 0, kTrailR = 1, kTsaN = 2, kTsaR = 3, kStsaN = 4, kStsaR = 5, kRadlN = 6, kRadlR = 7, kRaslN = 8, kRaslR = 9,
  kBlaWLp = 16, kBlaWRadl = 17, kBlaNLp = 18, kIdrWRadl = 19, kIdrNLp = 20, kCra = 21,
  kVps = 32, kSpsNal = 33, kPpsNal = 34, kAud = 35, kEos = 36, kEob = 37, kSeiSuffix = 40
};
inline bool is_irap(int t) { return t >= 16 && t <= 23; }
inline bool is_idr(int t) { return t == kIdrWRadl || t == kIdrNLp; }
inline bool is_bla(int t) { return t >= kBlaWLp && t <= kBlaNLp; }
inline bool is_rasl(int t) { return t == kRaslN || t == kRaslR; }
inline bool is_radl(int t) { return t == kRadlN || t == kRadlR; }
// Sub-layer non-reference pictures: types 0, 2, 4, 6, 8, 10, 12, 14.
inline bool is_sub_layer_non_ref(int t) { return t <= 14 && (t & 1) == 0; }

enum SliceType : int { kSliceB = 0, kSliceP = 1, kSliceI = 2 };

// 7.3.7 - a short-term reference picture set.
struct StRps {
  int num_negative = 0, num_positive = 0;
  int delta_poc_s0[16] = {}, delta_poc_s1[16] = {};
  bool used_s0[16] = {}, used_s1[16] = {};
  int num_delta_pocs() const { return num_negative + num_positive; }
};

// 7.3.4 - scaling lists, in raster order: sizeId 0 is 4x4 (16 entries), 1..3 are 8x8 (64 entries, up-sampled for 16x16 and 32x32), with the DC value of the big ones.
struct ScalingList {
  uint8_t list[4][6][64];
  uint8_t dc[4][6];
  void set_default();
};

struct Sps {
  bool valid = false;
  int id = 0;
  int max_sub_layers = 1;
  int chroma_format_idc = 1;
  int width = 0, height = 0;                       // luma samples
  int crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;   // luma samples
  int bit_depth_luma = 8, bit_depth_chroma = 8;
  int log2_max_poc_lsb = 4;
  int max_dec_pic_buffering[8] = {}, num_reorder[8] = {}, max_latency_increase_plus1[8] = {};
  int log2_min_cb = 3, log2_ctb = 4, log2_min_tb = 2, log2_max_tb = 5;
  int max_th_depth_inter = 0, max_th_depth_intra = 0;
  bool scaling_list_enabled = false;
  bool amp = false, sao = false, pcm = false;
  int pcm_bit_depth_luma = 8, pcm_bit_depth_chroma = 8, log2_min_pcm = 3, log2_max_pcm = 3;
  bool pcm_loop_filter_disabled = false;
  int num_st_rps = 0;
  StRps st_rps[65];
  bool long_term_ref_pics_present = false;
  int num_lt_sps = 0;
  int lt_ref_pic_poc_lsb_sps[33] = {};
  bool used_by_curr_pic_lt_sps[33] = {};
  bool temporal_mvp = false, strong_intra_smoothing = false;
  ScalingList scaling;
  // derived
  int ctb_size = 16, pic_w_ctb = 0, pic_h_ctb = 0;
};

struct Pps {
  bool valid = false;
  int id = 0, sps_id = 0;
  bool dependent_slice_segments = false, output_flag_present = false;
  int num_extra_slice_header_bits = 0;
  bool sign_data_hiding = false, cabac_init_present = false;
  int num_ref_idx_default[2] = {1, 1};
  int init_qp = 26;
  bool constrained_intra_pred = false, transform_skip = false, cu_qp_delta_enabled = false;
  int diff_cu_qp_delta_depth = 0;
  int cb_qp_offset = 0, cr_qp_offset = 0;
  bool slice_chroma_qp_offsets_present = false, weighted_pred = false, weighted_bipred = false, transquant_bypass = false;
  bool tiles_enabled = false, entropy_coding_sync = false;
  int num_tile_cols = 1, num_tile_rows = 1;
  bool uniform_spacing = true;
  int col_width_minus1[20] = {}, row_height_minus1[22] = {};
  bool loop_filter_across_tiles = true, loop_filter_across_slices = false;
  bool deblocking_control_present = false, deblocking_override_enabled = false, deblocking_disabled = false;
  int beta_offset_div2 = 0, tc_offset_div2 = 0;
  bool scaling_list_present = false;
  ScalingList scaling;
  bool lists_modification_present = false;
  int log2_parallel_merge_level = 2;
  bool slice_header_extension_present = false;
};

struct SliceHeader {
  int nal_type = 0, temporal_id = 0;
  bool first_slice_segment_in_pic = false, no_output_of_prior_pics = false;
  int pps_id = 0;
  bool dependent_slice_segment = false;
  int segment_address = 0;
  int type = kSliceI;
  bool pic_output_flag = true;
  int poc_lsb = 0;
  StRps st_rps;
  int num_lt = 0;
  int lt_poc_lsb[33] = {};
  bool lt_used_by_curr[33] = {}, lt_msb_present[33] = {};
  int lt_delta_poc_msb_cycle[33] = {};
  bool temporal_mvp = false, sao_luma = false, sao_chroma = false;
  int num_ref_idx[2] = {0, 0};
  bool list_modification[2] = {false, false};
  int list_entry[2][kMaxRefs] = {};
  bool mvd_l1_zero = false, cabac_init_flag = false, collocated_from_l0 = true;
  int collocated_ref_idx = 0;
  int luma_log2_denom = 0, chroma_log2_denom = 0;
  bool luma_weight_flag[2][kMaxRefs] = {}, chroma_weight_flag[2][kMaxRefs] = {};
  int luma_weight[2][kMaxRefs] = {}, luma_offset[2][kMaxRefs] = {};
  int chroma_weight[2][kMaxRefs][2] = {}, chroma_offset[2][kMaxRefs][2] = {};
  int max_num_merge_cand = 5;
  int slice_qp = 26;
  int cb_qp_offset = 0, cr_qp_offset = 0;
  bool deblocking_disabled = false;
  int beta_offset_div2 = 0, tc_offset_div2 = 0;
  bool loop_filter_across_slices = false;
  int num_entry_points = 0;
  uint64_t data_bit_offset = 0;   // where slice_segment_data() starts, in bits from the start of the payload (after the two header bytes), emulation prevention removed
  int num_pic_total_curr = 0;
  int num_delta_pocs_ref_rps_idx = 0;   // when the slice header's reference picture set is predicted from another: how many pictures that one names
};

// Motion of a 4x4 block.
struct MvField {
  int16_t mv[2][2];
  int8_t ref_idx[2];
  uint8_t pred_flags;   // bit 0: list 0 is used, bit 1: list 1
};

// What a later picture needs of a coded picture's motion (collocated prediction), kept for blocks of 16x16 luma samples: the motion of the top-left 4x4 block, with
// the pictures it refers to named by order count.
struct ColBlock {
  int16_t mv[2][2];
  int32_t ref_poc[2];
  uint8_t pred_flags;
  uint8_t long_term;   // bit per list: the reference picture was a long-term one
};

struct Picture {
  Mem<uint8_t> plane[3];
  int stride[3] = {0, 0, 0};
  int width = 0, height = 0;                          // luma samples, as coded
  int crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0; // the conformance window, in luma samples
  bool full_range = false;
  Mem<ColBlock> col;
  int poc = 0;
  bool in_use = false, is_ref = false, is_long = false, needed_for_output = false, generated = false;
  bool force_out = false;                              // belongs to an earlier coded video sequence: goes out before anything of the current one
  int latency_count = 0;
  int64_t pts = 0;
  uint32_t order = 0;   // decode order, for picking the oldest
};

struct SaoParams {
  uint8_t type[3] = {0, 0, 0};            // 0 off, 1 band, 2 edge
  uint8_t band_position[3] = {0, 0, 0};
  uint8_t eo_class[3] = {0, 0, 0};
  int8_t offset[3][4] = {};               // SaoOffsetVal[1..4]
};

// A slice segment as the loop filters and the collocated prediction need it later.
struct SliceInfo {
  int slice_addr_rs = 0;              // address of the first CTB of the slice (not the segment)
  bool deblocking_disabled = false;
  int beta_offset_div2 = 0, tc_offset_div2 = 0;
  bool loop_filter_across_slices = false;
  int cb_qp_offset = 0, cr_qp_offset = 0;
  bool sao_luma = false, sao_chroma = false;
  Picture* ref_pic[2][kMaxRefs] = {};
  int ref_poc[2][kMaxRefs] = {};
  bool ref_long[2][kMaxRefs] = {};
};

// A picture handed to another decoder (a hardware engine) instead of being decoded here: the parameter sets, the reference picture set as numbers the engine
// can use, the slice NAL units as they came, and the picture's planes to fill. Everything else - picture order, reference marking, the output order, the
// hash check - stays with the Core.
struct OffloadReference {
  int slot = 0;          // which picture buffer of the Core the reference is (stable for as long as it stays a reference)
  int poc = 0;
  bool is_long = false;
};
// A slice segment of the picture for an engine that wants its header: the parsed header (a dependent segment's is that of the independent segment before it, with its own
// address) and the segment's final reference picture lists as indexes into OffloadPicture::refs (0xFF: no entry).
struct OffloadSlice {
  const SliceHeader* header = nullptr;
  uint8_t list[2][kMaxRefs - 1] = {};
};

struct OffloadPicture {
  const Sps* sps = nullptr;
  const Pps* pps = nullptr;
  const SliceHeader* first = nullptr;                 // the header of the first slice
  int poc = 0;
  int slot = 0;
  bool reference = true;                              // a later picture of the same sub-layer may refer to it (not a sub-layer non-reference picture)
  uint32_t ref_count = 0;
  OffloadReference refs[kMaxRefs];                    // the pictures kept as references, which the three sets below index
  uint32_t st_before_count = 0, st_after_count = 0, lt_count = 0;
  uint8_t st_before[kMaxRefs] = {}, st_after[kMaxRefs] = {}, lt_curr[kMaxRefs] = {};
  uint8_t list[2][kMaxRefs - 1] = {};                 // the first slice's final reference picture lists, as indexes into refs (0xFF: no entry)
  ArrayView<const ConstByteView> slices;              // the NAL units of the picture, header bytes included
  ArrayView<const OffloadSlice> slice_info;           // one per entry of `slices`
  ByteView plane[3];                                  // I420, the coded size
  int stride[3] = {0, 0, 0};
};
class IOffload {
 public:
  // Whether the pictures of this sequence can be decoded by the offload (a size or a tool it cannot take: no). Asked at every picture; a picture it refuses is
  // decoded by the Core itself, so a stream should be refused from its first picture on or not at all.
  virtual bool accepts(const Sps& sps, const Pps& pps) = 0;
  // Decodes the picture into the planes. False: the planes are left grey and the picture counts as damaged.
  virtual bool decode_picture(const OffloadPicture& picture) = 0;

 protected:
  constexpr IOffload() = default;
  ~IOffload() = default;
};

class Core {
 public:
  explicit Core(allocators::IAllocator& allocator) : allocator_(allocator), slices_(allocator), offload_nals_(allocator), offload_headers_(allocator), offload_info_(allocator) {}
  ~Core() = default;

  bool configure(ConstByteView hvcc);
  bool decode(ConstByteView data, int64_t pts_us);
  bool receive(VideoFrame* frame);
  void flush();
  void reset();
  VideoFormat output_format() const;
  bool ready() const { return output_ready(); }
  // How many pictures carried a decoded picture hash (SEI) and how many of them the decoder's own picture did not match.
  H265Decoder::HashStatistics hash_statistics() const { return hash_stats_; }
  void set_plane_hasher(IPlaneHasher* hasher) { hasher_ = hasher; }
  // Pictures are decoded by `offload` from then on (nullptr: here again). Set it before the first picture.
  void set_offload(IOffload* offload) { offload_ = offload; }

 private:
  // --- H265Syntax.cpp
  bool handle_nal(const uint8_t* nal, uint64_t size, int64_t pts);
  bool parse_sps(BitReader& br);
  bool parse_pps(BitReader& br);
  bool parse_slice_header(BitReader& br, int nal_type, int temporal_id, SliceHeader* sh);
  bool parse_st_rps(BitReader& br, int idx, int num_sets, const StRps* sets, StRps* out);
  bool parse_scaling_list(BitReader& br, ScalingList* list);
  void parse_sei(BitReader& br);
  void verify_picture_hash();

  // --- H265Picture.cpp
  bool start_picture(const SliceHeader& sh, int64_t pts);
  bool setup_picture_state();
  void build_tile_tables();
  bool derive_rps(const SliceHeader& sh);
  Picture* generate_missing(int poc, bool long_term);
  bool build_ref_lists(const SliceHeader& sh, SliceInfo* info);
  void finish_picture();
  void run_offload(Picture* picture);
  void bump_output(bool all);
  bool output_ready() const;
  Picture* find_free_picture();
  bool alloc_picture(Picture* p);
  void clear_dpb();

  // --- H265Slice.cpp
  bool decode_slice_data(const SliceHeader& sh, BitReader& br);
  void coding_quadtree(int x0, int y0, int log2_size, int depth);
  void coding_unit(int x0, int y0, int log2_size);
  void prediction_unit(int x0, int y0, int w, int h, int part_idx, int log2_cb, int x_cb, int y_cb);
  void transform_tree(int x0, int y0, int x_base, int y_base, int log2_size, int depth, int blk_idx, bool cbf_cb_parent[2], bool cbf_cr_parent[2], int inter_split_flag);
  void transform_unit(int x0, int y0, int x_base, int y_base, int log2_size, int depth, int blk_idx, bool cbf_luma, bool cbf_cb[2], bool cbf_cr[2]);
  void residual_coding(int x0, int y0, int log2_size, int c_idx);
  void read_sao(int rx, int ry);
  void read_pcm(int x0, int y0, int log2_size);
  void update_qp(int x_cb, int y_cb);
  void set_qp_block(int x0, int y0, int w, int h, int qp);
  int  chroma_qp(int c_idx) const;
  bool available(int x_cur, int y_cur, int x_n, int y_n) const;
  bool available_pb(int x_cb, int y_cb, int n_cb, int x_pb, int y_pb, int w, int h, int part_idx, int x_n, int y_n) const;
  // motion
  void merge_motion(int x_cb, int y_cb, int n_cb, int x_pb, int y_pb, int w, int h, int part_idx, int merge_idx, MvField* out);
  void amvp(int x_cb, int y_cb, int n_cb, int x_pb, int y_pb, int w, int h, int part_idx, int list, int ref_idx, int mvp_flag, int mv[2]);
  bool temporal_mv(int x_pb, int y_pb, int w, int h, int ref_idx, int list, int mv[2]);
  bool col_mv(const Picture* col, int xcol, int ycol, int ref_idx, int list, int mv[2]);
  void store_motion(int x0, int y0, int w, int h, const MvField& m);

  // --- H265Predict.cpp
  void predict_intra(int x0, int y0, int log2_size, int c_idx, int mode);
  void predict_inter(int x0, int y0, int w, int h, const MvField& m);
  void reconstruct_block(int x0, int y0, int log2_size, int c_idx);

  // --- H265Filter.cpp
  void deblock_picture();
  void sao_picture();

  allocators::IAllocator& allocator_;

  // parameter sets
  Sps sps_[16];
  Pps pps_[64];
  int nal_length_size_ = 0;
  bool have_sps_ = false;

  // the decoded picture buffer
  Picture pics_[kMaxPics];
  Picture* cur_ = nullptr;
  uint32_t decode_counter_ = 0;
  uint64_t output_counter_ = 0;
  Picture* rps_st_curr_before_[16] = {};
  Picture* rps_st_curr_after_[16] = {};
  Picture* rps_lt_curr_[16] = {};
  int num_st_before_ = 0, num_st_after_ = 0, num_lt_curr_ = 0;
  int prev_tid0_poc_lsb_ = 0, prev_tid0_poc_msb_ = 0;
  bool first_picture_ = true, no_rasl_output_ = true, after_eos_ = true;
  bool skip_rasl_ = false;
  int poc_ = 0;

  // active parameter sets and the state of the picture being decoded
  Sps active_sps_;                       // copies taken when a picture starts: a parameter set sent while the picture is being decoded cannot change it
  Pps active_pps_;
  const Sps* sp_ = nullptr;
  const Pps* pp_ = nullptr;
  bool picture_open_ = false;
  int w4_ = 0, h4_ = 0;                  // the picture in 4x4 blocks
  int ctb_w_ = 0, ctb_h_ = 0;
  SliceHeader first_header_, sh_;
  Mem<uint8_t> pred_mode_;               // per 4x4: 0 not decoded, 1 intra, 2 inter; bit 2: skipped; bit 3: no loop filter (pcm/bypass)
  Mem<uint8_t> intra_mode_;
  Mem<int8_t> qp_map_;
  Mem<uint8_t> ct_depth_;
  Mem<MvField> mvf_;
  Mem<uint8_t> nz_;
  Mem<uint8_t> edge_;                    // bit 0 vertical TU edge, 1 vertical PU edge, 2 horizontal TU edge, 3 horizontal PU edge (at the left / top of the 4x4 block)
  Mem<uint32_t> zscan_;
  Mem<int32_t> rs_to_ts_, ts_to_rs_, tile_id_ts_;
  Mem<int32_t> col_bd_, row_bd_;
  Mem<int32_t> ctb_slice_;               // per CTB (raster): index into slices_, -1 if not decoded
  Mem<SaoParams> sao_;
  Mem<uint8_t> sao_plane_[3];
  List<SliceInfo> slices_;
  int slice_idx_ = -1;
  IOffload* offload_ = nullptr;
  bool offloaded_ = false;               // the picture being decoded goes to the offload
  int rps_ref_delta_pocs_ = 0;
  List<ConstByteView> offload_nals_;     // the slice NAL units of the picture being collected for the offload
  List<SliceHeader> offload_headers_;    // their headers
  List<OffloadSlice> offload_info_;      // built when the picture goes to the offload
  ConstByteView current_nal_;            // the NAL unit being handled

  // slice data state
  BitReader* br_ = nullptr;
  Cabac cabac_;
  uint8_t wpp_saved_[kContextCount] = {};
  bool wpp_saved_valid_ = false;
  SliceInfo* slice_ = nullptr;
  int slice_addr_rs_ = 0;
  int ctb_addr_ts_ = 0, ctb_addr_rs_ = 0;
  int qp_y_ = 26, qp_y_prev_ = 26;
  bool is_cu_qp_delta_coded_ = false;
  int cu_qp_delta_val_ = 0;
  int qg_x_ = 0, qg_y_ = 0;
  bool cu_transquant_bypass_ = false;
  bool cur_intra_ = false;
  int cur_part_mode_ = 0;
  bool merge_flag_cu_ = false;
  bool cu_skip_ = false;
  int max_trafo_depth_ = 0;
  bool ts_flag_ = false;
  int cur_cb_x_ = 0, cur_cb_y_ = 0, cur_cb_log2_ = 0;
  int intra_luma_modes_[4] = {};
  int intra_chroma_mode_[4] = {};
  int qg_pred_ = 26;                     // qPY_PRED of the current quantization group
  uint8_t ctx_end_of_segment_[kContextCount] = {};   // context variables at the end of the previous slice segment (for a dependent one)
  bool ctx_end_valid_ = false;
  int cur_pcm_ = 0;
  bool no_backward_pred_ = false;
  int32_t coeff_[32 * 32];
  int32_t resid_[32 * 32];
  int16_t pred_buf_[2][64 * 64];

  Mem<uint8_t> rbsp_;
  // the decoded picture hash (D.3.19) announced for the picture being decoded
  struct PendingHash {
    bool present = false;
    int type = 0;              // 0 MD5, 1 CRC, 2 checksum
    uint8_t md5[3][16] = {};
    uint32_t value[3] = {};    // CRC or checksum
  } pending_hash_;
  H265Decoder::HashStatistics hash_stats_;
  IPlaneHasher* hasher_ = nullptr;
  // the picture's output
  VideoFormat out_format_;
  Mem<uint8_t> out_frame_;
  Picture* returned_ = nullptr;
};

}  // namespace h265
}  // namespace hardware
