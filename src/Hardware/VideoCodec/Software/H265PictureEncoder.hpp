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

// An H.265 (HEVC Main / Main 10) encoder of single pictures driven from outside, the way Direct3D 12's HEVC video encoder is (ID3D12VideoEncodeCommandList2::EncodeFrame
// with D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC): the program keeps the decoded picture buffer, names the picture's type, order count, the pictures it keeps
// (the reference picture set: every descriptor, used by the picture or not, short or long term) and the final reference lists, and writes the VPS, SPS and PPS itself; the
// encoder writes the slice segment NAL units (Annex B, four-byte start codes; header and data) and the reconstruction (after deblocking and SAO), which is what a decoder
// shows and what later pictures refer to.
//
// The slices are what parameter sets of this form describe (the program's must say the same; write_parameter_sets below writes them):
//   VPS / SPS: one layer, sps_max_sub_layers above the highest temporal id, general_profile_idc 1 (Main) or 2 (Main 10), 4:2:0, bit_depth 8 (Main) or 10 (Main 10), pic_width / height_in_luma_samples
//        the coded size (multiples of the minimum coding block; a conformance window crops what is shown), log2_max_pic_order_cnt_lsb as given, log2_min_luma_coding_block_size /
//        log2_diff_max_min, log2_min_luma_transform_block_size / log2_diff_max_min, max_transform_hierarchy_depth_inter / intra as given (D3D12's configuration),
//        scaling_list_enabled 0, amp_enabled as given, sample_adaptive_offset_enabled as given, pcm 0, num_short_term_ref_pic_sets 0, long_term_ref_pics_present as given
//        with num_long_term_ref_pics_sps 0, sps_temporal_mvp_enabled 0, strong_intra_smoothing_enabled 0, no VUI needed, no extensions.
//   PPS: dependent_slice_segments 0, output_flag_present 0, num_extra_slice_header_bits 0, sign_data_hiding 0, cabac_init_present 0, num_ref_idx_l0/l1_default_active
//        anything (every P and B slice overrides them), init_qp_minus26 0, constrained_intra_pred as given, transform_skip_enabled as given, cu_qp_delta_enabled 1 with
//        diff_cu_qp_delta_depth 0 (a QP a coding tree block), cb / cr_qp_offset 0, slice_chroma_qp_offsets_present 0, weighted_pred 0, weighted_bipred 0,
//        transquant_bypass 0, tiles 0, entropy_coding_sync 0, pps_loop_filter_across_slices_enabled as given, deblocking_filter_control_present 1 with
//        deblocking_filter_override_enabled 0 and pps_deblocking_filter_disabled 0 (beta / tc offsets 0), pps_scaling_list_data_present 0,
//        lists_modification_present 1, log2_parallel_merge_level_minus2 0, slice_segment_header_extension_present 0, no extensions.
// What the slices hold: independent slice segments of whole coding tree blocks (16, 32 or 64 luma samples), each its own NAL unit (IDR_W_RADL for IDR pictures,
// TRAIL_R / TRAIL_N otherwise, nuh_temporal_id_plus1 the temporal layer + 1); the slice header codes the reference picture set explicitly
// (short_term_ref_pic_set_sps_flag 0, long-term pictures with delta_poc_msb_present_flag 1), the lists' sizes and - where the final lists are not the initial ones - the
// list modifications, five_minus_max_num_merge_cand 0, slice_qp_delta, the SAO flags; coding quadtrees down to the minimum coding block chosen by cost, 2Nx2N intra
// coding units (35 luma modes, chroma derived) and 2Nx2N inter coding units (merge, skip, or AMVP with one or two lists, quarter-sample vectors found with
// MotionEstimator and refined with HEVC's interpolation), transform trees split only where the sizes force it, CABAC residual coding of every transform size,
// cu_qp_delta for QP maps, SAO parameters per coding tree block chosen by the encoder when SAO is on.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/Software/VideoEncPlanes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

namespace h265 {
class PictureEncoder;
}

enum class H265EncFrameType : uint8_t { I = 0, P = 1, B = 2, Idr = 3 };   // (D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC's values)
// How the picture is cut into slices (D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE's values; BytesPerSlice is not done). Units are coding tree blocks.
enum class H265EncSliceMode : uint8_t { FullFrame = 0, BytesPerSlice = 1, CtbsPerSlice = 2, RowsPerSlice = 3, SlicesPerFrame = 4 };
enum class H265EncPrecision : uint8_t { Quarter, Half, Full };   // the finest vector the search may give
enum class H265EncResult : uint8_t {
  Ok,
  PictureControlNotSupported,    // a picture description the encoder cannot code (type, sets, lists, numbers out of range)
  SubregionLayoutNotSupported,   // the slice layout
  InvalidReferences,             // a reference picture of the wrong size, a list entry naming no picture the picture may use
  NoMemory,
};

constexpr uint32_t kH265EncMaxReferences = 16;   // descriptors (the decoded picture buffer)

