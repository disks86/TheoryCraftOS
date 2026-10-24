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
#include "Hardware/VideoCodec/Software/H264Core.hpp"
#include "Hardware/VideoCodec/Software/H265Core.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// The messages the VCN decode firmware reads, and the sizes of the buffers it works in: how a program tells the engine what to decode. The layouts are the ones of
// Mesa's radeon_vcn_dec.h (Copyright Advanced Micro Devices, Inc.; MIT license), measured by scripts/gen_vcn_message_layout.py into AmdVcnMessageLayout.inc, and the
// values put into them follow Mesa's radeon_vcn_dec.c for H.264 (RDECODE_CODEC_H264_PERF, the stream type it uses on every VCN). The firmware keeps the decoded
// picture buffer itself and finds reference pictures by frame number and order count, so the messages carry those and no buffer addresses of references.
//
// Nothing here touches hardware: a message is built into a buffer the caller gives, and the sizes are arithmetic.
namespace amd_vcn {

// Commands sent to the firmware through the GPCOM registers (each followed by the address of the buffer it names in DATA0/DATA1).
constexpr uint32_t kCmdMsgBuffer = 0x000;
constexpr uint32_t kCmdDpbBuffer = 0x001;
constexpr uint32_t kCmdDecodingTargetBuffer = 0x002;
constexpr uint32_t kCmdFeedbackBuffer = 0x003;
constexpr uint32_t kCmdSessionContextBuffer = 0x005;
constexpr uint32_t kCmdBitstreamBuffer = 0x100;
constexpr uint32_t kCmdItScalingTableBuffer = 0x204;
constexpr uint32_t kCmdContextBuffer = 0x206;

constexpr uint32_t kMsgCreate = 0, kMsgDecode = 1, kMsgDestroy = 2;       // header msg_type
constexpr uint32_t kMessageCreate = 1, kMessageDecode = 2, kMessageAvc = 6, kMessageHevc = 13;   // index message_id
constexpr uint32_t kStreamTypeH264Perf = 7, kStreamTypeH265 = 0x10;
constexpr uint32_t kProfileBaseline = 0, kProfileMain = 1, kProfileHigh = 2;
constexpr uint32_t kArrayModeLinear = 0;

constexpr uint32_t kMessageBufferBytes = 0x1000;     // the message page
constexpr uint32_t kFeedbackOffset = 0x1000;         // from the start of the message buffer
constexpr uint32_t kFeedbackBytes = 2048;
constexpr uint32_t kItTableOffset = kFeedbackOffset + kFeedbackBytes;
constexpr uint32_t kItTableBytes = 992;              // 4x4 lists at 0 (6 x 16), 8x8 lists at 96 (2 x 64)
constexpr uint32_t kItTable8x8Offset = 96;
constexpr uint32_t kMessageFeedbackItBytes = kItTableOffset + kItTableBytes;   // message page, feedback and scaling table together
constexpr uint32_t kSessionContextBytes = 128 * 1024;
constexpr uint32_t kMaxH264References = 17;          // NUM_H264_REFS
constexpr uint32_t kMaxHevcReferences = 17;          // the pictures Mesa sizes the buffers for (8 from 4096 x 2000 up)

// Largest picture the OS asks the engine for: 4096 x 2304 (level 5.1 at 4K). Bigger is refused so sizes cannot overflow.
constexpr uint32_t kMaxWidth = 4096, kMaxHeight = 2304;

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

// Messages ----------------------------------------------------------------------------------------------------------------------------------------------------

// Bytes the messages occupy at the start of the message buffer.
uint32_t create_message_bytes();
uint32_t destroy_message_bytes();
uint32_t decode_message_bytes();   // header, the two indexes, the decode message and the AVC message
uint32_t hevc_decode_message_bytes();   // the same with the HEVC message
// False when `out` is too small. The bytes after the message are left alone.
bool build_create_message(ByteView out, uint32_t stream_handle, uint32_t width, uint32_t height, uint32_t stream_type = kStreamTypeH264Perf);
bool build_destroy_message(ByteView out, uint32_t stream_handle);

// What the engine is told about the picture, taken from the parameter sets and the first slice header.
struct AvcReference {
  uint32_t frame_num = 0;   // FrameNum for a short-term reference, LongTermFrameIdx for a long-term one
  int32_t top_poc = 0, bottom_poc = 0;
};

struct AvcPictureState {
  uint32_t width = 0, height = 0;   // the coded size, a whole number of macroblocks
  uint32_t level = 0;               // level_idc
  uint32_t profile = kProfileMain;
  uint32_t sps_flags = 0, pps_flags = 0;
  uint8_t chroma_format = 1;
  uint8_t log2_max_frame_num_minus4 = 0, poc_type = 0, log2_max_poc_lsb_minus4 = 0, num_ref_frames = 0;
  int8_t pic_init_qp_minus26 = 0, chroma_qp_index_offset = 0, second_chroma_qp_index_offset = 0;
  uint8_t num_ref_idx_l0_active_minus1 = 0, num_ref_idx_l1_active_minus1 = 0;
  uint8_t scaling_4x4[6][16] = {};   // in the order the bitstream sends them (zigzag), after the fall-back rules
  uint8_t scaling_8x8[2][64] = {};
  uint32_t frame_num = 0;
  int32_t top_poc = 0, bottom_poc = 0;
  uint32_t reference_count = 0;
  AvcReference references[16];
};

// Fills `*state` from the parser's structures. `references` are the pictures marked as reference when the picture starts (at most 16 are used). False if the
// picture is not one the engine takes (a size beyond the limits, a profile the firmware has no number for, a stream the parser should have refused).
bool make_avc_picture_state(const h264::ParameterSets& sets, const h264::Sps& sps, const h264::Pps& pps, const h264::SliceHeader& first_slice, int32_t top_poc, int32_t bottom_poc,
                            ArrayView<const AvcReference> references, AvcPictureState* state);

// Buffers -----------------------------------------------------------------------------------------------------------------------------------------------------

// How the decoded picture is laid out in the surface the engine writes: linear NV12, luma then chroma in one buffer.
struct TargetLayout {
  uint32_t luma_pitch = 0;       // bytes (= pixels) per luma row
  uint32_t chroma_pitch = 0;     // bytes per chroma row (interleaved Cb Cr)
  uint32_t uv_pitch_texels = 0;  // what the message calls dt_uv_pitch: the chroma pitch in two-byte texels, as Mesa passes the pitch of its two-channel chroma plane
  uint64_t chroma_offset = 0;    // from the start of the surface
  uint64_t bytes = 0;
};
TargetLayout target_layout(uint32_t width, uint32_t height);

struct AvcBufferSizes {
  uint64_t context = 0;   // the firmware's work memory (hw_ctxt_size)
  uint64_t dpb = 0;       // the decoded picture buffer the firmware keeps
  uint32_t references = 0;   // pictures both are sized for
};
// Sizes for pictures of `width` x `height` luma samples at `level`, with `max_references` reference frames (the stream's num_ref_frames). 0 sizes when the picture is too big.
AvcBufferSizes avc_buffer_sizes(uint32_t width, uint32_t height, uint32_t level, uint32_t max_references);
// The bitstream buffer: every slice behind a start code, padded with zeros to a multiple of 128 bytes (what bsd_size says). Never less than this for a picture.
uint64_t bitstream_buffer_bytes(uint32_t width, uint32_t height);

// What the decode message tells the engine about the buffers it is given for this picture.
struct DecodeBuffers {
  uint32_t bitstream_bytes = 0;   // the slices, before padding
  uint64_t dpb_bytes = 0, context_bytes = 0;
  TargetLayout target;
};
// Builds the decode message (header, indexes, decode message, AVC message) for one picture. `feedback_number` comes back in the feedback buffer. False when
// `out` is too small or a size does not fit the message's 32-bit fields.
bool build_avc_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const AvcPictureState& picture, const DecodeBuffers& buffers);
// Writes the feedback header (and nothing else) into the feedback area and the scaling lists into the scaling table, both inside a buffer of at least
// kMessageFeedbackItBytes that starts at the message buffer.
bool prepare_feedback_and_tables(ByteView message_buffer, const AvcPictureState& picture);

