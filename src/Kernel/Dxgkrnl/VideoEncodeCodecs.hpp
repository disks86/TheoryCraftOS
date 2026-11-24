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

// The HEVC and AV1 EncodeFrames (VideoEncodeHevc.cpp, VideoEncodeAv1.cpp; run_video_encode passes them the jobs whose word 0 says so) and what they share
// (VideoEncodePlanes.cpp). Both run on the CPU encoders of Hardware/VideoCodec/Software (H265PictureEncoder, Av1PictureEncoder) with the rate controller of the H.264
// path. The pictures are widened planes (DxgTypes.hpp): NV12 as 4 bytes a pixel, P010 as 8 (four 16-bit words, the sample in the top ten bits of the first one or two).
//
// HEVC arguments (little-endian 32-bit words, kVideoEncodeParamWords of them - those not named here zero -, then the arrays):
//    0 codec (1)   1 profile (0 Main, 1 Main 10)   2 width   3 height (the coded size)   4 configuration flags (D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAGS)
//    5 MinLumaCodingUnitSize   6 MaxLumaCodingUnitSize   7 MinLumaTransformUnitSize   8 MaxLumaTransformUnitSize (the D3D12 enumerations' values)
//    9 max_transform_hierarchy_depth_inter | max_transform_hierarchy_depth_intra << 8
//   10 .. 30 rate control, as the H.264 arguments (VideoEncode.hpp)
//   31 slice layout mode   32 its value   33 motion estimation precision (D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE)
//   34 frame type (D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC)   35 slice_pic_parameter_set_id   36 PictureOrderCountNumber   37 TemporalLayerIndex
//   38 picture flags (1 used as reference, 2 intra constrained slices requested)   39 log2_max_pic_order_cnt_lsb_minus4
//   40 descriptor count   41 list 0 count   42 list 1 count   43 list 0 modification count   44 list 1 modification count   45 QP map count
//   46 errors the runtime found (nothing is coded, the metadata says them)   47 the largest number of slices the metadata buffer has room for   48 bit depth (8, 10)
//   then per descriptor 5 words (texture index, IsRefUsedByCurrentPic, IsLongTermReference, PictureOrderCountNumber, TemporalLayerIndex), the list entries (descriptor
//   indices), the modifications (list_entry values), the QP map's signed bytes (one a coding tree block, padded to a word).
// Its metadata is the H.264 path's (version 1: D3D12_VIDEO_ENCODER_OUTPUT_METADATA and a record a slice).
//
// AV1 arguments (kVideoEncodeParamWords words, then kVideoEncodeAv1FixedWords, then the arrays):
//    0 codec (2)   1 profile (0 Main)   2 width   3 height   4 feature flags (D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAGS)   5 OrderHintBitsMinus1   6 bit depth (8, 10)
//    7 subregion layout mode (FULL_FRAME, UNIFORM_GRID_PARTITION 5, CONFIGURABLE_GRID_PARTITION 6)   8 tile rows   9 tile columns
//   10 .. 30 rate control, as the H.264 arguments, the QPs quantizer indices (0 .. 255)
//   31 ContextUpdateTileId   32 (0)   33 motion estimation precision
//   34 frame type (D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE)   35 picture flags (D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAGS)   36 CompoundPredictionType
//   37 InterpolationFilter   38 TxMode   39 SuperResDenominator   40 OrderHint   41 PictureIndex   42 TemporalLayerIndexPlus1 | SpatialLayerIndexPlus1 << 16
//   43 PrimaryRefFrame (asked for; the encoder answers in the post-encode values)   44 RefreshFrameFlags   45 QP map count   46 errors the runtime found
//   47 the largest number of tiles the metadata buffer has room for   48 (0)   49 (0)
//   fixed part: ReferenceIndices[7]; per reference slot 4 words (texture index, 0xFFFFFFFF: empty; OrderHint; FrameType; PictureIndex); FrameRestorationConfig (3 types,
//   3 sizes); the loop filter (LoopFilterLevel[0], [1], U, V, sharpness, DeltaEnabled, UpdateRefDelta, RefDeltas[8], UpdateModeDelta, ModeDeltas[2]); the loop filter
//   delta (DeltaLFPresent, DeltaLFMulti, DeltaLFRes); the quantization (BaseQIndex, Y DC, U DC, U AC, V DC, V AC deltas, UsingQMatrix, QMY, QMU, QMV); the quantization
//   delta (DeltaQPresent, DeltaQRes); CDEF (CdefBits, CdefDampingMinus3, Y primary[8], UV primary[8], Y secondary[8], UV secondary[8]). Signed values are two's
//   complement words.
//   then (CONFIGURABLE_GRID_PARTITION) the row heights and column widths in superblocks, then the QP map's 16-bit values (one a 64x64 superblock, padded to a word).
// Its metadata (version 3): D3D12_VIDEO_ENCODER_OUTPUT_METADATA, a D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA a tile, then
// D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_TILES and D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES - the layout ResolveEncoderOutputMetadata gives
// an AV1 program - after the 8-byte header.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/VideoCodec/Software/VideoEncPlanes.hpp"
#include "Hardware/VideoCodec/Software/VideoRateControl.hpp"
#include "Kernel/Dxgkrnl/VideoEncode.hpp"
#include "Utility/ByteStream.hpp"
#include "Utility/ArrayView.hpp"

