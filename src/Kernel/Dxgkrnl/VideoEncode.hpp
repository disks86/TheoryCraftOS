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

// The video encoding and motion estimation the command stream's EncodeFrame / ResolveEncoderMetadata / EstimateMotion / ResolveMotionVectors do (CommandStream.hpp), on the
// CPU (Hardware/VideoCodec/Software: H264PictureEncoder, VideoRateControl, MotionEstimator). What they read and write are views of the allocations the stream names; the
// pictures are the widened planes a runtime keeps NV12 in (DxgTypes.hpp VideoDecoderDesc: 4 bytes a pixel, Y or U, V in the first bytes).
//
// The arguments of an EncodeFrame (a buffer the stream names; little-endian 32-bit words, then the arrays):
//    0 codec (0 H.264; 1 HEVC and 2 AV1 have layouts of their own: VideoEncodeCodecs.hpp)   1 profile (0 Main, 1 High)   2 width   3 height (the target resolution: even; the coded size is that in whole macroblocks, the program's SPS crops it)
//    4 configuration flags (D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAGS: constrained intra prediction, the 8x8 transform, CABAC)   5 direct mode (0 none, 1 temporal,
//      2 spatial)   6 deblocking mode (disable_deblocking_filter_idc)
//    7 pic_order_cnt_type   8 log2_max_frame_num_minus4   9 log2_max_pic_order_cnt_lsb_minus4
//   10 rate control mode   11 rate control flags   12 frame rate numerator   13 denominator   14 CQP intra   15 CQP P   16 CQP B   17 initial QP   18 minimum QP   19 maximum QP
//   20/21 target bit rate (low, high)   22/23 peak bit rate   24/25 VBV capacity   26/27 initial VBV fullness   28/29 maximum frame bits   30 quality target (QVBR)
//   31 slice layout mode   32 its value (macroblocks, rows, slices or bytes)   33 motion estimation precision (D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE)
//   34 frame type   35 idr_pic_id   36 pic_parameter_set_id   37 PictureOrderCountNumber   38 FrameDecodingOrderNumber   39 picture flags (1 used as reference, 2 intra
//      constrained slices requested)   40 adaptive_ref_pic_marking_mode_flag
//   41 reference descriptor count   42 list 0 count   43 list 1 count   44 marking operation count   45 list 0 modification count   46 list 1 modification count
//   47 QP map count   48 errors the runtime found (D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAGS: nothing is coded, the metadata says them)   49 the largest number of slices the metadata buffer has room for
//   50 intra refresh mode (0 none, 1 row based)   51 its duration (pictures)   52 IntraRefreshFrameIndex
//   53 unchanged-macroblock map count (0, or one byte a macroblock: dirty regions)   54 the list 0 index those macroblocks copy
//   55 motion hint count (0, or one a macroblock)   56 hint mode (D3D12_VIDEO_ENCODER_FRAME_MOTION_SEARCH_MODE)   57 hint distance limit (whole samples)
//   58 optional metadata (D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAGS: frame and subregion PSNR)   59 .. 63 zero
//   then per descriptor 6 words (texture index, long term, long-term index, POC, frame number, temporal layer), the list entries (descriptor indices), 5 words per marking
//   operation, 3 per list modification, the QP map's signed bytes (padded to a word), the unchanged map's bytes (padded to a word), 12 bytes a hint (list 0 and list 1
//   reference index as signed bytes - -1 none -, 2 zero bytes, then the list 0 and list 1 vectors as signed 16-bit x, y quarter samples).
// The metadata buffer (opaque to the program; ResolveEncoderMetadata turns it into D3D12_VIDEO_ENCODER_OUTPUT_METADATA and the subregion records): a word 'TCEM', a word 1
// (2 with the optional metadata), then exactly that layout (72 bytes, then 24 a slice); version 2 goes on with a word 'TCEO', the optional metadata flags, then the frame's
// and each slice's sums of squared errors (luma, Cb, Cr) and sample counts (luma, chroma): five 64-bit words each.
// The encoder heap holds the rate controller's state between pictures (kVideoEncodeHeapBytes), then, if it has room (video_encode_heap_bytes), what H.264 keeps of each
// reconstructed picture that later pictures refer to: its macroblocks' motion (for direct prediction) and the samples of its coded size that its texture has no room for
// (cropped sizes), found again by the texture (its allocation) and the picture's order count. The motion vector heap: a word 'TCMV', the block size, columns, rows, then per
// block the vector (two signed 16-bit quarter samples) and its SAD (32 bits).
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Utility/ArrayView.hpp"

