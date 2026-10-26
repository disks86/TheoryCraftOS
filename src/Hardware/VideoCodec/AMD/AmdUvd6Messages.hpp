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
#include "Hardware/VideoCodec/AMD/AmdUvd6Ring.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnDecodeMessages.hpp"
#include "Hardware/VideoCodec/Software/H265Core.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2Core.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// The messages the UVD 6 firmware reads (Radeon RX Vega M and the other Polaris-family cards), and the sizes of the buffers it works in: how a program tells the engine
// what to decode. The layouts are those of Mesa's ac_uvd_dec.h (struct ruvd_msg; Copyright Advanced Micro Devices, Inc.; MIT license), measured by scripts/gen_uvd_message_layout.py
// into AmdUvd6MessageLayout.inc, and the values put into them follow Mesa's radeon_uvd.c for H.264 on Polaris and newer (RUVD_CODEC_H264_PERF: the firmware keeps the
// decoded picture buffer itself and finds reference pictures by frame number and order count, so the messages carry those and no buffer addresses of references) with
// a linear NV12 decoding target.
//
// Unlike VCN's, a UVD message is one fixed structure (struct ruvd_msg, 3556 bytes) with no header of indexes. The message page holds it, the feedback area follows at
// 0x1000 and the scaling table behind that.
//
// Nothing here touches hardware: a message is built into a buffer the caller gives, and the sizes are arithmetic.
namespace amd_uvd6 {

constexpr uint32_t kMsgCreate = 0, kMsgDecode = 1, kMsgDestroy = 2;      // msg_type
constexpr uint32_t kStreamTypeMpeg2 = 3, kStreamTypeH264Perf = 7, kStreamTypeH265 = 0x10;   // RUVD_CODEC_MPEG2, _H264_PERF, _H265

constexpr uint32_t kMessagePageBytes = 0x1000;       // the message structure's page; the feedback area starts behind it
constexpr uint32_t kFeedbackOffset = 0x1000;         // FB_BUFFER_OFFSET
constexpr uint32_t kFeedbackBytes = 2048;            // FB_BUFFER_SIZE
constexpr uint32_t kItTableOffset = kFeedbackOffset + kFeedbackBytes;
constexpr uint32_t kItTableBytes = 992;              // IT_SCALING_TABLE_SIZE: 4x4 lists at 0 (6 x 16), 8x8 lists at 96 (2 x 64)
constexpr uint32_t kItTable8x8Offset = 96;
constexpr uint32_t kMessageFeedbackItBytes = kItTableOffset + kItTableBytes;   // message page, feedback and scaling table together
constexpr uint32_t kSessionContextBytes = 128 * 1024;                     // UVD_SESSION_CONTEXT_SIZE
constexpr uint32_t kMaxH264References = 17;                               // NUM_H264_REFS
constexpr uint32_t kMaxWidth = 4096, kMaxHeight = 2304;                   // as the VCN messages: bigger is refused so sizes cannot overflow

// The size of struct ruvd_msg, which is also the `size` field of every message.
uint32_t message_bytes();

// False when `out` is smaller than the structure. The rest of `out` is left alone; the structure is cleared first, as Mesa clears it (memset) before it fills it.
bool build_create_message(ByteView out, uint32_t stream_handle, uint32_t width, uint32_t height, uint32_t dpb_bytes, uint32_t stream_type = kStreamTypeH264Perf);
bool build_destroy_message(ByteView out, uint32_t stream_handle);

// Sizes of the buffers the engine works in for pictures of `width` x `height` luma samples (whole macroblocks) at `level`, with `max_references` reference frames (the
// stream's num_ref_frames): Mesa's calc_dpb_size and calc_ctx_size_h264_perf for a part from Polaris on. 0 sizes when the picture is too big or not whole macroblocks.
struct AvcBufferSizes {
  uint64_t dpb = 0;       // the decoded picture buffer the firmware keeps
  uint64_t context = 0;   // the firmware's work memory (hw_ctxt_size, which the message calls dpb_reserved)
  uint32_t references = 0;
};
AvcBufferSizes avc_buffer_sizes(uint32_t width, uint32_t height, uint32_t level, uint32_t max_references);
// The bitstream buffer: every slice behind a start code, padded with zeros to a multiple of 128 bytes (what bsd_size says). Mesa sizes it from the picture: width * height * 512 / 256.
uint64_t bitstream_buffer_bytes(uint32_t width, uint32_t height);

// What the decode message tells the engine about the buffers it is given for this picture.
struct DecodeBuffers {
  uint32_t bitstream_bytes = 0;   // the slices, before padding
  uint64_t dpb_bytes = 0, context_bytes = 0;
  amd_vcn::TargetLayout target;   // the linear NV12 surface the picture is written to
};
// Builds the decode message for one picture. `feedback_number` comes back in the feedback buffer. False when `out` is too small, the picture is not one the engine
// takes here (8-bit 4:2:0 within the limits) or a size does not fit the message's 32-bit fields.
bool build_avc_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const amd_vcn::AvcPictureState& picture, const DecodeBuffers& buffers);
// Writes the feedback area's first word (its size, as Mesa does before a picture) and the scaling lists into the scaling table, inside a buffer of at least
// kMessageFeedbackItBytes that starts at the message page. The rest of the feedback area is cleared.
bool prepare_avc_feedback_and_tables(ByteView message_buffer, const amd_vcn::AvcPictureState& picture);

