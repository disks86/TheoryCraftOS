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

// An H.264 encoder of single pictures driven from outside, the way Direct3D 12's video encoder is (ID3D12VideoEncodeCommandList2::EncodeFrame): the program keeps the
// decoded picture buffer, names the picture's type, numbers, reference lists and marking, and writes the sequence and picture parameter sets itself; the encoder writes the
// slice NAL units (Annex B, four-byte start codes) and the reconstructed (deblocked) picture, which is what a decoder shows and what later pictures refer to.
//
// The slices are what parameter sets of this form describe (the program's must say the same):
//   SPS: frame_mbs_only_flag 1, direct_8x8_inference_flag 1, pic_order_cnt_type 0 or 2, log2_max_frame_num / log2_max_pic_order_cnt_lsb as given here, 8-bit 4:2:0, no
//        scaling matrices (seq_scaling_matrix_present_flag 0), frame cropping as the program likes (the encoder codes whole macroblocks); profile Main, or High;
//   PPS: entropy_coding_mode_flag as `entropy` says (CABAC: cabac_init_idc 0 is written in each P and B slice), bottom_field_pic_order_in_frame_present_flag 0, one slice
//        group, weighted_pred_flag 0, weighted_bipred_idc 0, pic_init_qp_minus26 0, chroma_qp_index_offset 0, deblocking_filter_control_present_flag 1,
//        constrained_intra_pred_flag as given, redundant_pic_cnt_present_flag 0, transform_8x8_mode_flag as `transform_8x8` says (High only), no pic_scaling_matrix,
//        second_chroma_qp_index_offset 0. (Every P and B slice overrides the PPS's default reference counts, so those may be anything.)
// What the slices hold: Intra16x16 macroblocks (all four luma and chroma modes), with the 8x8 transform also I_NxN macroblocks of Intra8x8 blocks (all nine modes), P_L0_16x16
// / P_Skip, B_L0_16x16 / B_L1_16x16 / B_Bi_16x16 and, with direct prediction (direct_spatial_mv_pred_flag as `direct` says), B_Direct_16x16 / B_Skip; quarter-sample vectors
// found by a diamond search with half and quarter refinement (and the program's hints), any reference of the lists, 4x4 transforms or (inter and Intra8x8 macroblocks)
// adaptively 8x8 ones, the in-loop filter as disable_deblocking_filter_idc says (0, 1 or 2), a QP per macroblock (a picture QP, or a map of deltas or absolute values per
// macroblock), rows of intra macroblocks for intra refresh, slices by macroblocks, rows, count or bytes.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace h264 {
class PictureCoder;
}

// An 8-bit 4:2:0 picture of whole macroblocks: Y rows of `width` samples, U and V rows of width / 2.
struct H264EncPlanes {
  ConstByteView y, u, v;
  uint32_t width = 0, height = 0;
};
struct H264EncWritablePlanes {
  ByteView y, u, v;
  uint32_t width = 0, height = 0;
};

enum class H264EncFrameType : uint8_t { I = 0, P = 1, B = 2, Idr = 3 };   // (D3D12_VIDEO_ENCODER_FRAME_TYPE_H264's values)
// How the picture is cut into slices (D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE's values): BytesPerSlice ends a slice before the macroblock that would take its NAL unit
// past `slice_value` bytes (start code included; a slice keeps at least one macroblock, so a macroblock bigger than that is a slice of its own).
enum class H264EncSliceMode : uint8_t { FullFrame = 0, BytesPerSlice = 1, MacroblocksPerSlice = 2, RowsPerSlice = 3, SlicesPerFrame = 4 };
// The finest vector the search may give (D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE: maximum and quarter are Quarter).
enum class H264EncPrecision : uint8_t { Quarter, Half, Full };
enum class H264EncEntropy : uint8_t { Cavlc, Cabac };
// Direct prediction in B slices (D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES' values): None leaves out B_Skip and B_Direct_16x16.
enum class H264EncDirect : uint8_t { None = 0, Temporal = 1, Spatial = 2 };
// How hints steer the motion search (D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE's values): Full searches from the encoder's own candidates and the hints, StartHint from
// the hints only, StartHintLimited from the hints and no further than `hint_limit` whole samples from them.
enum class H264EncHintMode : uint8_t { Full = 0, StartHint = 1, StartHintLimited = 2 };
enum class H264EncResult : uint8_t {
  Ok,
  PictureControlNotSupported,    // a picture description the encoder cannot code (type, lists, numbers out of range)
  SubregionLayoutNotSupported,   // the slice layout
  InvalidReferences,             // a reference picture of the wrong size
  NoMemory,
};