// What the sequence and picture parameter sets say (D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC and the GOP structure).
struct H265EncSequence {
  uint32_t width = 0, height = 0;          // the coded size: multiples of 1 << log2_min_cb, at least 16 x 16, at most 8192 x 4320
  uint8_t bit_depth = 8;                   // 8 (Main) or 10 (Main 10)
  uint8_t log2_ctb = 5;                    // 4 .. 6
  uint8_t log2_min_cb = 3;                 // 3 .. log2_ctb
  uint8_t log2_min_tb = 2, log2_max_tb = 5;   // 2 <= min < log2_min_cb, 3 <= max <= 5, min <= max <= log2_ctb (luma transform blocks are at least 8x8)
  uint8_t max_th_depth_inter = 0, max_th_depth_intra = 0;   // 0 .. log2_ctb - log2_min_tb
  uint8_t log2_max_poc_lsb = 8;            // 4 .. 16
  bool amp = false, sao = false, long_term_refs = false, constrained_intra_pred = false, transform_skip = false, loop_filter_across_slices = true;
};

// A picture the program keeps (a descriptor of ReferenceFramesReconPictureDescriptors).
struct H265EncReference {
  EncPlanes16 picture;                     // its reconstruction (the sequence's size)
  int32_t poc = 0;                         // PictureOrderCountNumber
  bool long_term = false;
  bool used_by_current = true;             // IsRefUsedByCurrentPic
};

struct H265EncPicture {
  H265EncSequence seq;
  H265EncFrameType type = H265EncFrameType::Idr;
  uint32_t pps_id = 0;                     // slice_pic_parameter_set_id (0 .. 63)
  int32_t poc = 0;                         // PictureOrderCountNumber (written modulo 1 << log2_max_poc_lsb)
  uint8_t temporal_id = 0;                 // 0 .. 6
  bool reference = true;                   // TRAIL_R (a later picture may refer to it) or TRAIL_N; an IDR picture always is
  ArrayView<const H265EncReference> dpb;   // every picture kept (at most 16; short-term ones at most 8 before and 8 after the picture in output order, long-term at most 8)
  // The final reference lists as indices into dpb (pictures used by the current picture): list[0] for P and B, list[1] for B, at most 15 entries each.
  ArrayView<const uint8_t> list[2];
  // list_entry_lX as the program gives them (D3D12's pList0RefPicModifications), or empty: the encoder then writes what turns the initial lists into the final ones.
  ArrayView<const uint32_t> modification[2];
  int8_t qp = 26;                          // the slices' QP (-QpBdOffsetY .. 51)
  ArrayView<const int8_t> qp_map;          // empty, or one value per coding tree block (raster order): added to qp, or (qp_map_absolute) the block's QP
  bool qp_map_absolute = false;
  H265EncSliceMode slice_mode = H265EncSliceMode::FullFrame;
  uint32_t slice_value = 0;                // coding tree blocks, rows or slices, by the mode
  H265EncPrecision precision = H265EncPrecision::Quarter;
};

struct H265EncSlice {
  uint64_t offset = 0, size = 0;           // in the output (start code included)
  uint64_t header_bits = 0;                // the start code, NAL header and slice segment header (emulation prevention bytes included), in bits
};

class H265PictureEncoder {
 public:
  explicit H265PictureEncoder(allocators::IAllocator& allocator);
  ~H265PictureEncoder();
  H265PictureEncoder(const H265PictureEncoder&) = delete;
  H265PictureEncoder& operator=(const H265PictureEncoder&) = delete;

  // Whether the sequence is one the encoder codes (the ranges above).
  [[nodiscard]] static bool sequence_valid(const H265EncSequence& seq);
  // How many slices a layout makes of a picture of ctb_width x ctb_height coding tree blocks (0: the layout cannot be coded).
  static uint32_t slice_count(H265EncSliceMode mode, uint32_t value, uint32_t ctb_width, uint32_t ctb_height);

  // Codes one picture: `source` and `reconstruction` are the sequence's size; the slices are appended to `out` and described in `slices` (cleared first).
  [[nodiscard]] H265EncResult encode(const H265EncPicture& picture, const EncPlanes16& source, const EncWritablePlanes16& reconstruction, ByteBuffer* out,
                                     List<H265EncSlice>* slices, EncStatistics* statistics);

  // The VPS, SPS and PPS a program writes (as the header comment above lists them; Annex B, four-byte start codes), for tests and examples. `visible_width` /
  // `visible_height` (even) make the conformance window; general_level_idc is `level_idc` (30 x the level); `max_sub_layers` (1 .. 7) must exceed every temporal id
  // the pictures use (sps_max_sub_layers_minus1; the sub-layers' ordering info is the same for all).
  [[nodiscard]] static bool write_parameter_sets(const H265EncSequence& seq, uint32_t visible_width, uint32_t visible_height, uint32_t pps_id, uint8_t level_idc,
                                                 uint8_t max_sub_layers, ByteBuffer* out);

  // For tests: with set_drift_check(true) every picture's slices are also decoded without the in-loop filters and compared with the encoder's own reconstruction
  // before them; last_drift() is the number of samples that differed (0: the encoder predicts from exactly what a decoder has).
  void set_drift_check(bool on) { check_drift_ = on; }
  uint64_t last_drift() const { return drift_; }

 private:
  allocators::IAllocator& allocator_;
  UniquePointer<h265::PictureEncoder> core_;
  bool check_drift_ = false;
  uint64_t drift_ = 0;
};

}  // namespace hardware
