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


#pragma once
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

// The task commands the VCE 3.4 firmware (version 52 and later: the command set of Mesa's radeon_vce_52.c; Copyright 2015 Advanced Micro Devices, Inc.; MIT license) reads from
// an indirect buffer: how a program tells the encoder to open a session, how to encode, and to encode a picture. Every command is a block of dwords - the block's size in BYTES
// (itself included), the command id, then the command's fields - and a task is a list of blocks. A buffer is named by its 64-bit GPU address, high dword first.
//
// Nothing here touches hardware: a task is built into a buffer the caller gives. The values follow Mesa's radeon_vce_52.c and radeon_vce.c for a Polaris-family card that is
// not "dual pipe" and has one instance (the Vega M: radeon_vce.c enables dual_pipe only for chips from Tonga on that are not Stoney, Polaris11, Polaris12 or VegaM, and
// dual_inst only with no harvested instance), with a linear NV12 input picture and a coded picture buffer laid out as radeon_vce.c's si_vce_frame_offset does for a chip before
// Vega 10. The VUI block (use_vui in Mesa) is not built: the encoder's streams carry no VUI.
namespace amd_vce3 {

// pipe_h2645_enc_picture_type, pipe_h2645_enc_rate_control_method (p_video_state.h)
enum class PictureType : uint32_t { P = 0, B = 1, I = 2, IDR = 3, Skip = 4 };
enum class RateControlMethod : uint32_t { Disable = 0, ConstantSkip = 1, VariableSkip = 2, Constant = 3, Variable = 4, QualityVariable = 5 };
// The task_operation of the task info block.
enum class TaskOperation : uint32_t { Create = 0, Destroy = 1, Config = 2, Encode = 3 };

// The command ids (the second dword of a block).
constexpr uint32_t kCmdSession = 0x00000001, kCmdTaskInfo = 0x00000002, kCmdCreate = 0x01000001, kCmdDestroy = 0x02000001, kCmdEncode = 0x03000001,
                   kCmdConfigExtension = 0x04000001, kCmdPicControl = 0x04000002, kCmdRateControl = 0x04000005, kCmdMotionEstimation = 0x04000007, kCmdRdo = 0x04000008,
                   kCmdContextBuffer = 0x05000001, kCmdBitstreamBuffer = 0x05000004, kCmdFeedbackBuffer = 0x05000005;

// The field blocks, in the order the firmware reads them (radeon_vce.h).
struct RateControl {
  uint32_t rc_method = 0, target_bitrate = 0, peak_bitrate = 0, frame_rate_num = 0, gop_size = 0, quant_i_frames = 0, quant_p_frames = 0, quant_b_frames = 0, vbv_buffer_size = 0, frame_rate_den = 0,
           vbv_buf_lv = 0, max_au_size = 0, qp_initial_mode = 0, target_bits_picture = 0, peak_bits_picture_integer = 0, peak_bits_picture_fraction = 0, min_qp = 0, max_qp = 0, skip_frame_enable = 0,
           fill_data_enable = 0, enforce_hrd = 0, b_pics_delta_qp = 0, ref_b_pics_delta_qp = 0, rc_reinit_disable = 0, enc_lcvbr_init_qp_flag = 0, lcvbrsatd_based_nonlinear_bit_budget_flag = 0;
};
struct MotionEstimation {
  uint32_t enc_ime_decimation_search = 0, motion_est_half_pixel = 0, motion_est_quarter_pixel = 0, disable_favor_pmv_point = 0, force_zero_point_center = 0, lsmvert = 0, enc_search_range_x = 0,
           enc_search_range_y = 0, enc_search1_range_x = 0, enc_search1_range_y = 0, disable_16x16_frame1 = 0, disable_satd = 0, enable_amd = 0, enc_disable_sub_mode = 0, enc_ime_skip_x = 0,
           enc_ime_skip_y = 0, enc_en_ime_overw_dis_subm = 0, enc_ime_overw_dis_subm_no = 0, enc_ime2_search_range_x = 0, enc_ime2_search_range_y = 0, parallel_mode_speedup_enable = 0,
           fme0_enc_disable_sub_mode = 0, fme1_enc_disable_sub_mode = 0, ime_sw_speedup_enable = 0;
};
struct PicControl {
  uint32_t enc_use_constrained_intra_pred = 0, enc_cabac_enable = 0, enc_cabac_idc = 0, enc_loop_filter_disable = 0, enc_lf_beta_offset = 0, enc_lf_alpha_c0_offset = 0, enc_crop_left_offset = 0,
           enc_crop_right_offset = 0, enc_crop_top_offset = 0, enc_crop_bottom_offset = 0, enc_num_mbs_per_slice = 0, enc_intra_refresh_num_mbs_per_slot = 0, enc_force_intra_refresh = 0,
           enc_force_imb_period = 0, enc_pic_order_cnt_type = 0, log2_max_pic_order_cnt_lsb_minus4 = 0, enc_sps_id = 0, enc_pps_id = 0, enc_constraint_set_flags = 0, enc_b_pic_pattern = 0,
           weight_pred_mode_b_picture = 0, enc_number_of_reference_frames = 0, enc_max_num_ref_frames = 0, enc_num_default_active_ref_l0 = 0, enc_num_default_active_ref_l1 = 0, enc_slice_mode = 0,
           enc_max_slice_size = 0;
};
struct Rdo {
  uint32_t enc_disable_tbe_pred_i_frame = 0, enc_disable_tbe_pred_p_frame = 0, use_fme_interpol_y = 0, use_fme_interpol_uv = 0, use_fme_intrapol_y = 0, use_fme_intrapol_uv = 0, use_fme_interpol_y_1 = 0,
           use_fme_interpol_uv_1 = 0, use_fme_intrapol_y_1 = 0, use_fme_intrapol_uv_1 = 0, enc_16x16_cost_adj = 0, enc_skip_cost_adj = 0, enc_force_16x16_skip = 0, enc_disable_threshold_calc_a = 0,
           enc_luma_coeff_cost = 0, enc_luma_mb_coeff_cost = 0, enc_chroma_coeff_cost = 0;
};
struct CreateExtension {   // rvce_enc_create: what the create block carries that comes from state, not from the picture size
  uint32_t enc_use_circular_buffer = 0, enc_pic_struct_restriction = 0, enc_pre_encode_context_buffer_offset = 0, enc_pre_encode_input_luma_buffer_offset = 0,
           enc_pre_encode_input_chroma_buffer_offset = 0, enc_pre_encode_mode_chromaflag_vbaqmode_scenechangesensitivity = 0;
};
struct TaskInfo {
  uint32_t offset_of_next_task_info = 0xffffffff, task_operation = 0, reference_picture_dependency = 0, collocate_flag_dependency = 0, feedback_index = 0, video_bitstream_ring_index = 0;
};

// What a session is set up from: the picture size, the profile and level, the rate control and the group of pictures.
struct EncoderParameters {
  uint32_t width = 0, height = 0;          // luma samples
  uint32_t profile_idc = 66;               // u_get_h264_profile_idc: 66 baseline, 77 main, 100 high
  uint32_t level = 40;                     // level_idc: 10 .. 52
  uint32_t max_references = 1;
  RateControlMethod rate_control = RateControlMethod::Constant;
  uint32_t target_bitrate = 0, peak_bitrate = 0, frame_rate_num = 30, frame_rate_den = 1;
  uint32_t gop_size = 30, quant_i_frames = 26, quant_p_frames = 28, quant_b_frames = 30;
  uint32_t vbv_buffer_size = 0, vbv_buf_lv = 0;
  bool fill_data_enable = false, enforce_hrd = false, cabac = false;
};

// The blocks of a session, as radeon_vce_52.c's get_*_param functions make them from the parameters.
struct EncoderSetup {
  EncoderParameters parameters;
  RateControl rc;
  MotionEstimation me;
  PicControl pc;
  Rdo rdo;
  CreateExtension ec;
  uint32_t config_extension_perf_logging = 0;
  uint32_t feedback_ring_size = 0;
  uint32_t addrmode_arraymode_disrdo_distwoinstants = 0;
};
EncoderSetup make_setup(const EncoderParameters& parameters);
bool parameters_ok(const EncoderParameters& parameters);   // within what the engine and the buffers take

// The buffers the engine works in, sized as radeon_vce.c does for a chip before Vega 10 and an NV12 input: a picture is `pitch` bytes wide, `vpitch` rows tall for the luma and
// half as many for the chroma below it; the coded picture buffer holds `slots` of them.
struct PictureLayout {
  uint32_t pitch = 0;          // bytes per row of the input and of the buffer's pictures
  uint32_t vpitch = 0;         // luma rows, a multiple of 16
  uint32_t frame_bytes = 0;    // pitch * (vpitch + vpitch / 2): one picture of the coded picture buffer
};
PictureLayout picture_layout(uint32_t width, uint32_t height);
uint32_t cpb_slot_count(uint32_t width, uint32_t height, uint32_t level);   // get_cpb_num
uint64_t cpb_bytes(uint32_t width, uint32_t height, uint32_t level);        // the buffer: slots of aligned 32 rows, 3/2, per slot
void frame_offset(const PictureLayout& layout, uint32_t slot, uint32_t* luma_offset, uint32_t* chroma_offset);   // si_vce_frame_offset

// A list of command blocks being written into a caller's buffer. Never writes outside it; overflow() says it was too small.
class TaskWriter {
 public:
  explicit TaskWriter(ArrayView<uint32_t> out) : out_(out) {}
  void begin(uint32_t command);     // RVCE_BEGIN
  void put(uint32_t value);         // RVCE_CS
  void put_address(uint64_t gpu_address) { put(static_cast<uint32_t>(gpu_address >> 32)); put(static_cast<uint32_t>(gpu_address)); }   // RVCE_READ / _WRITE with a VM: high, low
  void end();                       // RVCE_END: the block's size in bytes into its first dword
  void patch(uint32_t index, uint32_t value);   // a dword already written
  uint32_t size() const { return count_; }
  bool overflow() const { return overflow_; }
  // The position of the offset_of_next_task_info dword of the last Encode task info (Mesa's task_info_idx), 0 if none.
  uint32_t last_encode_task_info = 0;

