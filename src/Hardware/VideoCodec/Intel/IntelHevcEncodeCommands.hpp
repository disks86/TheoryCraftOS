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
#include "Hardware/VideoCodec/Intel/IntelEncodeCommands.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace intel_enc {

// H.265 encoding on the video box of Xe-LP (Gen12: Tiger Lake and later) in its low-power VDEnc mode: the VDEnc engine searches the motion and decides the coding tree, the HCP
// (PAK) engine transforms, quantises and codes the CABAC slice data; the driver describes the picture, the buffers and the rate (a fixed quantiser) and inserts the headers.
// The commands are the ones the Intel media driver's CodechalVdencHevcStateG12::ExecutePictureLevel / ExecuteSliceLevel send for one constant-QP slice of a frame without HuC
// firmware (target usage 7, no RDOQ, no SAO, no tiles, no weighted prediction); their bit layouts are generated from the driver's hardware command definitions
// (tools/gen_intel_encode_commands.py -> IntelEncodeCommandImagesGen12Hevc.inc). The stream structure is the one IntelHevcHeaders writes (64x64 coding tree blocks, 8x8 minimum
// coding blocks, cu_qp_delta on, SAO/AMP/PCM/TMVP off, one reference). A "P" picture is coded as a low-delay B picture whose two lists hold the previous picture, as the driver does.
// Ice Lake builds two of its VDEnc state commands with HuC firmware, which is not loaded here, so this class of encoder exists on Xe-LP only.

constexpr uint32_t kHcpSurfaceIdRecon = 0, kHcpSurfaceIdSource = 1, kHcpSurfaceIdReference = 5;
// HCP_ENC_BITSTREAM_BYTECOUNT_FRAME of the first video box: the size of the access unit the PAK has written.
constexpr uint32_t kHcpBitstreamByteCountRegisterGen12 = 0x1C28A0;

// The surfaces: the picture in whole coding tree blocks and its 4x and 8x down-scaled copies (the VDEnc engine writes those from the reconstruction and its motion search reads
// them), all Y-tiled NV12.
struct HevcEncodeLayout {
  uint32_t width = 0, height = 0;                 // the coded picture in luma samples (multiples of 8)
  uint32_t aligned_width = 0, aligned_height = 0; // ... rounded up to whole 64x64 blocks: the size of the surfaces
  uint32_t ds4_width = 0, ds4_height = 0, ds8_width = 0, ds8_height = 0;
  uint32_t ctbs_x = 0, ctbs_y = 0;
  Nv12TiledLayout full, ds4, ds8;
};
// False for a size the engine does not take (not a multiple of 8, under 16 or over 4096 samples a side).
bool hevc_encode_layout(uint32_t coded_width, uint32_t coded_height, HevcEncodeLayout* out);

// How the engine's row stores, statistics and streamout buffers are laid out in one scratch buffer (the media driver's allocation sizes), and the sizes of the buffers on their own.
struct HevcScratchPlan {
  uint32_t deblocking_line = 0, deblocking_tile_line = 0, deblocking_tile_column = 0;
  uint32_t metadata_line = 0, metadata_tile_line = 0, metadata_tile_column = 0;
  uint32_t sao_line = 0, sao_tile_line = 0, sao_tile_column = 0, sao_row_store = 0;
  uint32_t lcu_ildb = 0, lcu_base = 0, frame_statistics = 0, sse_row_store = 0, cu_streamout = 0;
  uint32_t vdenc_row_store = 0, vdenc_statistics = 0, vdenc_tile_row_store = 0, cu_count = 0;
  uint64_t total = 0;                             // bytes of the whole scratch buffer (offsets above are into it)
  uint64_t mv_temporal_bytes = 0;                 // one co-located motion vector buffer
  uint64_t pak_object_bytes = 0;                  // the buffer the VDEnc engine writes the LCU/CU commands to
  uint32_t pak_object_cu_offset = 0;              // where the coding unit records start in it
};
HevcScratchPlan hevc_scratch_plan(const HevcEncodeLayout& layout);

// The graphics addresses of everything a picture touches (0: not used).
struct HevcEncodeBuffers {
  uint32_t source = 0;
  uint32_t recon = 0, recon_ds4 = 0, recon_ds8 = 0, mv_temporal = 0;                  // what the engine writes for this picture
  uint32_t reference = 0, reference_ds4 = 0, reference_ds8 = 0, reference_mv_temporal = 0;   // what a P picture predicts from
  uint32_t bitstream = 0, bitstream_bytes = 0;
  uint32_t pak_objects = 0;
  uint32_t status = 0;                            // 4 bytes the bitstream size is stored to
  uint32_t deblocking_line = 0, deblocking_tile_line = 0, deblocking_tile_column = 0;
  uint32_t metadata_line = 0, metadata_tile_line = 0, metadata_tile_column = 0;
  uint32_t sao_line = 0, sao_tile_line = 0, sao_tile_column = 0, sao_row_store = 0;
  uint32_t lcu_ildb = 0, lcu_base = 0, frame_statistics = 0, sse_row_store = 0, cu_streamout = 0;
  uint32_t vdenc_row_store = 0, vdenc_statistics = 0, vdenc_tile_row_store = 0, cu_count = 0;
};
// Fills the scratch addresses of `b` from the plan and the address of the scratch buffer.
void hevc_resolve_scratch(const HevcScratchPlan& plan, uint32_t scratch_base, HevcEncodeBuffers* b);

struct HevcEncodePicture {
  bool p_picture = false;                         // a low-delay B picture predicting from the previous one, else I
  int32_t qp = 26;                                // the slice quantiser (10 .. 51; the driver's tables start at 10)
  int32_t pps_qp = 26;                            // init_qp of the picture parameter set (26 + init_qp_minus26)
  int32_t cb_qp_offset = 0, cr_qp_offset = 0;     // pps_cb_qp_offset, pps_cr_qp_offset
  uint32_t poc = 0, reference_poc = 0;            // picture order counts
  bool deblocking = true;
};

// Everything from the VDEnc initialisation to CMD2: the state a picture's slice runs in.
void emit_hevc_vdenc_picture(DwordWriter& w, const HevcEncodePicture& p, const HevcEncodeBuffers& b, const HevcEncodeLayout& layout);

// One header the PAK writes into the bitstream before the slice data (`bits` of `data`, packed from the first byte).
struct HevcHeaderInsert {
  ConstByteView data;
  uint32_t bits = 0;
  bool emulation_prevention = false;              // the engine inserts the emulation prevention bytes
  uint32_t skip_emulation_bytes = 0;              // ... after this many bytes (the start code and NAL unit header)
};
// The slice-level commands for the only slice of the picture: reference lists, slice state, the headers, the weights, the VDEnc walker and the VDEnc pipeline flush. `headers` are
// put in order; the last is the slice header.
bool emit_hevc_vdenc_slice(DwordWriter& w, const HevcEncodePicture& p, const HevcEncodeLayout& layout, ArrayView<const HevcHeaderInsert> headers);
// What follows the slice: flush the HCP pipeline, store the bitstream size, end the batch.
void emit_hevc_vdenc_finish(DwordWriter& w, const HevcEncodeBuffers& b);

}  // namespace intel_enc
}  // namespace hardware