// The motion of one coded macroblock, as later pictures' direct prediction reads it from their co-located picture: for each 8x8 block (raster order) and list, the reference
// index (-1: the list is not used), the identity of the picture it refers to, and the vector (quarter samples).
struct H264EncMbMotion {
  uint8_t intra = 1;
  int8_t ref[2][4] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
  uint32_t ident[2][4] = {};
  int16_t mv[2][4][2] = {};
};
struct H264EncReference {
  H264EncPlanes picture;   // the reconstruction of an earlier picture
  uint32_t identity = 0;   // which picture it is (two entries of the lists with the same identity are the same picture)
  int32_t poc = 0;         // its PicOrderCnt (temporal direct prediction scales by it)
  bool long_term = false;
  ArrayView<const H264EncMbMotion> motion;   // its macroblocks' motion (what encode() gave), or empty: not known (it is then not a co-located picture direct prediction uses)
  uint32_t clean_rows = 0xFFFFFFFFu;         // macroblock rows from the top an intra refresh has made clean (all: everything)
  // The part of `picture` (from the top left, in luma samples) that is the reconstruction itself; outside it the samples are not (0: all of it is). Predictions read only
  // inside it, so that a reference whose last columns or rows are not known gives what a decoder gives.
  uint32_t exact_width = 0, exact_height = 0;
};
// memory_management_control_operation and its arguments, as the slice header has them (an operation 0 ends the list).
struct H264EncMarking {
  uint8_t operation = 0;
  uint32_t difference_of_pic_nums_minus1 = 0, long_term_pic_num = 0, long_term_frame_idx = 0, max_long_term_frame_idx_plus1 = 0;
};
// modification_of_pic_nums_idc and its argument (an idc of 3 ends the list).
struct H264EncListModification {
  uint8_t idc = 0;
  uint32_t abs_diff_pic_num_minus1 = 0, long_term_pic_num = 0;
};
// A macroblock's motion hint: a vector per list (quarter samples) into the reference of that index (-1: no hint for the list).
struct H264EncHint {
  int8_t ref[2] = {-1, -1};
  int16_t mv[2][2] = {};
};
// What encode() reports of each macroblock (when asked): its QP, the SATD of its luma residual before the transform (4x4 Hadamard), and the bits its syntax took.
struct H264EncMbReport {
  uint8_t qp = 0;
  uint32_t satd = 0, bits = 0;
};

