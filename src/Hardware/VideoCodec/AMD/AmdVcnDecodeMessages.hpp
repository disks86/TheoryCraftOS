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
#include "Hardware/VideoCodec/Software/Av1Core.hpp"
#include "Hardware/VideoCodec/Software/H265Core.hpp"
#include "Hardware/VideoCodec/Software/Vp9Core.hpp"
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
constexpr uint32_t kCmdProbTableBuffer = 0x004;      // (the AV1 segmentation and film grain table, in the place the H.264/H.265 scaling table has)
constexpr uint32_t kCmdItScalingTableBuffer = 0x204;
constexpr uint32_t kCmdContextBuffer = 0x206;

constexpr uint32_t kMsgCreate = 0, kMsgDecode = 1, kMsgDestroy = 2;       // header msg_type
constexpr uint32_t kMessageCreate = 1, kMessageDecode = 2, kMessageAvc = 6, kMessageHevc = 13, kMessageVp9 = 0x0E, kMessageDynamicDpb = 0x10, kMessageAv1 = 0x11;   // index message_id
constexpr uint32_t kStreamTypeH264Perf = 7, kStreamTypeH265 = 0x10, kStreamTypeVp9 = 0x11, kStreamTypeAv1 = 0x13;
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

// AV1 ---------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kAv1References = 9;   // the pictures the buffers are sized for: eight reference slots and the one being decoded
constexpr uint32_t kAv1NoPicture = 0x7F;

struct Av1BufferSizes {
  uint64_t context = 0;   // the firmware's work memory (hw_ctxt_size), which starts with four initial entropy contexts
  uint64_t dpb = 0;
  uint32_t references = 0;
};
// Sizes for a stream whose frames are at most `max_width` x `max_height` luma samples (the sequence header's maximum). Mesa sizes the picture buffer for 8192 x 4320
// whatever the stream is; here it follows the stream's own maximum. 0 sizes when the picture is too big.
Av1BufferSizes av1_buffer_sizes(uint32_t max_width, uint32_t max_height);
// The picture buffer slots the engine is told about: the one this frame is decoded into and, for each of the eight reference slots of the stream, the engine slot of
// the frame it holds (kAv1NoPicture for none).
struct Av1Slots {
  uint8_t current = 0;
  uint8_t ref_frame_map[8] = {kAv1NoPicture, kAv1NoPicture, kAv1NoPicture, kAv1NoPicture, kAv1NoPicture, kAv1NoPicture, kAv1NoPicture, kAv1NoPicture};
};
uint32_t av1_decode_message_bytes();
// Builds the decode message (header, indexes, decode message, AV1 message) of one frame. Only 8-bit 4:2:0 or monochrome frames without film grain or
// super-resolution (the engine's output of those is not compared with the CPU's here): false otherwise, and when `out` is too small. The tile data goes into the
// bitstream buffer back to back, tile after tile, and the message names where each tile starts and how long it is (`DecodeBuffers::bitstream_bytes` is their sum).
bool build_av1_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const av1::OffloadFrame& frame, const Av1Slots& slots, const DecodeBuffers& buffers);
// Bytes a message buffer must have for AV1: the message page, the feedback area and the segmentation / film grain table behind them.
uint32_t av1_message_buffer_bytes();
// The feedback header, and the segmentation table (feature data and mask, which the engine reads from the table buffer) of the frame.
bool prepare_av1_feedback_and_tables(ByteView message_buffer, const av1::FrameHeader& header);
// The four initial entropy contexts that start the context buffer, as runs of non-zero bytes (everything between is zero). `index` < av1_context_run_count().
uint32_t av1_context_run_count();
bool av1_context_run(uint32_t index, uint32_t* offset, ConstByteView* bytes);
uint64_t av1_context_bytes();   // the four contexts, with their padding

// VP9 ---------------------------------------------------------------------------------------------------------------------------------------------------------

constexpr uint32_t kVp9References = 9;   // the pictures the buffers are sized for: eight reference slots and the one being decoded
constexpr uint32_t kVp9NoPicture = 0x7F;

struct Vp9BufferSizes {
  uint64_t context = 0;   // the firmware's work memory (hw_ctxt_size), which starts with the default probabilities
  uint64_t dpb = 0;
  uint32_t references = 0;
};
// Sizes for pictures of up to `width` x `height` luma samples (Mesa's, for engines before Renoir). 0 sizes when the picture is too big.
Vp9BufferSizes vp9_buffer_sizes(uint32_t width, uint32_t height);
// The picture buffer slots the engine is told about: the one this frame is decoded into and, for each of the eight reference slots of the stream, the engine slot of
// the frame it holds (kVp9NoPicture for none).
struct Vp9Slots {
  uint8_t current = 0;
  uint8_t ref_frame_map[8] = {kVp9NoPicture, kVp9NoPicture, kVp9NoPicture, kVp9NoPicture, kVp9NoPicture, kVp9NoPicture, kVp9NoPicture, kVp9NoPicture};
};
uint32_t vp9_decode_message_bytes();   // header, the three indexes, the decode message, the dynamic picture buffer message and the VP9 message
// Builds the decode message of one 8-bit 4:2:0 frame: false otherwise, and when `out` is too small or a size does not fit the message's fields. The picture buffer is
// described the way Mesa does for the engines before Navi 21 (a "dynamic" buffer of nine pictures of the frame's size).
bool build_vp9_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const vp9::OffloadFrame& frame, const Vp9Slots& slots, const DecodeBuffers& buffers);
// Bytes a message buffer must have for VP9: the message page, the feedback area and the probability and segmentation table behind them.
uint32_t vp9_message_buffer_bytes();
// The feedback header, and the table the engine reads next to the message: the default probabilities and the segmentation data of the frame.
bool prepare_vp9_feedback_and_tables(ByteView message_buffer, const vp9::OffloadFrame& frame);
// The default probabilities as they start the context buffer.
ConstByteView vp9_default_probabilities();

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
