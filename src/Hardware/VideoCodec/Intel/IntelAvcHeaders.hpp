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

// The H.264 headers the application writes in front of the slice data the video engine produces (the engine does not make parameter sets or slice headers): Annex B NAL
// units with a four-byte start code. The stream is Baseline profile (CAVLC, progressive, one slice per picture, one reference frame, output order = decoding order).
struct AvcStreamHeaders {
  uint32_t width = 0, height = 0;            // the displayed picture in luma samples (the coded picture is whole macroblocks)
  uint32_t level_idc = 40;
  uint32_t log2_max_frame_num = 8;            // 4 .. 16
  int32_t chroma_qp_offset = 0;
};

// The sequence parameter set and the picture parameter set, with emulation prevention bytes, appended to `out`. Each returns the bits it added, start code included
// (a whole number of bytes; the units end on a byte boundary). False when memory ran out or the parameters are out of range.
bool write_avc_sps(const AvcStreamHeaders& s, ByteBuffer* out, uint32_t* bits);
bool write_avc_pps(const AvcStreamHeaders& s, ByteBuffer* out, uint32_t* bits);

// The slice header of the picture's only slice, start code and NAL unit header included, but without emulation prevention (the engine inserts those bytes while it writes
// the slice data behind it) and without the byte alignment: `bits` is the exact length, and the slice data follows on the next bit.
struct AvcSliceHeader {
  bool idr = false;
  bool p_slice = false;
  uint32_t frame_num = 0;
  uint32_t idr_pic_id = 0;
  int32_t qp = 26;
  bool deblocking = true;
  int32_t alpha_c0_offset_div2 = 0, beta_offset_div2 = 0;
};
bool write_avc_slice_header(const AvcStreamHeaders& s, const AvcSliceHeader& h, ByteBuffer* out, uint32_t* bits);

}  // namespace intel_enc
}  // namespace hardware
