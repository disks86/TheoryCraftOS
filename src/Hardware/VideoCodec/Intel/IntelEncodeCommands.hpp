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
namespace intel_enc {

using intel_mfx::DwordWriter;
using intel_mfx::Nv12TiledLayout;

// H.264 encoding on the video box of Gen9.5 (Kaby Lake, Coffee Lake, Comet Lake), Gen11 (Ice Lake) and Gen12 (Xe-LP: Tiger Lake and later) in its low-power VDEnc mode: the fixed-function engine searches the motion, decides the
// macroblock modes and entropy-codes the slice data itself; the driver describes the picture, the buffers and the rate (here a fixed quantiser) and inserts the headers.
// The commands are the ones the Intel media driver's CodechalVdencAvcState::ExecutePictureLevel / ExecuteSliceLevel send for a constant-QP frame; their bit layouts are
// generated from the driver's hardware command definitions (tools/gen_intel_encode_commands.py) and the cost tables from its tables (tools/gen_intel_vdenc_tables.py).
// The MFX (PAK) side and the picture/slice state are the same on every generation; what changed is the size of four VDEnc commands (pipe mode select on Xe-LP, DS reference
// surface, pipe buffer addresses and walker from Ice Lake), the VDEnc control state Xe-LP starts a picture with, MFX_WAIT around the MFX pipe mode select (Gen11 and later), and
// where the frame's bitstream byte count register lives.

constexpr uint32_t kMocs = 2;              // memory object control state: the page table's cache setting (index 1)
constexpr uint32_t kMfxSurfaceRecon = 0, kMfxSurfaceSource = 4, kMfxSurfaceDownscaledRecon = 5;
constexpr uint32_t kMaxQp = 51;

enum class EncodeGeneration : uint8_t { Gen9, Gen11, Gen12 };
// MFC_BITSTREAM_BYTECOUNT_FRAME of the first video box: the size of the access unit the PAK has written.
constexpr uint32_t kBitstreamByteCountRegisterGen9 = 0x128A0, kBitstreamByteCountRegisterGen11 = 0x1C08A0;
constexpr uint32_t bitstream_byte_count_register(EncodeGeneration g) { return g == EncodeGeneration::Gen9 ? kBitstreamByteCountRegisterGen9 : kBitstreamByteCountRegisterGen11; }

// The surfaces: the picture in whole macroblocks and its 4x down-scaled copy (the VDEnc motion search runs on that), both Y-tiled NV12.
struct AvcEncodeLayout {
  uint32_t width = 0, height = 0;               // luma samples, whole macroblocks
  uint32_t quarter_width = 0, quarter_height = 0;
  Nv12TiledLayout full, quarter;
};
// False for a size the engine does not take (zero, more than 4096 samples a side, or too small for the down-scaling).
bool avc_encode_layout(uint32_t width_mbs, uint32_t height_mbs, AvcEncodeLayout* out);

// The graphics addresses of everything a picture touches (0: not used).
struct AvcEncodeBuffers {
  uint32_t source = 0;                          // the input picture
  uint32_t recon = 0, recon_ds4 = 0;            // what the engine writes: the reconstruction and its 4x down-scaled copy
  uint32_t reference = 0, reference_ds4 = 0;    // what a P picture predicts from
  uint32_t bitstream = 0, bitstream_bytes = 0;  // the access unit goes here
  uint32_t vdenc_row_store = 0, vdenc_statistics = 0, pak_statistics = 0;
  uint32_t intra_row_store = 0, deblocking_row_store = 0, bsd_mpc_row_store = 0, mpr_row_store = 0;
  uint32_t status = 0;                          // 4 bytes the bitstream size is stored to
};

// How much scratch each of the engine's row stores and statistics buffers needs for a picture of this many macroblocks (the media driver's allocation sizes, 64-byte lines).
struct AvcEncodeScratchSizes {
  uint32_t vdenc_row_store, vdenc_statistics, pak_statistics, intra_row_store, deblocking_row_store, bsd_mpc_row_store, mpr_row_store;
};
AvcEncodeScratchSizes avc_encode_scratch_sizes(uint32_t width_mbs, uint32_t height_mbs);

struct AvcEncodePicture {
  EncodeGeneration generation = EncodeGeneration::Gen9;
  uint32_t width_mbs = 0, height_mbs = 0;
  bool p_picture = false;                       // P (one reference), else I
  int32_t qp = 26;                              // the slice quantiser (0 .. 51)
  int32_t chroma_qp_offset = 0;
  bool deblocking = true;                       // the in-loop filter runs (disable_deblocking_filter_idc 0)
  int32_t alpha_c0_offset_div2 = 0, beta_offset_div2 = 0;
  uint32_t level_idc = 40;
  uint32_t reference_frame_store = 0;           // the frame store id the reference list entry names
  uint32_t log2_max_frame_num = 8;
  uint32_t last_in_stream = 0;
  bool xe_lpg = false;                          // Meteor Lake / Arrow Lake: the VDEnc commands of Xe_LPM_plus (Gen12 otherwise means Xe-LP)
  bool xe_hpg = false;                          // Arc Alchemist: the VDEnc commands of Xe_HPM
  uint32_t poc = 0, reference_poc = 0;          // picture order counts (low eight bits are what the VDEnc image state carries)
};

// Everything from the pipe mode select to the quantiser matrices: the state a picture's slices run in.
void emit_avc_vdenc_picture(DwordWriter& w, const AvcEncodePicture& p, const AvcEncodeBuffers& b, const AvcEncodeLayout& layout);

// One header the PAK writes into the bitstream before the slice data (`bits` of `data`, packed from the first byte).
struct AvcHeaderInsert {
  ConstByteView data;
  uint32_t bits = 0;
  bool emulation_prevention = false;            // the engine inserts the emulation prevention bytes
  uint32_t skip_emulation_bytes = 0;            // ... after this many bytes (the start code and NAL header)
  bool slice_header = false;                    // the last header, the slice's own
};
// The slice-level commands for the only slice of the picture: reference list, slice state, the headers, the VDEnc walker and the pipeline flush. `headers` are put in order;
// the last must be the slice header.
bool emit_avc_vdenc_slice(DwordWriter& w, const AvcEncodePicture& p, ArrayView<const AvcHeaderInsert> headers);
// What follows the slice: wait for the pipeline, store the bitstream size, end the batch.
void emit_avc_vdenc_finish(DwordWriter& w, const AvcEncodeBuffers& b, EncodeGeneration generation = EncodeGeneration::Gen9);

}  // namespace intel_enc
}  // namespace hardware