struct H264EncPicture {
  uint32_t mb_width = 0, mb_height = 0;
  bool constrained_intra_pred = false;
  uint8_t disable_deblocking_filter_idc = 0;    // 0, 1 or 2
  H264EncEntropy entropy = H264EncEntropy::Cavlc;
  bool transform_8x8 = false;                     // the PPS's transform_8x8_mode_flag
  H264EncDirect direct = H264EncDirect::None;
  uint8_t log2_max_frame_num = 4, pic_order_cnt_type = 2, log2_max_poc_lsb = 4;
  H264EncFrameType type = H264EncFrameType::Idr;
  uint32_t idr_pic_id = 0, pic_parameter_set_id = 0;
  uint32_t frame_num = 0, poc = 0;               // (written modulo their maxima; `poc` as a signed number is PicOrderCnt for temporal direct prediction)
  bool reference = true;                          // nal_ref_idc != 0 (an IDR picture always is)
  bool adaptive_ref_pic_marking = false;
  ArrayView<const H264EncMarking> marking;
  // The final reference lists (after the modifications, which are written as given): list[0] for P and B, list[1] for B. At most 16 each.
  ArrayView<const H264EncReference> list[2];
  ArrayView<const H264EncListModification> modification[2];
  uint8_t qp = 26;                                // the slices' QP (0 .. 51)
  ArrayView<const int8_t> qp_map;                 // empty, or one value per macroblock: added to qp, or (qp_map_absolute) the macroblock's QP
  bool qp_map_absolute = false;
  H264EncSliceMode slice_mode = H264EncSliceMode::FullFrame;
  uint32_t slice_value = 0;                       // macroblocks, rows, slices or bytes, by the mode
  H264EncPrecision precision = H264EncPrecision::Quarter;
  // Intra refresh: the macroblock rows [refresh_begin, refresh_end) are coded intra; macroblocks above refresh_begin (refreshed by the pictures before) predict only from
  // references whose clean rows reach refresh_begin, and only from those rows (so that the refreshed part never depends on what is not refreshed yet).
  bool refresh = false;
  uint32_t refresh_begin = 0, refresh_end = 0;
  // Unchanged macroblocks (dirty regions): empty, or one flag per macroblock; a set flag codes the macroblock as a copy of list 0 entry `unchanged_ref` (vector 0, no residual).
  ArrayView<const uint8_t> unchanged;
  uint32_t unchanged_ref = 0;
  // Motion hints: empty, or one per macroblock.
  ArrayView<const H264EncHint> hints;
  H264EncHintMode hint_mode = H264EncHintMode::Full;
  uint32_t hint_limit = 0;
};

struct H264EncSlice {
  uint64_t offset = 0, size = 0;   // in the output (start code included)
  uint64_t header_bits = 0;        // the start code, NAL header and slice header (with their emulation prevention bytes), in bits
  uint32_t first_mb = 0, mb_count = 0;
};
struct H264EncStatistics {
  uint64_t qp_sum = 0;             // over the macroblocks
  uint64_t intra = 0, inter = 0, skipped = 0;   // (skipped: P_Skip / B_Skip; inter counts the rest of the inter macroblocks)
  uint64_t direct = 0;                           // B_Skip and B_Direct_16x16 macroblocks
  int64_t vector_x_sum = 0, vector_y_sum = 0;   // of the inter macroblocks' list 0 (else list 1) vectors, quarter samples
  uint32_t clean_rows = 0xFFFFFFFFu;            // the rows of this picture an intra refresh has made clean (for its H264EncReference when it is one)
};
// Where encode() puts what the program asked for besides the slices: the motion of each macroblock (for later direct prediction) and the per-macroblock report; each
// empty or one entry per macroblock.
struct H264EncOutputs {
  ArrayView<H264EncMbMotion> motion;
  ArrayView<H264EncMbReport> report;
};

class H264PictureEncoder {
 public:
  explicit H264PictureEncoder(allocators::IAllocator& allocator);
  ~H264PictureEncoder();
  H264PictureEncoder(const H264PictureEncoder&) = delete;
  H264PictureEncoder& operator=(const H264PictureEncoder&) = delete;

  // How many slices a layout makes of a picture of `mbs` macroblocks (0: the layout cannot be coded). BytesPerSlice gives 1: the count is known once the picture is coded.
  static uint32_t slice_count(H264EncSliceMode mode, uint32_t value, uint32_t mb_width, uint32_t mb_height);

  // Codes one picture: `source` and `reconstruction` are mb_width * 16 x mb_height * 16; the slices are appended to `out` and described in `slices` (cleared first).
  [[nodiscard]] H264EncResult encode(const H264EncPicture& picture, const H264EncPlanes& source, const H264EncWritablePlanes& reconstruction, ByteBuffer* out,
                                     List<H264EncSlice>* slices, H264EncStatistics* statistics, const H264EncOutputs& outputs = H264EncOutputs());

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<h264::PictureCoder> core_;
};

}  // namespace hardware