// H.265 -------------------------------------------------------------------------------------------------------------------------------------------------------

struct HevcBufferSizes {
  uint64_t context = 0;
  uint64_t dpb = 0;
  uint32_t references = 0;   // pictures both are sized for
};
// Sizes for pictures of `width` x `height` luma samples (multiples of 8) whose stream keeps up to `max_dec_pic_buffering` pictures (the sequence parameter set's
// sps_max_dec_pic_buffering_minus1 + 1). 0 sizes when the picture is too big or the stream wants more pictures than the buffers hold.
HevcBufferSizes hevc_buffer_sizes(uint32_t width, uint32_t height, uint32_t max_dec_pic_buffering);
// Builds the decode message (header, indexes, decode message, HEVC message) for one picture of the Core's offload. Only 8-bit 4:2:0 pictures: false otherwise, and when
// `out` is too small or a size does not fit the message's fields.
bool build_hevc_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const h265::OffloadPicture& picture, const DecodeBuffers& buffers);
// The feedback header and the scaling table (all four block sizes; kItTableBytes) of an H.265 picture.
bool prepare_hevc_feedback_and_tables(ByteView message_buffer, const h265::Sps& sps, const h265::Pps& pps);

// What the firmware wrote into the feedback area after a picture.
struct Feedback {
  bool valid = false;
  uint32_t feedback_number = 0;
  uint32_t status = 0;
  uint32_t error_bits = 0;
};
Feedback read_feedback(ConstByteView message_buffer);

}  // namespace amd_vcn
}  // namespace hardware
