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


// The values and the order of the fields follow Mesa's radeon_vce_52.c and radeon_vce.c (Copyright Advanced Micro Devices, Inc.; MIT license).
#include "Hardware/VideoCodec/AMD/AmdVce3Messages.hpp"

namespace hardware {
namespace amd_vce3 {

namespace {
constexpr uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
constexpr uint32_t kMaxWidth = 4096, kMaxHeight = 2304;

// Writes every dword of a field block, in the order of its declaration (which is the order the firmware reads them). The blocks are plain arrays of dwords.
template <class T>
void put_block(TaskWriter& w, const T& block) {
  static_assert(sizeof(T) % 4 == 0, "a field block is dwords");
  const uint32_t* words = reinterpret_cast<const uint32_t*>(&block);   // (a struct of uint32_t: no padding, aligned like the dwords it is)
  for (uint32_t i = 0; i < sizeof(T) / 4; ++i) w.put(words[i]);
}
static_assert(sizeof(RateControl) == 26 * 4 && sizeof(MotionEstimation) == 24 * 4 && sizeof(PicControl) == 27 * 4 && sizeof(Rdo) == 17 * 4, "the blocks have the firmware's field counts");
}  // namespace

void TaskWriter::begin(uint32_t command) {
  if (in_block_) overflow_ = true;
  in_block_ = true;
  block_begin_ = count_;
  put(0);   // (the size, filled in by end())
  put(command);
}

void TaskWriter::put(uint32_t value) {
  if (count_ >= out_.size()) { overflow_ = true; return; }
  out_[count_++] = value;
}

void TaskWriter::patch(uint32_t index, uint32_t value) {
  if (index >= count_) { overflow_ = true; return; }
  out_[index] = value;
}

void TaskWriter::end() {
  if (!in_block_ || overflow_) { overflow_ = true; return; }
  out_[block_begin_] = (count_ - block_begin_) * 4;
  in_block_ = false;
}

bool parameters_ok(const EncoderParameters& p) {
  if (p.width < 16 || p.height < 16 || p.width > kMaxWidth || p.height > kMaxHeight || p.width % 2 != 0 || p.height % 2 != 0) return false;
  if (p.profile_idc != 66 && p.profile_idc != 77 && p.profile_idc != 88 && p.profile_idc != 100) return false;
  if (p.level < 10 || p.level > 52 || p.max_references > 16 || static_cast<uint32_t>(p.rate_control) > 5) return false;
  if (p.frame_rate_num == 0 || p.frame_rate_den == 0 || p.gop_size == 0 || p.quant_i_frames > 51 || p.quant_p_frames > 51 || p.quant_b_frames > 51) return false;
  return true;
}

EncoderSetup make_setup(const EncoderParameters& p) {
  EncoderSetup s;
  s.parameters = p;
  // get_rate_control_param
  s.rc.rc_method = static_cast<uint32_t>(p.rate_control);
  s.rc.target_bitrate = p.target_bitrate;
  s.rc.peak_bitrate = p.peak_bitrate;
  s.rc.quant_i_frames = p.quant_i_frames;
  s.rc.quant_p_frames = p.quant_p_frames;
  s.rc.quant_b_frames = p.quant_b_frames;
  s.rc.gop_size = p.gop_size;
  s.rc.frame_rate_num = p.frame_rate_num;
  s.rc.frame_rate_den = p.frame_rate_den;
  s.rc.max_qp = 51;
  s.rc.vbv_buffer_size = p.vbv_buffer_size;
  s.rc.vbv_buf_lv = p.vbv_buf_lv;
  s.rc.fill_data_enable = p.fill_data_enable ? 1 : 0;
  s.rc.enforce_hrd = p.enforce_hrd ? 1 : 0;
  // get_motion_estimation_param
  s.me.motion_est_quarter_pixel = 1;
  s.me.enc_disable_sub_mode = 254;
  s.me.lsmvert = 2;
  s.me.enc_en_ime_overw_dis_subm = 0;
  s.me.enc_ime_overw_dis_subm_no = 0;
  s.me.enc_ime2_search_range_x = 4;
  s.me.enc_ime2_search_range_y = 4;
  s.me.enc_ime_decimation_search = 0x00000001;
  s.me.motion_est_half_pixel = 0x00000001;
  s.me.enc_search_range_x = 0x00000010;
  s.me.enc_search_range_y = 0x00000010;
  s.me.enc_search1_range_x = 0x00000010;
  s.me.enc_search1_range_y = 0x00000010;
  // get_pic_control_param (no frame cropping flag: the padding of the last macroblock is cropped, in units of two samples)
  uint32_t enc_num_mbs_per_slice = align_up(p.width, 16) / 16;
  enc_num_mbs_per_slice *= align_up(p.height, 16) / 16;
  s.pc.enc_crop_right_offset = (align_up(p.width, 16) - p.width) >> 1;
  s.pc.enc_crop_bottom_offset = (align_up(p.height, 16) - p.height) >> 1;
  s.pc.enc_num_mbs_per_slice = enc_num_mbs_per_slice;
  s.pc.enc_b_pic_pattern = (p.max_references > 1 ? p.max_references : 1) - 1;
  s.pc.enc_number_of_reference_frames = p.max_references < 1 ? p.max_references : 1;
  s.pc.enc_max_num_ref_frames = p.max_references + 1;
  s.pc.enc_num_default_active_ref_l0 = 0x00000001;
  s.pc.enc_num_default_active_ref_l1 = 0x00000001;
  s.pc.enc_cabac_enable = p.cabac ? 1 : 0;
  s.pc.enc_constraint_set_flags = 0x00000040;
  // get_feedback_buffer_param, get_config_ext_param
  s.feedback_ring_size = 0x00000001;
  s.config_extension_perf_logging = 0x00000003;
  // si_vce_52_get_param: not "dual inst"
  s.addrmode_arraymode_disrdo_distwoinstants = 0x01000201;
  return s;
}

PictureLayout picture_layout(uint32_t width, uint32_t height) {
  PictureLayout l;
  l.pitch = align_up(width, 256);   // (a multiple of 128, which the buffer's pictures need; the input uses the same pitch)
  l.vpitch = align_up(height, 16);
  l.frame_bytes = l.pitch * (l.vpitch + l.vpitch / 2);
  return l;
}

uint32_t cpb_slot_count(uint32_t width, uint32_t height, uint32_t level) {
  const uint32_t w = align_up(width, 16) / 16, h = align_up(height, 16) / 16;
  uint32_t dpb;
  switch (level) {
    case 10: dpb = 396; break;
    case 11: dpb = 900; break;
    case 12: case 13: case 20: dpb = 2376; break;
    case 21: dpb = 4752; break;
    case 22: case 30: dpb = 8100; break;
    case 31: dpb = 18000; break;
    case 32: dpb = 20480; break;
    case 40: case 41: dpb = 32768; break;
    case 42: dpb = 34816; break;
    case 50: dpb = 110400; break;
    default: dpb = 184320; break;   // 51, 52 and anything else
  }
  const uint32_t n = dpb / (w * h);
  return n < 16 ? n : 16;
}

uint64_t cpb_bytes(uint32_t width, uint32_t height, uint32_t level) {
  const PictureLayout l = picture_layout(width, height);
  // radeon_vce.c: align(pitch, 128) * align(rows, 32), 3/2 of it, once for each slot.
  const uint64_t per_slot = uint64_t{l.pitch} * align_up(height, 32) * 3 / 2;
  return per_slot * cpb_slot_count(width, height, level);
}

// si_vce_frame_offset (a chip before Vega 10)
void frame_offset(const PictureLayout& layout, uint32_t slot, uint32_t* luma_offset, uint32_t* chroma_offset) {
  *luma_offset = slot * layout.frame_bytes;
  *chroma_offset = *luma_offset + layout.pitch * layout.vpitch;
}

void write_session(TaskWriter& w, uint32_t stream_handle) {
  w.begin(kCmdSession);   // session cmd
  w.put(stream_handle);
  w.end();
}

void write_task_info(TaskWriter& w, TaskOperation operation, uint32_t dependency, uint32_t feedback_index, uint32_t ring_index) {
  TaskInfo ti;
  w.begin(kCmdTaskInfo);   // task info
  if (operation == TaskOperation::Encode) {
    // The previous encode task of this job points to this one: its offset_of_next_task_info is how far (in bytes, from the start of its task info block's fields) the next
    // block's fields are (Mesa: task_info_idx).
    if (w.last_encode_task_info) w.patch(w.last_encode_task_info, w.size() - w.last_encode_task_info + 3);
    w.last_encode_task_info = w.size();
  }
  ti.task_operation = static_cast<uint32_t>(operation);
  ti.reference_picture_dependency = dependency;
  ti.feedback_index = feedback_index;
  ti.video_bitstream_ring_index = ring_index;
  put_block(w, ti);
  w.end();
}

void write_feedback(TaskWriter& w, uint64_t feedback_gpu_address, uint32_t feedback_ring_size) {
  w.begin(kCmdFeedbackBuffer);   // feedback buffer
  w.put_address(feedback_gpu_address);   // feedbackRingAddressHi/Lo
  w.put(feedback_ring_size);
  w.end();
}

void write_create(TaskWriter& w, const EncoderSetup& setup, const PictureLayout& layout) {
  write_task_info(w, TaskOperation::Create, 0, 0, 0);
  const EncoderParameters& p = setup.parameters;
  w.begin(kCmdCreate);   // create cmd
  w.put(setup.ec.enc_use_circular_buffer);
  w.put(p.profile_idc);   // encProfile
  w.put(p.level);         // encLevel
  w.put(setup.ec.enc_pic_struct_restriction);
  w.put(p.width);         // encImageWidth
  w.put(p.height);        // encImageHeight
  w.put(layout.pitch);    // encRefPicLumaPitch
  w.put(layout.pitch);    // encRefPicChromaPitch
  w.put(layout.vpitch / 8);   // encRefYHeightInQw
  w.put(setup.addrmode_arraymode_disrdo_distwoinstants);
  w.put(setup.ec.enc_pre_encode_context_buffer_offset);
  w.put(setup.ec.enc_pre_encode_input_luma_buffer_offset);
  w.put(setup.ec.enc_pre_encode_input_chroma_buffer_offset);
  w.put(setup.ec.enc_pre_encode_mode_chromaflag_vbaqmode_scenechangesensitivity);
  w.end();
}

void write_destroy(TaskWriter& w, uint64_t feedback_gpu_address, uint32_t feedback_ring_size) {
  write_task_info(w, TaskOperation::Destroy, 0, 0, 0);
  write_feedback(w, feedback_gpu_address, feedback_ring_size);
  w.begin(kCmdDestroy);   // destroy
  w.end();
}

void write_config(TaskWriter& w, const EncoderSetup& setup) {
  write_task_info(w, TaskOperation::Config, 0, 0xffffffff, 0);
  w.begin(kCmdRateControl);   // rate control
  put_block(w, setup.rc);
  w.end();
  w.begin(kCmdConfigExtension);   // config extension
  w.put(setup.config_extension_perf_logging);
  w.end();
  w.begin(kCmdMotionEstimation);   // motion estimation
  put_block(w, setup.me);
  w.end();
  w.begin(kCmdRdo);   // rdo
  put_block(w, setup.rdo);
  w.end();
  w.begin(kCmdPicControl);   // pic control
  put_block(w, setup.pc);
  w.end();
}

void write_encode(TaskWriter& w, const EncoderSetup& setup, const PictureLayout& layout, const EncodeTask& t, uint32_t stream_handle) {
  (void)setup;
  write_session(w, stream_handle);
  write_task_info(w, TaskOperation::Encode, 0, 0, t.bitstream_index);

  w.begin(kCmdContextBuffer);   // context buffer
  w.put_address(t.cpb_gpu_address);   // encodeContextAddressHi/Lo
  w.end();

  w.begin(kCmdBitstreamBuffer);   // video bitstream buffer
  w.put_address(t.bitstream_gpu_address - uint64_t{t.bitstream_index} * t.bitstream_bytes);   // videoBitstreamRingAddressHi/Lo (with Mesa's bs_offset)
  w.put(t.bitstream_bytes);   // videoBitstreamRingSize
  w.end();

  w.begin(kCmdEncode);   // encode
  w.put(t.frame_num ? 0x0 : 0x11);   // insertHeaders
  w.put(0);   // picture_structure
  w.put(t.bitstream_bytes);   // allowedMaxBitstreamSize
  w.put(0);   // force_refresh_map
  w.put(0);   // insert_aud
  w.put(0);   // end_of_sequence
  w.put(0);   // end_of_stream
  w.put_address(t.input_luma_gpu_address);     // inputPictureLumaAddressHi/Lo
  w.put_address(t.input_chroma_gpu_address);   // inputPictureChromaAddressHi/Lo
  w.put(layout.vpitch);   // encInputFrameYPitch
  w.put(layout.pitch);    // encInputPicLumaPitch
  w.put(layout.pitch);    // encInputPicChromaPitch
  w.put(0x00010000);      // enc_input_pic_addr_array_disable2pipe_disablemboffload (not dual pipe)
  w.put(0);               // enc_input_pic_tile_config
  w.put(static_cast<uint32_t>(t.picture_type));   // encPicType
  w.put(t.picture_type == PictureType::IDR);      // encIdrFlag
  w.put(0);               // enc_idr_pic_id
  w.put(0);               // enc_mgs_key_pic
  w.put(!t.not_referenced);
  w.put(0);               // enc_temporal_layer_index
  w.put(0);               // num_ref_idx_active_override_flag
  w.put(0);               // num_ref_idx_l0_active_minus1
  w.put(0);               // num_ref_idx_l1_active_minus1

  // The first entry of the reference picture list modification: a P picture whose reference is not the previous frame says how far back it is.
  const int32_t distance = static_cast<int32_t>(t.frame_num - t.ref_idx_l0);
  if (distance > 1 && t.picture_type == PictureType::P) {
    w.put(0x00000001);   // enc_ref_list_modification_op
    w.put(static_cast<uint32_t>(distance - 1));   // enc_ref_list_modification_num
  } else {
    w.put(0);
    w.put(0);
  }
  for (int i = 0; i < 3; ++i) {
    w.put(0);   // enc_ref_list_modification_op
    w.put(0);   // enc_ref_list_modification_num
  }
  for (int i = 0; i < 4; ++i) {
    w.put(0);   // enc_decoded_picture_marking_op
    w.put(0);   // enc_decoded_picture_marking_num
    w.put(0);   // enc_decoded_picture_marking_idx
    w.put(0);   // enc_decoded_ref_base_picture_marking_op
    w.put(0);   // enc_decoded_ref_base_picture_marking_num
  }

  // encReferencePictureL0[0]
  w.put(0x00000000);   // pictureStructure
  if (t.picture_type == PictureType::P || t.picture_type == PictureType::B) {
    w.put(static_cast<uint32_t>(t.l0.type));
    w.put(t.l0.frame_num);
    w.put(t.l0.pic_order_cnt);
    w.put(t.l0.luma_offset);
    w.put(t.l0.chroma_offset);
  } else {
    w.put(0x00000000);   // l0_enc_pic_type
    w.put(0x00000000);   // l0_frame_number
    w.put(0x00000000);   // l0_picture_order_count
    w.put(0xffffffff);   // l0_luma_offset
    w.put(0xffffffff);   // l0_chroma_offset
  }
  // encReferencePictureL0[1]
  w.put(0x00000000);   // l0_picture_structure
  w.put(0x00000000);   // l0_enc_pic_type
  w.put(0x00000000);   // l0_frame_number
  w.put(0x00000000);   // l0_picture_order_count
  w.put(0xffffffff);   // l0_luma_offset
  w.put(0xffffffff);   // l0_chroma_offset
  // encReferencePictureL1[0]
  w.put(0x00000000);   // pictureStructure
  if (t.picture_type == PictureType::B) {
    w.put(static_cast<uint32_t>(t.l1.type));
    w.put(t.l1.frame_num);
    w.put(t.l1.pic_order_cnt);
    w.put(t.l1.luma_offset);
    w.put(t.l1.chroma_offset);
  } else {
    w.put(0x00000000);   // l1_enc_pic_type
    w.put(0x00000000);   // l1_frame_number
    w.put(0x00000000);   // l1_picture_order_count
    w.put(0xffffffff);   // l1_luma_offset
    w.put(0xffffffff);   // l1_chroma_offset
  }

  w.put(t.current_luma_offset);
  w.put(t.current_chroma_offset);
  w.put(0);   // enc_coloc_buffer_offset
  w.put(0);   // enc_reconstructed_ref_base_picture_luma_offset
  w.put(0);   // enc_reconstructed_ref_base_picture_chroma_offset
  w.put(0);   // enc_reference_ref_base_picture_luma_offset
  w.put(0);   // enc_reference_ref_base_picture_chroma_offset
  w.put(t.frame_num_cnt - 1);
  w.put(t.frame_num);
  w.put(t.pic_order_cnt);
  w.put(t.i_remain);
  w.put(t.p_remain);
  w.put(0);   // num_b_pic_remain_in_rcgop
  w.put(0);   // num_ir_pic_remain_in_rcgop
  w.put(0);   // enable_intra_refresh

  w.put(0);   // aq_variance_en
  w.put(0);   // aq_block_size
  w.put(0);   // aq_mb_variance_sel
  w.put(0);   // aq_frame_variance_sel
  w.put(0);   // aq_param_a
  w.put(0);   // aq_param_b
  w.put(0);   // aq_param_c
  w.put(0);   // aq_param_d
  w.put(0);   // aq_param_e

  w.put(0);   // context_in_sfb
  w.end();

  write_feedback(w, t.feedback_gpu_address, setup.feedback_ring_size);
}

Feedback read_feedback(ArrayView<const uint32_t> dwords) {
  Feedback f;
  if (dwords.size() < 10) return f;
  // rvce_get_feedback: the size is encBitstreamSize less the offset of the bitstream in its buffer, when the status says there is a bitstream.
  f.has_bitstream = dwords[1] != 0;
  f.bytes = f.has_bitstream && dwords[4] >= dwords[9] ? dwords[4] - dwords[9] : 0;
  return f;
}

}  // namespace amd_vce3
}  // namespace hardware
