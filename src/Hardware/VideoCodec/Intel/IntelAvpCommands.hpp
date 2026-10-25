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
#include "Hardware/VideoCodec/Intel/IntelMfxCommands.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace intel_avp {

// The AV1 decode pipeline (AVP) of the Gen12 (Xe-LP) video engine. Pure encoders, like IntelMfxCommands: parameters in (plain numbers and graphics addresses), command dwords
// out. The layouts follow the MIT-licensed Intel media driver (iHD: mhw_vdbox_avp_g12_X, decode_av1_picture_packet) as it is known from its source; nothing here has been
// run on an engine.
using intel_mfx::DwordWriter;

// AVP commands: pipeline 2, opcode 3, the command number in bits 16-22.
constexpr uint32_t avp_header(uint32_t command) { return (3u << 29) | (2u << 27) | (3u << 23) | (command << 16); }
constexpr uint32_t kAvpPipeModeSelect = avp_header(0);
constexpr uint32_t kAvpSurfaceState = avp_header(1);
constexpr uint32_t kAvpPipeBufAddrState = avp_header(2);
constexpr uint32_t kAvpIndObjBaseAddrState = avp_header(3);
constexpr uint32_t kAvpInterPredState = avp_header(18);
constexpr uint32_t kAvpTileCoding = avp_header(21);
constexpr uint32_t kAvpBsdObject = avp_header(32);
constexpr uint32_t kAvpPicState = avp_header(48);
constexpr uint32_t kAvpSegmentState = avp_header(50);
constexpr uint32_t kAvpInloopFilterState = avp_header(51);
// MFX_WAIT with its sync flag (a single dword), and VD_PIPELINE_FLUSH (two dwords).
constexpr uint32_t kMfxWait = 0x68000100;
constexpr uint32_t kVdPipelineFlush = (3u << 29) | (2u << 27) | (15u << 23);

// The lengths of the commands in dwords.
constexpr uint32_t kPipeModeSelectDwords = 6, kSurfaceStateDwords = 5, kPipeBufAddrDwords = 188, kIndObjDwords = 6, kPicStateDwords = 51, kSegmentStateDwords = 4;
constexpr uint32_t kTileCodingDwords = 6, kBsdObjectDwords = 3, kInloopFilterDwords = 15, kInterPredDwords = 15;

// Surface ids of AVP_SURFACE_STATE.
constexpr uint32_t kSurfaceRecon = 0, kSurfaceIntra = 6, kSurfaceLast = 7, kSurfaceIntrabc = 14;

// The AV1 reference frame names; the pictures an inter frame predicts from are LAST .. ALTREF, INTRA stands for the picture itself.
constexpr uint32_t kIntraFrame = 0, kLastFrame = 1, kAltrefFrame = 7;

// The engine's line and column buffers (iHD: MhwVdboxAvpInternalBufferType), in the order the frame's workspace holds them.
enum Av1Line : uint32_t {
  kBsdLine, kBsdTileLine, kIntraPredLine, kIntraPredTileLine, kSpatialMvLine, kSpatialMvTileLine, kLrMetaTileColumn, kLrTileLineY, kLrTileLineU, kLrTileLineV,
  kDeblockLineY, kDeblockLineU, kDeblockLineV, kDeblockTileLineY, kDeblockTileLineV, kDeblockTileLineU, kDeblockTileColumnY, kDeblockTileColumnU, kDeblockTileColumnV,
  kCdefLine, kCdefTileLine, kCdefTileColumn, kCdefMetaTileLine, kCdefMetaTileColumn, kCdefTopLeft, kSuperResTileColumnY, kSuperResTileColumnU, kSuperResTileColumnV,
  kLrTileColumnY, kLrTileColumnU, kLrTileColumnV, kFrameStatus, kBlockStreamout,
  kLineCount
};

// How big a frame's buffers are. 8-bit 4:2:0.
struct Av1Dimensions {
  uint32_t sb_wide = 0, sb_high = 0;      // the frame in superblocks
  bool sb128 = false;
  uint32_t tile_columns = 1, tiles = 1;
};
struct Av1BufferBytes {
  uint64_t line[kLineCount] = {};
  uint64_t segment_id = 0, motion_vector = 0;
};
// False for dimensions that are zero or too big (the line buffers are made for tiles at most 4096 pixels wide).
bool av1_buffer_bytes(const Av1Dimensions& d, Av1BufferBytes* out);

// Where everything is, as graphics addresses (0: none).
struct AvpBuffers {
  uint64_t current = 0, intrabc = 0;
  uint64_t reference[8] = {};              // [0]: the picture itself (intra frame), [1..7]: LAST .. ALTREF
  uint64_t cdf_init = 0, cdf_backward = 0, segment_read = 0, segment_write = 0;
  uint64_t motion_vector_collocated[8] = {}, motion_vector_current = 0;
  uint64_t line[kLineCount] = {};
  uint32_t mocs = 0;                       // the memory-object control index (shifted into the attribute dword by the emitter)
};