 private:
  ArrayView<uint32_t> out_;
  uint32_t count_ = 0, block_begin_ = 0;
  bool overflow_ = false, in_block_ = false;
};

// The blocks.
void write_session(TaskWriter& w, uint32_t stream_handle);
void write_task_info(TaskWriter& w, TaskOperation operation, uint32_t dependency, uint32_t feedback_index, uint32_t ring_index);
void write_feedback(TaskWriter& w, uint64_t feedback_gpu_address, uint32_t feedback_ring_size);
// The sizes the create block carries: the pitch of the reference pictures' luma and chroma, and the luma height in quad words.
void write_create(TaskWriter& w, const EncoderSetup& setup, const PictureLayout& layout);
void write_destroy(TaskWriter& w, uint64_t feedback_gpu_address, uint32_t feedback_ring_size);
void write_config(TaskWriter& w, const EncoderSetup& setup);   // task info (config), rate control, config extension, motion estimation, rdo, pic control

// One picture to encode: where it is, what the coded picture buffer holds, and the group-of-pictures state. The reference pictures are the buffer slots (offsets) the caller
// chose; `l0` and `l1` are used for P and B pictures.
struct ReferencePicture {
  PictureType type = PictureType::Skip;
  uint32_t frame_num = 0, pic_order_cnt = 0;
  uint32_t luma_offset = 0, chroma_offset = 0;
};
struct EncodeTask {
  PictureType picture_type = PictureType::IDR;
  uint32_t frame_num = 0, frame_num_cnt = 0, pic_order_cnt = 0, i_remain = 0, p_remain = 0;
  uint32_t ref_idx_l0 = 0;                // the frame number of the L0 reference (the reference list modification uses its distance)
  bool not_referenced = false;
  ReferencePicture l0, l1;
  uint32_t current_luma_offset = 0, current_chroma_offset = 0;   // the picture's own place in the coded picture buffer
  uint64_t input_luma_gpu_address = 0, input_chroma_gpu_address = 0;
  uint32_t bitstream_index = 0;           // enc->bs_idx: which bitstream ring entry this task writes (0 for one picture a job)
  uint64_t cpb_gpu_address = 0;
  uint64_t bitstream_gpu_address = 0;
  uint32_t bitstream_bytes = 0;
  uint64_t feedback_gpu_address = 0;
};
// session, the encode task (task info, the coded picture buffer, the bitstream buffer, the encode block) and the feedback buffer, as rvce_begin_frame / rvce_encode_bitstream do.
void write_encode(TaskWriter& w, const EncoderSetup& setup, const PictureLayout& layout, const EncodeTask& task, uint32_t stream_handle);

// The dwords of the feedback buffer the firmware fills (rvce_get_feedback): encStatus, encHasBitstream, ..., encBitstreamSize at 4 and the offset at 9.
constexpr uint32_t kFeedbackBytes = 512;
struct Feedback {
  bool has_bitstream = false;
  uint32_t bytes = 0;
};
Feedback read_feedback(ArrayView<const uint32_t> dwords);

}  // namespace amd_vce3
}  // namespace hardware