namespace dxgk {

constexpr uint32_t kVideoEncodeParamWords = 64;
constexpr uint32_t kVideoEncodeMaxReferences = 16;
constexpr uint32_t kVideoEncodeMetadataHeader = 8 + 72, kVideoEncodeSubregionBytes = 24;
constexpr uint32_t kVideoEncodeHeapBytes = 256;   // (the least an encoder heap has: the rate controller's state)
constexpr uint32_t kVideoEncodeMetadataOptionalMagic = 0x4F454354u;   // 'TCEO'
constexpr uint32_t kVideoEncodePsnrRecordBytes = 40;
constexpr uint32_t kVideoEncodeOptionalFramePsnr = 0x8, kVideoEncodeOptionalSubregionPsnr = 0x10;   // (D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAGS)
// H.264's store of reference pictures in the encoder heap: a header, then this many slots (16 references and the picture being coded) of h264_slot_bytes.
constexpr uint32_t kH264StoreSlots = 17, kH264StoreHeader = 32, kH264SlotHeader = 64, kH264MotionBytes = 40;
inline uint64_t h264_slot_bytes(uint32_t width, uint32_t height) {
  const uint64_t w = (uint64_t{width} + 15) / 16 * 16, h = (uint64_t{height} + 15) / 16 * 16;
  // (the macroblocks' motion; at most 15 columns and rows of the coded size beyond a texture of the target size, luma and both chroma planes)
  return kH264SlotHeader + (w / 16) * (h / 16) * kH264MotionBytes + (15 * (w + h) * 3 / 2 + 7) / 8 * 8;
}
// The bytes an encoder heap for pictures up to width x height has: the rate controller's state and the reference store.
inline uint64_t video_encode_heap_bytes(uint32_t width, uint32_t height) { return kVideoEncodeHeapBytes + kH264StoreHeader + kH264StoreSlots * h264_slot_bytes(width, height); }
// The metadata buffer EncodeFrame needs for a picture of `mbs` macroblocks (slices at most one a macroblock), with the optional metadata `optional` asks for.
inline uint64_t video_encode_metadata_bytes(uint64_t mbs, uint32_t optional) {
  const bool psnr = (optional & (kVideoEncodeOptionalFramePsnr | kVideoEncodeOptionalSubregionPsnr)) != 0;
  return kVideoEncodeMetadataHeader + mbs * kVideoEncodeSubregionBytes + (psnr ? 8 + kVideoEncodePsnrRecordBytes * (1 + mbs) : 0);
}
constexpr uint32_t kMotionVectorHeapHeader = 16, kMotionVectorEntryBytes = 8;
constexpr uint32_t kVideoEncodeMetadataMagic = 0x4D454354u, kMotionVectorHeapMagic = 0x564D4354u;   // 'TCEM', 'TCMV'

// One widened 8-bit plane: `width` x `height` pixels of 4 bytes, rows `pitch` bytes apart.
struct VideoPlaneImage {
  ConstByteView memory;
  uint64_t pitch = 0;
  uint32_t width = 0, height = 0;
};
struct VideoWritablePlaneImage {
  ByteView memory;
  uint64_t pitch = 0;
  uint32_t width = 0, height = 0;
};
struct VideoPictureImages {
  VideoPlaneImage luma, chroma;
};

struct VideoEncodeJob {
  ConstByteView params;
  ByteView heap;                                  // the encoder heap (empty: none kept): the rate controller's state, the reference store if it has room
  VideoPictureImages input;
  bool has_reconstruction = false;
  VideoWritablePlaneImage reconstruction_luma, reconstruction_chroma;
  ArrayView<const VideoPictureImages> references; // the reference textures, by the descriptors' texture index
  uint64_t reconstruction_key = 0;                // what tells textures apart (their luma allocation's id): the reconstruction's, and the references' by texture index
  ArrayView<const uint64_t> reference_keys;
  ByteView bitstream;                             // from the frame's start offset to the end of the buffer
  ByteView metadata;                              // from the metadata offset to the end of the buffer
};

// Codes one picture: the slices into the bitstream, the reconstruction, the metadata (always, if it has room for its header: a picture that cannot be coded says why in
// EncodeErrorFlags). False only for allocation failure.
[[nodiscard]] bool run_video_encode(allocators::IAllocator& allocator, const VideoEncodeJob& job);
// The metadata EncodeFrame wrote, resolved: D3D12_VIDEO_ENCODER_OUTPUT_METADATA and one record a slice into `out` (a source that is not EncodeFrame's says
// INVALID_METADATA_BUFFER_SOURCE); with `frame_psnr` / `subregion_psnr` not empty also the PSNRs (D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_PSNR_RESOLVED_LAYOUT: three
// floats, the frame's, or one for each slice; zeros where EncodeFrame was not asked for them). False if an output has no room for what is to be written.
[[nodiscard]] bool resolve_video_encode_metadata(ConstByteView metadata, ByteView out, ByteView frame_psnr = ByteView(), ByteView subregion_psnr = ByteView());

// The motion vectors of `current` into `reference` (both width x height luma), blocks of `block` (8 or 16) samples, into `heap`; `hints` an earlier heap or empty.
[[nodiscard]] bool run_motion_estimation(allocators::IAllocator& allocator, const VideoPlaneImage& current, const VideoPlaneImage& reference, uint32_t width, uint32_t height,
                                         uint32_t block, ConstByteView hints, ByteView heap);
// The vectors of `heap` (for a picture of width x height) as R16G16_SINT pixels of an image kept widened (A16B16G16R16Sint: 8 bytes a pixel) at (x, y) of `image`.
// (`texel`: the bytes of a pixel of the image - 4 for an R16G16Sint one, the vector alone; 8 for an A16B16G16R16Sint one, the vector, 0 and an integer 1)
[[nodiscard]] bool resolve_motion_vectors(ConstByteView heap, uint32_t width, uint32_t height, const VideoWritablePlaneImage& image, uint32_t x, uint32_t y, uint32_t texel = 8);
// The bytes a motion vector heap of a picture of at most width x height needs.
inline uint64_t motion_vector_heap_bytes(uint32_t width, uint32_t height, uint32_t block) {
  return kMotionVectorHeapHeader + uint64_t{(width + block - 1) / block} * ((height + block - 1) / block) * kMotionVectorEntryBytes;
}

}  // namespace dxgk