struct AvpPicture {
  uint32_t width = 0, height = 0;          // the frame, in luma samples
  bool sb128 = false, order_hint_enabled = false;
  uint32_t order_hint_bits = 0;
  bool full_range = false, filter_intra = false, intra_edge_filter = false, dual_filter = false, inter_intra = false, masked_compound = false, joint_compound = false;
  bool screen_content_tools = false, force_integer_mv = false, warped_motion = false, cdef = false, restoration = false;
  uint32_t frame_type = 0;                 // 0 key, 1 inter, 2 intra-only, 3 switch
  bool error_resilient = false, intrabc = false;
  uint32_t primary_ref_frame = 7;
  bool segmentation_enabled = false, segmentation_update_map = false, segmentation_temporal_update = false, segmentation_preskip = false;
  uint32_t segmentation_last_active = 0;
  bool delta_q_present = false;
  uint32_t delta_q_res = 0;
  bool coded_lossless = false, segment_map_is_zero = false, segment_id_in = false, segment_id_out = false;
  uint32_t base_qindex = 0;
  int32_t y_dc_delta_q = 0, u_dc_delta_q = 0, u_ac_delta_q = 0, v_dc_delta_q = 0, v_ac_delta_q = 0;
  bool allow_high_precision_mv = false, reference_select = false;
  uint32_t interp_filter = 0;              // 0 regular, 1 smooth, 2 sharp, 3 bilinear, 4 switchable
  bool motion_mode_switchable = false, use_ref_frame_mvs = false;
  uint32_t sign_bias_mask = 0;             // bit r (1..7): reference r is a backward one
  uint32_t order_hint = 0;
  bool reduced_tx_set = false;
  uint32_t tx_mode = 0;
  bool skip_mode_present = false;
  uint32_t skip_mode_frame[2] = {};
  uint32_t ref_frame_side = 0;             // bit r (1..7)
  uint32_t global_motion_type[7] = {};
  bool global_motion_invalid[7] = {};
  uint32_t global_motion_words[21] = {};   // see av1_global_motion
  uint32_t reference_width[8] = {}, reference_height[8] = {};   // [0]: the frame itself; zero for an intra frame
  uint32_t reference_scale_vertical[8] = {}, reference_scale_horizontal[8] = {};
  uint32_t reference_order_hint[8] = {};   // [0]: the frame's own
};

// The three dwords AVP_PIC_STATE takes for a reference's global motion: the matrix of the frame header (the specification's gm_params) in the engine's form.
struct AvpGlobalMotion {
  uint32_t words[3] = {};
};
AvpGlobalMotion av1_global_motion(uint32_t type, const int32_t matrix[6], bool allow_high_precision_mv);

struct AvpSegment {
  uint32_t id = 0, feature_mask = 0;
  int32_t delta_q = 0;
  bool skip = false, global_mv = false, lossless = false;
  uint32_t qm_level_y = 15, qm_level_u = 15, qm_level_v = 15;
  int32_t delta_lf_y_vertical = 0, delta_lf_y_horizontal = 0, delta_lf_u = 0, delta_lf_v = 0;
  uint32_t reference_frame = 0;
};

struct AvpInloop {
  uint32_t level_y_vertical = 0, level_y_horizontal = 0, level_u = 0, level_v = 0, sharpness = 0;
  bool mode_ref_delta_enabled = false;
  uint32_t delta_lf_res = 0;
  bool delta_lf_multi = false, delta_lf_present = false;
  int32_t ref_delta[8] = {}, mode_delta[2] = {};
  uint32_t cdef_damping_minus3 = 0, cdef_bits = 0, cdef_y_strength[8] = {}, cdef_uv_strength[8] = {};
  uint32_t upscaled_width = 0;
  uint32_t restoration_type[3] = {};       // 0 none, 1 Wiener, 2 self-guided, 3 switchable
  uint32_t restoration_unit_size_luma = 0; // log2 of the size minus 5, 0 when there is no restoration
  bool restoration_same_unit_size_chroma = false;
};

struct AvpTile {
  uint32_t id = 0, group_num = 0, group_id = 0;
  uint32_t column_sb = 0, row_sb = 0, width_sb = 1, height_sb = 1;
  bool last_of_column = false, last_of_row = false, first_of_group = false, last_of_group = false, last_of_frame = false;
  bool disable_cdf_update = false, disable_frame_context_update = false;
  uint32_t columns = 1, rows = 1;
};

struct AvpInterPred {
  uint32_t saved_order_hint[7][7] = {};    // for each of LAST .. ALTREF: the order hints of the references it was decoded with
  uint32_t motion_field_mask = 0;          // bit r (0..6): LAST .. ALTREF take part in the motion field projection
};

void emit_mfx_wait(DwordWriter& w);
void emit_vd_pipeline_flush(DwordWriter& w);
void emit_avp_pipe_mode_select(DwordWriter& w);
void emit_avp_surface_state(DwordWriter& w, const intel_mfx::Nv12TiledLayout& layout, uint32_t id);
void emit_avp_pipe_buf_addr_state(DwordWriter& w, const AvpBuffers& b);
void emit_avp_ind_obj_base_addr_state(DwordWriter& w, uint64_t bitstream, uint64_t bitstream_bytes, uint32_t mocs);
void emit_avp_pic_state(DwordWriter& w, const AvpPicture& p);
void emit_avp_segment_state(DwordWriter& w, const AvpSegment& s);
void emit_avp_inloop_filter_state(DwordWriter& w, const AvpInloop& f);
void emit_avp_inter_pred_state(DwordWriter& w, const AvpInterPred& p);
void emit_avp_tile_coding(DwordWriter& w, const AvpTile& t);
void emit_avp_bsd_object(DwordWriter& w, uint32_t length, uint32_t offset);

}  // namespace intel_avp
}  // namespace hardware
