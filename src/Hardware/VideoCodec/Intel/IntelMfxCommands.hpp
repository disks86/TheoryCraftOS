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

// MI commands for the video ring.
constexpr uint32_t kMiNoop = 0;
constexpr uint32_t kMiBatchBufferEnd = 0x0Au << 23;
constexpr uint32_t kMiFlushDw = (0x26u << 23) | 0x2;
constexpr uint32_t kMiFlushDwVideoPipelineCacheInvalidate = 1u << 7;

constexpr uint32_t kFormatAvc = 2;       // MFX_FORMAT_AVC
constexpr uint32_t kSurfacePlanar420 = 4;  // MFX_SURFACE_PLANAR_420_8
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

// Graphics addresses of every buffer a picture uses (0 means "none"). The engine reads and writes them through the graphics page tables.
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
void emit_pipe_mode_select(DwordWriter& w, bool post_deblocking_output, bool pre_deblocking_output);
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

}  // namespace intel_mfx
}  // namespace hardware
