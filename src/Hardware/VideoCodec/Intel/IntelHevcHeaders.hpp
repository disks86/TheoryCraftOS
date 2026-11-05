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
#include "Utility/ByteBuffer.hpp"

namespace hardware {
namespace intel_enc {

// The fixed coding structure of the hardware H.265 encoder: the parameter sets declare it and the picture-level commands use the same constants.
constexpr uint32_t kHevcLog2MinCb = 3;   // 8x8 minimum coding block
constexpr uint32_t kHevcLog2Ctb = 6;     // 64x64 coding tree blocks (the only size VDEnc supports)
constexpr uint32_t kHevcLog2MinTb = 2, kHevcLog2MaxTb = 5;
constexpr uint32_t kHevcMaxTransformDepthInter = 3, kHevcMaxTransformDepthIntra = 3;
constexpr uint32_t kHevcLog2ParMrgLevel = 2;
constexpr uint32_t kHevcDiffCuQpDeltaDepth = 0;   // cu_qp_delta_enabled_flag is 1 (the media driver requires it for VDEnc), at CTB granularity

// The H.265 headers the application writes around the slice data the video engine produces (the engine writes the CABAC slice_segment_data only): Annex B NAL units with a
// four-byte start code and a two-byte NAL unit header. The stream is Main profile, main tier, 8-bit 4:2:0, progressive, one slice per picture, one reference picture,
// output order = decoding order.
struct HevcStreamHeaders {
  uint32_t width = 0, height = 0;   // the displayed picture in luma samples (even; the coded picture is a multiple of 8 and the conformance window crops the rest)
  uint32_t level_idc = 120;         // general_level_idc (30 x the level number)
  uint32_t log2_max_poc_lsb = 8;    // 4 .. 16
  int32_t cb_qp_offset = 0, cr_qp_offset = 0;   // pps_cb_qp_offset, pps_cr_qp_offset: -12 .. 12
};

// The video, sequence and picture parameter sets, with emulation prevention bytes, appended to `out`. Each returns in `bits` what it added, start code included (a whole number
// of bytes; the units end on a byte boundary). False, with `out` unchanged, when memory ran out or the parameters are out of range.
bool write_hevc_vps(const HevcStreamHeaders& s, ByteBuffer* out, uint32_t* bits);
bool write_hevc_sps(const HevcStreamHeaders& s, ByteBuffer* out, uint32_t* bits);
bool write_hevc_pps(const HevcStreamHeaders& s, ByteBuffer* out, uint32_t* bits);

// The slice segment header of the picture's only slice, start code and NAL unit header included, ending with byte_alignment() because the CABAC slice data starts on a byte
// boundary. No emulation prevention bytes (the engine inserts those while it writes the slice data behind the header). `bits` is the exact length (a multiple of 8).
// IDR pictures are I slices of type IDR_W_RADL; the others are P slices of type TRAIL_R that reference the previous picture, or, for the video engine (which codes a P picture as a
// low-delay B one, the media driver converts P to B as well), B slices whose two lists both hold the previous picture. Deblocking must be on (the PPS says so).
struct HevcSliceHeader {
  bool idr = false;
  bool p_slice = false;      // ignored for IDR pictures (always I)
  bool b_slice = false;      // a generalised B slice (both lists the previous picture) instead of P; ignored for IDR pictures
  uint32_t poc_lsb = 0;      // not coded for IDR pictures
  int32_t qp = 26;           // 0 .. 51
  bool deblocking = true;
};
bool write_hevc_slice_header(const HevcStreamHeaders& s, const HevcSliceHeader& h, ByteBuffer* out, uint32_t* bits);

}  // namespace intel_enc
}  // namespace hardware
