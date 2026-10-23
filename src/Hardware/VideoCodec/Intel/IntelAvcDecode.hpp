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
#include "Hardware/VideoCodec/Intel/IntelMfxCommands.hpp"
#include "Hardware/VideoCodec/Software/H264Parser.hpp"

namespace hardware {
namespace intel_mfx {

// The decoded-picture buffer as the engine addresses it: up to 16 frame stores, each with its picture and direct-motion-vector buffer, its order
// counts, and whether it is a long-term reference.
struct AvcFrameStore {
  bool used = false;
  uint64_t picture = 0;      // graphics address of the decoded NV12 frame
  uint64_t direct_mv = 0;    // graphics address of its direct motion vector buffer
  int32_t poc[2] = {};       // top, bottom
  bool long_term = false;
};

// What the batch of one picture is built from: the parsed slices (from H264StreamParser, all slices of the picture) and the buffers.
struct AvcPictureJob {
  const h264::ParameterSets* sets = nullptr;  // where the sequence and picture parameter sets live (H264StreamParser::parameter_sets())
  const h264::Sps* sps = nullptr;
  const h264::Pps* pps = nullptr;
  ArrayView<const H264Slice> slices;       // all slices of the picture, in stream order (non-empty)
  ConstByteView access_unit;              // the bytes the slices' offsets refer to, as they sit in the bitstream buffer
  uint64_t bitstream_address = 0;         // graphics address of the access unit's bytes
  AvcFrameStore frames[kMaxAvcReferenceFrames];
  // RefPicList0/1 per slice: indices into `frames` (by decoding order of this OS's reference lists), 0xFF for "no picture". One row per slice.
  // Filled by the caller from the same reference list construction the software decoder does.
  const uint8_t (*ref_list0)[kMaxAvcRefIdx] = nullptr;
  const uint8_t (*ref_list1)[kMaxAvcRefIdx] = nullptr;
  AvcBuffers buffers;                     // current picture and scratch buffer addresses (the reference arrays are filled from `frames`)
  int32_t current_poc[2] = {};            // top, bottom order counts of the picture being decoded
};

// Why a batch could not be built.
enum class AvcBatchError : uint8_t {
  None,
  Unsupported,    // a stream feature the engine path does not take (field pictures and MBAFF are refused by the parser; others here)
  BadInput,       // slices missing, a size the engine cannot do, a reference to a frame store that is not there
  NoRoom,         // the output buffer is too small
};

// Translates parsed H.264 syntax into the engine's picture parameters.
bool make_avc_picture(const h264::Sps& sps, const h264::Pps& pps, const h264::SliceHeader& first, AvcPicture* out);
// ... and one slice. `next` is the next slice of the picture or nullptr for the last.
bool make_avc_slice(const AvcPicture& picture, const h264::Pps& pps, const H264Slice& slice, const H264Slice* next, AvcSlice* out);

// Builds the complete command batch that decodes one picture, in the order the reference driver uses (flush, pipe mode, surface, buffers, quantiser
// matrices, picture ID, picture state, then per slice: direct mode, reference lists, weights, slice state, bitstream object), ending with the batch
// end. Writes into `out` and sets `*dwords`. Pure: no hardware is touched.
AvcBatchError build_avc_decode_batch(const AvcPictureJob& job, ArrayView<uint32_t> out, uint64_t* dwords);

}  // namespace intel_mfx
}  // namespace hardware