namespace dxgk {

constexpr uint32_t kVideoEncodeCodecH264 = 0, kVideoEncodeCodecHevc = 1, kVideoEncodeCodecAv1 = 2;
constexpr uint32_t kVideoEncodeMetadataVersionAv1 = 3;   // (1 and 2 are the H.264 / HEVC layouts: VideoEncode.hpp)
constexpr uint32_t kVideoEncodeAv1FixedWords = 7 + 8 * 4 + 6 + 18 + 3 + 10 + 2 + 34;
constexpr uint32_t kVideoEncodeAv1TilesBytes = 8 + 8 + 64 * 8 + 64 * 8 + 8;   // D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_TILES
constexpr uint32_t kVideoEncodeAv1PostEncodeBytes = 8 + 144 + 24 + 80 + 16 + 272 + 608 + 8 + 56;   // D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES
constexpr uint32_t kVideoEncodeOutputMetadataBytes = 72;                      // D3D12_VIDEO_ENCODER_OUTPUT_METADATA

// D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAGS
constexpr uint64_t kEncodeErrorPictureControl = 0x1, kEncodeErrorSubregionLayout = 0x2, kEncodeErrorReferences = 0x4, kEncodeErrorReconfiguration = 0x8,
                   kEncodeErrorMetadataSource = 0x10;

[[nodiscard]] bool run_video_encode_hevc(allocators::IAllocator& allocator, const VideoEncodeJob& job);
[[nodiscard]] bool run_video_encode_av1(allocators::IAllocator& allocator, const VideoEncodeJob& job);
// The resolved form of a version 3 (AV1) metadata buffer (`metadata` from its magic on) into `out`; false if `out` has no room.
[[nodiscard]] bool resolve_av1_encode_metadata(ConstByteView metadata, ByteView out);

// The rate controller's state in the encoder heap (VideoEncode.cpp's, shared with the H.264 path).
void load_rate_control_state(ConstByteView heap, hardware::RateControlState* state);
void store_rate_control_state(ByteView heap, const hardware::RateControlState& state);
// Words 10 .. 30 of the arguments as a configuration; false if the mode is not one there is.
[[nodiscard]] bool rate_control_from_words(ArrayView<const uint32_t> words, hardware::RateControlConfig* config);

// A 4:2:0 picture of 16-bit samples (VideoEncPlanes.hpp) the encoders take, made from and written back to widened planes.
class Picture16 {
 public:
  explicit Picture16(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~Picture16();
  Picture16(const Picture16&) = delete;
  Picture16& operator=(const Picture16&) = delete;
  [[nodiscard]] bool make(uint32_t width, uint32_t height);
  hardware::EncPlanes16 planes() const;
  hardware::EncWritablePlanes16 writable() const;
  // From the top-left width x height of widened planes of `bit_depth` (8: 4 bytes a pixel, 10: 8 bytes, the sample in the top ten bits). The planes must fit
  // (planes_fit).
  void read(const VideoPictureImages& in, uint32_t bit_depth);
  // Into widened planes: (Y, 0, 0, max) and (U, V, 0, max) in the sample size of the bit depth.
  void write(const VideoWritablePlaneImage& luma, const VideoWritablePlaneImage& chroma, uint32_t bit_depth) const;

 private:
  allocators::IAllocator& allocator_;
  ArrayView<uint16_t> samples_;
  uint32_t width_ = 0, height_ = 0;
};
// Whether widened planes of `bit_depth` hold a width x height picture (luma) and its half-size chroma.
[[nodiscard]] bool planes_fit(const VideoPictureImages& images, uint32_t width, uint32_t height, uint32_t bit_depth);
[[nodiscard]] bool planes_fit(const VideoWritablePlaneImage& luma, const VideoWritablePlaneImage& chroma, uint32_t width, uint32_t height, uint32_t bit_depth);

// D3D12_VIDEO_ENCODER_OUTPUT_METADATA (72 bytes) from the statistics; `subregions` the count written after it.
void write_output_metadata(ByteWriter& w, uint64_t errors, uint64_t written, const hardware::EncStatistics& stats, uint64_t subregions);

}  // namespace dxgk