// H.265 (RUVD_CODEC_H265, 8-bit 4:2:0): unlike the H.264 performance stream type, the engine's decoded picture buffer is a set of slots the message names (a picture's slot,
// and the slots of the pictures it refers to), so the messages carry slot numbers as the VCN ones do. Sizes follow Mesa's calc_dpb_size and calc_ctx_size_h265_main.
struct HevcBufferSizes {
  uint64_t dpb = 0;       // all the slots
  uint64_t context = 0;
  uint32_t references = 0;   // slots (17; 8 from 4096 x 2000 up)
};
// 0 sizes when the picture is not whole minimum coding blocks (8) within the limits, or the stream needs more slots than there are.
HevcBufferSizes hevc_buffer_sizes(uint32_t width, uint32_t height, uint32_t max_dec_pic_buffering);
// Builds the decode message for one H.265 picture (the offload interface of H265Decoder hands over what it needs). False when `out` is too small, the sequence is not
// 8-bit 4:2:0 within the limits, or a size does not fit the message.
bool build_hevc_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const h265::OffloadPicture& picture, const DecodeBuffers& buffers);
// The feedback area's first word and the scaling table (4x4 lists at 0, 8x8 at 96, 16x16 at 480, 32x32 at 864) in a buffer of kMessageFeedbackItBytes at the message page.
bool prepare_hevc_feedback_and_tables(ByteView message_buffer, const h265::Sps& sps, const h265::Pps& pps);

// MPEG-2 (RUVD_CODEC_MPEG2, 4:2:0): the engine's picture buffer holds the last six pictures it decoded, and the message names a picture by the number of its decode job
// (decoded_pic_idx; Mesa's frame_number, counted from 1) and each reference by the number of the job that decoded it, no more than six back. There is no context buffer
// and no scaling table.
constexpr uint32_t kMpeg2References = 6;   // NUM_MPEG2_REFS
struct Mpeg2BufferSizes {
  uint64_t dpb = 0;
  uint32_t references = 0;
};
// Mesa's calc_dpb_size for MPEG-2: six pictures of the size aligned to a macroblock. 0 when the picture is too big or not whole macroblocks.
Mpeg2BufferSizes mpeg2_buffer_sizes(uint32_t width, uint32_t height);
// The job numbers the message carries: this picture's, and those of the pictures it predicts from (a picture with no reference names the latest job, as Mesa does).
struct Mpeg2Pictures {
  uint32_t current = 0, forward = 0, backward = 0;
};
// Builds the decode message of one picture (a frame, or a field). The quantiser matrices go in the order of the scan the picture uses, as Mesa sends them. False when `out`
// is too small, the picture is not 4:2:0 or within the limits, or a reference is not one of the last six jobs.
bool build_mpeg2_decode_message(ByteView out, uint32_t stream_handle, uint32_t feedback_number, const mpeg2::OffloadPicture& picture, const Mpeg2Pictures& pictures, const DecodeBuffers& buffers);
// The feedback area's first word (its size) in a buffer of at least kMessageFeedbackItBytes at the message page; the rest of the area is cleared.
bool prepare_mpeg2_feedback(ByteView message_buffer);

// Where the buffers of a job are, as the GPU addresses them (the message buffer holds the message page, the feedback area and the scaling table).
struct JobAddresses {
  uint64_t session_context = 0;
  uint64_t message = 0;           // the start of the message buffer
  uint64_t dpb = 0;
  uint64_t context = 0;
  uint64_t bitstream = 0;
  uint64_t target = 0;            // the start of the decoding target surface
};
constexpr uint32_t kDecodeJobBuffers = 8;
// The buffers of a decode job in the order Mesa names them to the engine (session context, message, picture buffer, context, bitstream, decoding target, feedback,
// scaling table). `out` must hold kDecodeJobBuffers; the count is returned (0 if it does not fit).
uint32_t decode_job_buffers(const JobAddresses& addresses, ArrayView<AmdUvd6Ring::Buffer> out);
// The buffers of an MPEG-2 job: session context, message, picture buffer, bitstream, decoding target, feedback (no context, no scaling table).
constexpr uint32_t kMpeg2JobBuffers = 6;
uint32_t mpeg2_job_buffers(const JobAddresses& addresses, ArrayView<AmdUvd6Ring::Buffer> out);
// The buffers of a create or destroy job: the session context and the message.
constexpr uint32_t kSessionJobBuffers = 2;
uint32_t session_job_buffers(const JobAddresses& addresses, ArrayView<AmdUvd6Ring::Buffer> out);

}  // namespace amd_uvd6
}  // namespace hardware
