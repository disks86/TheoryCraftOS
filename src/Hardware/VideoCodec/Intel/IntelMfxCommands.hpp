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
namespace intel_mfx {

// Command streams for the video (MFX) engine of Intel GPUs, Gen8 and Gen9/9.5 layout (Broadwell through Coffee/Comet Lake graphics). These are pure
// encoders: parameters in (plain numbers and graphics addresses), command dwords out. Nothing here touches hardware; it is the part of the driver that
// can be checked against the documented layouts. Layouts follow the MIT-licensed intel-vaapi-driver (gen8_mfd.c) and the Intel programming reference.

// Writes dwords into a fixed buffer. Running out of room is remembered, not fatal: check ok() once at the end.
class DwordWriter {
 public:
  explicit DwordWriter(ArrayView<uint32_t> out) : out_(out) {}
  void emit(uint32_t dword) {
    if (count_ >= out_.size()) { overflow_ = true; return; }
    out_[count_++] = dword;
  }
  // A 64-bit graphics address as two dwords, low first.
  void emit_address(uint64_t address) {
    emit(static_cast<uint32_t>(address));
    emit(static_cast<uint32_t>(address >> 32));
  }
  uint64_t count() const { return count_; }
  bool ok() const { return !overflow_; }

 private:
  ArrayView<uint32_t> out_;
  uint64_t count_ = 0;
  bool overflow_ = false;
};

// --- Command headers (3 << 29 | pipeline << 27 | opcode << 24 | sub-opcode A << 21 | sub-opcode B << 16, then the length in dwords minus 2).
constexpr uint32_t mfx_header(uint32_t pipeline, uint32_t opcode, uint32_t sub_a, uint32_t sub_b) {
  return (3u << 29) | (pipeline << 27) | (opcode << 24) | (sub_a << 21) | (sub_b << 16);
}
constexpr uint32_t kMfxPipeModeSelect = mfx_header(2, 0, 0, 0);
constexpr uint32_t kMfxSurfaceState = mfx_header(2, 0, 0, 1);
constexpr uint32_t kMfxPipeBufAddrState = mfx_header(2, 0, 0, 2);
constexpr uint32_t kMfxIndObjBaseAddrState = mfx_header(2, 0, 0, 3);
constexpr uint32_t kMfxBspBufBaseAddrState = mfx_header(2, 0, 0, 4);
constexpr uint32_t kMfxQmState = mfx_header(2, 0, 0, 7);
constexpr uint32_t kMfxAvcImgState = mfx_header(2, 1, 0, 0);
constexpr uint32_t kMfxAvcDirectmodeState = mfx_header(2, 1, 0, 2);
constexpr uint32_t kMfxAvcSliceState = mfx_header(2, 1, 0, 3);
constexpr uint32_t kMfxAvcRefIdxState = mfx_header(2, 1, 0, 4);
constexpr uint32_t kMfxAvcWeightoffsetState = mfx_header(2, 1, 0, 5);
constexpr uint32_t kMfdAvcPicidState = mfx_header(2, 1, 1, 5);
constexpr uint32_t kMfdAvcBsdObject = mfx_header(2, 1, 1, 8);
constexpr uint32_t kMfxVc1PredPipeState = mfx_header(2, 2, 0, 1);
constexpr uint32_t kMfxVc1DirectmodeState = mfx_header(2, 2, 0, 2);
constexpr uint32_t kMfdVc1LongPicState = mfx_header(2, 2, 1, 1);
constexpr uint32_t kMfdVc1BsdObject = mfx_header(2, 2, 1, 8);
constexpr uint32_t kMfxMpeg2PicState = mfx_header(2, 3, 0, 0);
constexpr uint32_t kMfdMpeg2BsdObject = mfx_header(2, 3, 1, 8);

// The HCP (HEVC and VP9) pipeline's commands: pipeline 2, opcode 7, the sub-opcode in bits 16-23.
constexpr uint32_t hcp_header(uint32_t command) { return (3u << 29) | (2u << 27) | (7u << 23) | (command << 16); }
constexpr uint32_t kHcpPipeModeSelect = hcp_header(0x00);
constexpr uint32_t kHcpSurfaceState = hcp_header(0x01);
constexpr uint32_t kHcpPipeBufAddrState = hcp_header(0x02);
constexpr uint32_t kHcpIndObjBaseAddrState = hcp_header(0x03);
constexpr uint32_t kHcpQmState = hcp_header(0x04);
constexpr uint32_t kHcpPicState = hcp_header(0x10);
constexpr uint32_t kHcpTileState = hcp_header(0x11);
constexpr uint32_t kHcpRefIdxState = hcp_header(0x12);
constexpr uint32_t kHcpWeightoffset = hcp_header(0x13);
constexpr uint32_t kHcpSliceState = hcp_header(0x14);
constexpr uint32_t kHcpBsdObject = hcp_header(0x20);
constexpr uint32_t kHcpVp9PicState = hcp_header(0x30);
constexpr uint32_t kHcpVp9SegmentState = hcp_header(0x32);
constexpr uint32_t kHcpCodecHevc = 0, kHcpCodecVp9 = 1;

// MI commands for the video ring.
constexpr uint32_t kMiNoop = 0;
constexpr uint32_t kMiBatchBufferEnd = 0x0Au << 23;
constexpr uint32_t kMiFlushDw = (0x26u << 23) | 0x2;
constexpr uint32_t kMiFlushDwVideoPipelineCacheInvalidate = 1u << 7;

constexpr uint32_t kFormatMpeg2 = 0, kFormatVc1 = 1, kFormatAvc = 2;   // MFX_FORMAT_*
constexpr uint32_t kSurfacePlanar420 = 4;  // MFX_SURFACE_PLANAR_420_8
constexpr uint32_t kQmMpegIntra = 0, kQmMpegNonIntra = 1;
constexpr uint32_t kQmAvc4x4Intra = 0, kQmAvc4x4Inter = 1, kQmAvc8x8Intra = 2, kQmAvc8x8Inter = 3;
constexpr uint32_t kMaxAvcReferenceFrames = 16;
constexpr uint32_t kMaxAvcRefIdx = 32;

// What a decode batch needs of the H.264 picture, in the terms of the commands.
struct AvcPicture {
  uint32_t width_in_mbs = 0, height_in_mbs = 0;  // frame size in macroblocks
  int32_t chroma_qp_index_offset = 0, second_chroma_qp_index_offset = 0;
  bool weighted_pred = false;
  uint32_t weighted_bipred_idc = 0;
  uint32_t chroma_format_idc = 1;     // 1: 4:2:0 (0, monochrome, is not produced by this OS's parser)
  bool entropy_coding_cabac = false;
  bool reference_picture = true;      // nal_ref_idc != 0
  bool constrained_intra_pred = false;
  bool direct_8x8_inference = false;
  bool transform_8x8_mode = false;
  bool frame_mbs_only = true;
  // Pictures of this encoder are frames; the field and MBAFF forms are carried in the command but the stream parser refuses them.
  bool mbaff_frame = false;
  bool field_pic = false;
  bool bottom_field = false;
};

struct AvcSlice {
  uint32_t slice_type = 2;            // 0 P, 1 B, 2 I (the H.264 slice_type % 5 values)
  uint32_t first_mb = 0;
  uint32_t next_first_mb = 0;         // first macroblock of the next slice; ignored when last_slice
  bool last_slice = true;
  uint32_t num_ref_idx_l0 = 0, num_ref_idx_l1 = 0;
  uint32_t luma_log2_weight_denom = 0, chroma_log2_weight_denom = 0;
  bool direct_spatial_mv_pred = false;
  uint32_t disable_deblocking_filter_idc = 0;
  uint32_t cabac_init_idc = 0;
  int32_t slice_qp = 26;
  int32_t slice_alpha_c0_offset_div2 = 0, slice_beta_offset_div2 = 0;
  // The slice's bytes in the bitstream buffer (as stored, emulation prevention bytes included), and where its macroblock data begins.
  uint32_t data_size = 0;             // bytes of the slice data to hand to the engine
  uint32_t data_offset = 0;           // of the slice in the bitstream buffer
  uint32_t first_mb_bit_offset = 0;   // bit offset of the macroblock data from data_offset
};

// Graphics addresses of every buffer a picture uses (0 means "none"). The other codecs of the MFX engine (MPEG-2, VC-1) use the same set. The engine reads and writes them through the graphics page tables.
struct AvcBuffers {
  uint64_t current_picture = 0;            // decoded output
  uint64_t intra_row_store = 0;            // width_in_mbs * 64 bytes
  uint64_t deblocking_row_store = 0;       // width_in_mbs * 64 * 4
  uint64_t bsd_mpc_row_store = 0;          // width_in_mbs * 64 * 2
  uint64_t mpr_row_store = 0;              // width_in_mbs * 64 * 2
  uint64_t reference_pictures[kMaxAvcReferenceFrames] = {};
  uint64_t direct_mv_current = 0;          // width_in_mbs * height_in_mbs * 128 bytes per picture
  uint64_t direct_mv_references[kMaxAvcReferenceFrames] = {};
  int32_t reference_poc[kMaxAvcReferenceFrames][2] = {};  // top, bottom
  int32_t current_poc[2] = {};
  uint64_t bitstream = 0;                  // the slice data buffer
  uint64_t bitplane_read = 0;              // VC-1: the bit plane buffer
  bool deblocking_enabled = true;          // some slice has the loop filter on: output goes through the post-deblocking path
  uint32_t mocs = 2;                       // memory object control state dword (Gen9: use the page table's cache setting)
};

// The surface layout the engine wants: NV12, Y-tiled, pitch a multiple of 128 bytes, rows a multiple of 32. The chroma plane starts `chroma_offset_rows` rows below the luma.
struct Nv12TiledLayout {
  uint32_t pitch = 0;              // bytes per row
  uint32_t aligned_height = 0;     // luma rows
  uint32_t chroma_offset_rows = 0;
  uint64_t size = 0;               // bytes of the whole surface (a multiple of the 4096-byte tile)
};
// False for a size the layout cannot represent (zero, or beyond 16384 in either direction).
bool nv12_tiled_layout(uint32_t width, uint32_t height, Nv12TiledLayout* out);

// --- The commands. Each appends its dwords to `w`.
void emit_mi_flush(DwordWriter& w);             // video pipeline cache invalidate flush
void emit_batch_end(DwordWriter& w);            // MI_BATCH_BUFFER_END, with an MI_NOOP in front when the count so far is even, so the batch ends on a quadword
void emit_pipe_mode_select(DwordWriter& w, bool post_deblocking_output, bool pre_deblocking_output);   // (H.264)
void emit_pipe_mode_select(DwordWriter& w, uint32_t format, bool post_deblocking_output, bool pre_deblocking_output);
void emit_surface_state(DwordWriter& w, uint32_t width, uint32_t height, const Nv12TiledLayout& layout);
void emit_pipe_buf_addr_state(DwordWriter& w, const AvcBuffers& b);
void emit_ind_obj_base_addr_state(DwordWriter& w, uint64_t bitstream, uint32_t mocs);
void emit_bsp_buf_base_addr_state(DwordWriter& w, const AvcBuffers& b);
// 4x4 lists are 3 x 16 bytes per matrix, 8x8 lists 64 bytes per matrix (raster order, as the stream's scaling lists after the zig-zag undo).
void emit_qm_state(DwordWriter& w, uint32_t matrix_type, ConstByteView matrix);
void emit_avc_picid_state(DwordWriter& w);       // picture ID remapping off
void emit_avc_img_state(DwordWriter& w, const AvcPicture& p);
void emit_avc_directmode_state(DwordWriter& w, const AvcBuffers& b);
// `entries` are the encoded reference entries (see avc_ref_idx_entry); the unused ones must be 0xFF.
void emit_avc_ref_idx_state(DwordWriter& w, uint32_t list, const uint8_t entries[kMaxAvcRefIdx]);
// One entry of an MFX_AVC_REF_IDX_STATE list for a frame (not a field) in frame store `frame_store_id` (0-15).
constexpr uint8_t avc_ref_idx_entry(uint32_t frame_store_id, bool long_term) {
  return static_cast<uint8_t>((long_term ? 1u << 6 : 0u) | (1u << 5) | (frame_store_id << 1));
}
// Weights and offsets of one list: 32 entries of luma weight, luma offset, Cb weight, Cb offset, Cr weight, Cr offset.
void emit_avc_weightoffset_state(DwordWriter& w, uint32_t list, const int16_t table[kMaxAvcRefIdx * 6]);
void emit_avc_slice_state(DwordWriter& w, const AvcPicture& p, const AvcSlice& s);
void emit_avc_bsd_object(DwordWriter& w, const AvcSlice& s);
// A slice that covers macroblocks no real slice does (the picture's first slice does not start at 0): the engine needs it to fill the gap.
void emit_avc_phantom_slice(DwordWriter& w, const AvcPicture& p, bool has_next, uint32_t next_first_mb);

// --- MPEG-2 (Gen8 and Gen9 layouts of the reference driver's gen8_mfd.c).

// What a picture needs of the MPEG-2 picture header: the picture coding extension's fields.
struct Mpeg2Picture {
  uint32_t width_in_mbs = 0, height_in_mbs = 0;   // of the frame
  uint32_t picture_coding_type = 1;               // 1 I, 2 P, 3 B
  uint32_t f_code[2][2] = {{15, 15}, {15, 15}};
  uint32_t intra_dc_precision = 0;
  uint32_t picture_structure = 3;                 // 1 top field, 2 bottom field, 3 frame
  bool top_field_first = false, frame_pred_frame_dct = false, concealment_motion_vectors = false, q_scale_type = false, intra_vlc_format = false, alternate_scan = false;
};
// One slice. `data_offset` is where the byte holding the slice's first macroblock bit is in the bitstream buffer; `data_size` the bytes from there to the end of the slice.
struct Mpeg2SliceJob {
  uint32_t data_offset = 0, data_size = 0;
  uint32_t bit_offset = 0;                        // of the first macroblock within that byte (0-7)
  uint32_t hpos = 0, vpos = 0;                    // first macroblock (the row within the field for a field picture)
  uint32_t next_hpos = 0, next_vpos = 0;          // of the next slice, or just below the picture for the last
  uint32_t mb_count = 0;
  bool last = false;
  uint32_t quantiser_scale_code = 1;
};
void emit_mpeg2_pic_state(DwordWriter& w, const Mpeg2Picture& p);
void emit_mpeg2_bsd_object(DwordWriter& w, const Mpeg2SliceJob& s);

// --- VC-1. The engine reads no picture header: everything the header says is in the picture state.

struct Vc1Picture {
  uint32_t width_in_mbs = 0, height_in_mbs = 0;
  bool direct_mv_valid = false;          // B picture: the backward reference was a P picture (its direct motion vector buffer is there)
  bool implicit_quantizer = false;
  uint32_t rounding_control = 0;
  bool sync_marker = false;
  uint32_t interpolation_mode = 0;       // 8: bilinear half sample, plus the fast chroma bit
  bool range_reduction = false, range_reduction_scale = false;
  bool loop_filter = false, overlap = false, advanced = false;
  uint32_t conditional_overlap = 0;      // 0 none, 2 all block boundaries, 3 selected by the bit plane
  uint32_t picture_type = 0;             // 0 I, 1 P, 2 B, 3 BI, 4 skipped
  uint32_t alt_pq = 0, pic_quantizer_scale = 0, scale_factor = 0;
  uint32_t unified_mv_mode = 0;          // 1 one MV, 2 half sample, 3 half sample bilinear, 0 mixed
  bool fast_uvmc = false;
  uint32_t extended_mv_range = 0;
  uint32_t alt_pquant_edge_mask = 0, alt_pquant_config = 0;
  bool half_qp = false, pic_quantizer_type = false;
  bool bitplane_present = false;
  bool raw_mv_type = false, raw_skip = false, raw_direct = false, raw_overflags = false, raw_ac_pred = false;
  uint32_t mv_table = 0;
  uint32_t frame_level_transform_type = 0;   // 0 8x8, 1 8x4, 2 4x8, 3 4x4
  bool mb_level_transform_type = true;
  uint32_t trans_ac_y = 0, transform_ac_codingset_idx1 = 0, intra_transform_dc_table = 0, cbp_table = 0;
};
void emit_vc1_pic_state(DwordWriter& w, const Vc1Picture& p);
// Intensity compensation of the forward reference (single, not the interlaced double form); progressive pictures only.
void emit_vc1_pred_pipe_state(DwordWriter& w, bool compensate_forward, uint32_t luma_scale, uint32_t luma_shift);
void emit_vc1_directmode_state(DwordWriter& w, uint64_t write_buffer, uint64_t read_buffer, uint32_t mocs);
// One unit of the picture (the frame, then each slice): `data_offset` is the byte holding its first macroblock bit in the bitstream buffer, `data_size` the bytes from there to
// its end, `bit_offset` (0-7) the first macroblock bit in that byte.
struct Vc1BsdJob {
  uint32_t data_offset = 0, data_size = 0, bit_offset = 0;
  uint32_t first_row = 0, next_row = 0;
};
void emit_vc1_bsd_object(DwordWriter& w, const Vc1BsdJob& job);

// --- HEVC (the HCP pipeline, Gen9 layouts of the reference driver's gen9_mfd.c). 8-bit 4:2:0 only.

constexpr uint32_t kMaxHevcReferenceFrames = 8;   // the engine's frame store: the pictures one picture's slices can refer to
constexpr uint32_t kMaxHevcRefIdx = 15;

struct HevcPicture {
  uint32_t width = 0, height = 0;                          // luma samples, as coded (a multiple of the minimum coding block)
  uint32_t log2_min_cb = 3, log2_ctb = 4, log2_min_tb = 2, log2_max_tb = 5;
  bool pcm = false, pcm_loop_filter_disabled = false;
  uint32_t log2_min_pcm = 3, log2_max_pcm = 3, pcm_bit_depth_luma = 8, pcm_bit_depth_chroma = 8;
  bool strong_intra_smoothing = false, transquant_bypass = false, amp = false, transform_skip = false;
  bool weighted_pred = false, weighted_bipred = false, sign_data_hiding = false, constrained_intra_pred = false, cu_qp_delta = false, sao = false;
  bool tiles = false, entropy_coding_sync = false, loop_filter_across_tiles = true;
  uint32_t log2_parallel_merge_level = 2, diff_cu_qp_delta_depth = 0;
  int32_t cb_qp_offset = 0, cr_qp_offset = 0;
  uint32_t max_transform_hierarchy_depth_inter = 0, max_transform_hierarchy_depth_intra = 0;
  uint32_t num_tile_columns = 1, num_tile_rows = 1;
  uint32_t column_width[20] = {}, row_height[22] = {};     // in coding tree blocks (the last included)
};

// Graphics addresses of what a picture uses (0: none). The line and column buffers the engine keeps state in are sized by hevc_buffer_bytes().
struct HevcBuffers {
  uint64_t current_picture = 0;
  uint64_t deblocking_line = 0, deblocking_tile_line = 0, deblocking_tile_column = 0;
  uint64_t metadata_line = 0, metadata_tile_line = 0, metadata_tile_column = 0;
  uint64_t sao_line = 0, sao_tile_line = 0, sao_tile_column = 0;
  uint64_t motion_vector_current = 0;                      // collocated motion vectors this picture leaves
  uint64_t reference_pictures[kMaxHevcReferenceFrames] = {};
  uint64_t motion_vector_references[kMaxHevcReferenceFrames] = {};
  uint64_t bitstream = 0;
  uint64_t bitstream_bytes = 0;
  uint32_t mocs = 2;
};

// The sizes of the buffers an 8-bit picture of `width` x `height` luma samples with coding tree blocks of 1 << log2_ctb needs (the reference driver's formulas).
struct HevcBufferBytes {
  uint64_t deblocking_line = 0, deblocking_tile_line = 0, deblocking_tile_column = 0;
  uint64_t metadata_line = 0, metadata_tile_line = 0, metadata_tile_column = 0;
  uint64_t sao_line = 0, sao_tile_line = 0, sao_tile_column = 0;
  uint64_t motion_vector = 0;                              // per picture
};
HevcBufferBytes hevc_buffer_bytes(uint32_t width, uint32_t height, uint32_t log2_ctb);

// Scaling lists in the engine's terms: 4x4 (6 x 16), 8x8, 16x16, 32x32 (two: intra, inter) as raster-order bytes, and the DC values of the big ones.
struct HevcScaling {
  uint8_t list4x4[6][16] = {}, list8x8[6][64] = {}, list16x16[6][64] = {}, list32x32[2][64] = {};
  uint8_t dc16x16[6] = {}, dc32x32[2] = {};
};

// One slice segment.
struct HevcSlice {
  uint32_t hor_pos = 0, ver_pos = 0, next_hor_pos = 0, next_ver_pos = 0;   // first coding tree block of this and of the next segment (0, 0 for the last)
  bool last = false;
  uint32_t slice_type = 2;                                 // 0 B, 1 P, 2 I
  int32_t qp = 26, cb_qp_offset = 0, cr_qp_offset = 0;
  bool temporal_mvp = false, dependent = false, cabac_init = false, mvd_l1_zero = false, collocated_from_l0 = true, low_delay = false;
  bool sao_luma = false, sao_chroma = false, loop_filter_across_slices = false, deblocking_disabled = false;
  uint32_t collocated_frame_store = 0;
  uint32_t max_merge_candidates = 5;
  uint32_t luma_log2_weight_denom = 0, chroma_log2_weight_denom = 0;
  int32_t beta_offset_div2 = 0, tc_offset_div2 = 0;
  uint32_t header_bytes = 0;                               // from the start of the NAL unit (as stored, emulation prevention bytes included) to the slice segment data
  uint32_t data_offset = 0, data_size = 0;                 // the NAL unit in the bitstream buffer
};

// Prediction weights of one list, as the slice header codes them (the weight minus 1 << denominator, the offsets).
struct HevcWeights {
  int8_t luma_weight[kMaxHevcRefIdx] = {}, luma_offset[kMaxHevcRefIdx] = {};
  int8_t chroma_weight[kMaxHevcRefIdx][2] = {}, chroma_offset[kMaxHevcRefIdx][2] = {};
};

// An entry of a reference index list: the frame store the picture is in, whether it is a long-term picture and the order count difference (the current minus the reference).
constexpr uint32_t hevc_ref_idx_entry(uint32_t frame_store, bool long_term, int32_t poc_delta) {
  const int32_t d = poc_delta < -128 ? -128 : poc_delta > 127 ? 127 : poc_delta;
  return (1u << 15) | (long_term ? 1u << 13 : 0u) | (frame_store << 8) | (static_cast<uint32_t>(d) & 0xFF);
}

void emit_hcp_pipe_mode_select(DwordWriter& w, uint32_t codec);
// `id` 0 is the picture being decoded; VP9 describes its three reference pictures as 2 to 4.
void emit_hcp_surface_state(DwordWriter& w, const Nv12TiledLayout& layout, uint32_t id = 0);
void emit_hevc_pipe_buf_addr_state(DwordWriter& w, const HevcBuffers& b);
void emit_hevc_ind_obj_base_addr_state(DwordWriter& w, const HevcBuffers& b);
void emit_hevc_qm_states(DwordWriter& w, const HevcScaling& s);
void emit_hevc_pic_state(DwordWriter& w, const HevcPicture& p);
void emit_hevc_tile_state(DwordWriter& w, const HevcPicture& p);
// `entries` are hevc_ref_idx_entry() values; only the first min(count, 15) are used.
void emit_hevc_ref_idx_state(DwordWriter& w, uint32_t list, uint32_t count, const uint32_t entries[kMaxHevcRefIdx]);
void emit_hevc_weightoffset_state(DwordWriter& w, uint32_t list, uint32_t count, const HevcWeights& weights);
void emit_hevc_slice_state(DwordWriter& w, const HevcSlice& s);
void emit_hevc_bsd_object(DwordWriter& w, const HevcSlice& s);

// --- VP9 (the HCP pipeline, Gen9.5 layouts of the reference driver's gen9_mfd.c). Profile 0 only: 8-bit 4:2:0.

// Graphics addresses of what a frame uses (0: none).
struct Vp9Buffers {
  uint64_t current_picture = 0;
  uint64_t reference_pictures[3] = {};                     // last, golden, alt-ref
  uint64_t deblocking_line = 0, deblocking_tile_line = 0, deblocking_tile_column = 0;
  uint64_t metadata_line = 0, metadata_tile_line = 0, metadata_tile_column = 0;
  uint64_t motion_vector_current = 0, motion_vector_last = 0;
  uint64_t probability = 0, segment_id = 0, hvd_line = 0, hvd_tile = 0;
  uint64_t bitstream = 0, bitstream_bytes = 0;
  uint32_t mocs = 2;
};

struct Vp9BufferBytes {
  uint64_t deblocking_line = 0, deblocking_tile_line = 0, deblocking_tile_column = 0;
  uint64_t metadata_line = 0, metadata_tile_line = 0, metadata_tile_column = 0;
  uint64_t hvd_line = 0, hvd_tile = 0;
  uint64_t probability = 0, segment_id = 0, motion_vector = 0;
};
// For a frame of `width` x `height` luma samples (the reference driver's formulas, in superblocks of 64).
Vp9BufferBytes vp9_buffer_bytes(uint32_t width, uint32_t height);

struct Vp9Picture {
  uint32_t width = 0, height = 0;                          // luma samples, rounded up to 8
  bool segment_id_in = false, segment_id_out = false, lossless = false, segmentation_temporal_update = false, segmentation_enabled = false;
  uint32_t sharpness_level = 0, filter_level = 0;
  bool frame_parallel = false, error_resilient = false, refresh_frame_context = false;
  uint32_t last_frame_type = 0;                            // 0 key frame, 1 not
  bool use_prev_frame_mvs = false;
  bool alt_ref_sign_bias = false, golden_ref_sign_bias = false, last_ref_sign_bias = false;
  uint32_t interp_filter = 0;                              // libvpx's numbering: 0 regular, 1 smooth, 2 sharp, 3 bilinear, 4 switchable
  bool allow_high_precision_mv = false, intra_only = false, adapt_probabilities = false;
  uint32_t frame_type = 0;                                 // 0 key frame, 1 inter frame
  uint32_t log2_tile_columns = 0, log2_tile_rows = 0;
  bool reference_present[3] = {false, false, false};
  uint32_t reference_width[3] = {}, reference_height[3] = {};   // of the reference pictures, as coded
  uint32_t frame_width = 0, frame_height = 0;              // as coded (not rounded)
  uint32_t first_partition_size = 0, header_bytes = 0;     // the compressed header, the uncompressed header
};

struct Vp9Segment {
  uint32_t id = 0;
  bool reference_enabled = false, reference_skipped = false;
  uint32_t reference = 0;
  uint8_t filter_level[4][2] = {};
  uint32_t luma_dc = 0, luma_ac = 0, chroma_dc = 0, chroma_ac = 0;
};

void emit_vp9_pipe_buf_addr_state(DwordWriter& w, const Vp9Buffers& b);
void emit_hcp_ind_obj_base_addr_state(DwordWriter& w, uint64_t bitstream, uint64_t bitstream_bytes, uint32_t mocs);
void emit_vp9_pic_state(DwordWriter& w, const Vp9Picture& p);
void emit_vp9_segment_state(DwordWriter& w, const Vp9Segment& s, bool filter_enabled, bool intra_frame);
// The frame's compressed header and tiles: `data_size` bytes at `data_offset` of the bitstream buffer.
void emit_vp9_bsd_object(DwordWriter& w, uint32_t data_size, uint32_t data_offset);

}  // namespace intel_mfx
}  // namespace hardware
