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
// The DXVA structures of VP9 decoding (dxva.h: DXVA_PicEntry_VPx, DXVA_segmentation_VP9, DXVA_PicParams_VP9, DXVA_Slice_VPx_Short), byte for byte as a Windows
// program lays them out. dxva.h declares them under #pragma pack(1), so 32-bit and 64-bit programs agree; the offsets below were checked against mingw-w64's dxva.h
// compiled for x86_64 and i686. The bit-field unions are kept as the whole integer they share, with masks for the fields (a Windows compiler allocates bit-fields from
// the least significant bit up).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace dxva {

#pragma pack(push, 1)

// DXVA_PicEntry_VPx: Index7Bits (bits 0-6), AssociatedFlag (bit 7); 0xFF is "no picture".
struct PicEntryVpx {
  uint8_t bPicEntry;
  uint8_t index() const { return bPicEntry & 0x7F; }
  bool none() const { return bPicEntry == 0xFF; }
};

struct SegmentationVp9 {
  uint8_t wSegmentInfoFlags;   // enabled 0, update_map 1, temporal_update 2, abs_delta 3
  uint8_t tree_probs[7];
  uint8_t pred_probs[3];
  int16_t feature_data[8][4];
  uint8_t feature_mask[8];
};

struct PicParamsVp9 {
  PicEntryVpx CurrPic;
  uint8_t profile;
  uint16_t wFormatAndPictureInfoFlags;   // see the kVp9* masks
  uint32_t width;
  uint32_t height;
  uint8_t BitDepthMinus8Luma;
  uint8_t BitDepthMinus8Chroma;
  uint8_t interp_filter;
  uint8_t Reserved8Bits;
  PicEntryVpx ref_frame_map[8];
  uint32_t ref_frame_coded_width[8];
  uint32_t ref_frame_coded_height[8];
  PicEntryVpx frame_refs[3];
  int8_t ref_frame_sign_bias[4];
  int8_t filter_level;
  int8_t sharpness_level;
  uint8_t wControlInfoFlags;   // mode_ref_delta_enabled 0, mode_ref_delta_update 1, use_prev_in_find_mv_refs 2
  int8_t ref_deltas[4];
  int8_t mode_deltas[2];
  int16_t base_qindex;
  int8_t y_dc_delta_q;
  int8_t uv_dc_delta_q;
  int8_t uv_ac_delta_q;
  SegmentationVp9 stVP9Segments;
  uint8_t log2_tile_cols;
  uint8_t log2_tile_rows;
  uint16_t uncompressed_header_size_byte_aligned;
  uint16_t first_partition_size;
  uint16_t Reserved16Bits;
  uint32_t Reserved32Bits;
  uint32_t StatusReportFeedbackNumber;
};

struct SliceVpxShort {
  uint32_t BSNALunitDataLocation;
  uint32_t SliceBytesInBuffer;
  uint16_t wBadSliceChopping;
};

#pragma pack(pop)

// wFormatAndPictureInfoFlags
constexpr uint16_t kVp9FrameType = 1u << 0;   // 0: key frame
constexpr uint16_t kVp9ShowFrame = 1u << 1;
constexpr uint16_t kVp9ErrorResilient = 1u << 2;
constexpr uint16_t kVp9SubsamplingX = 1u << 3;
constexpr uint16_t kVp9SubsamplingY = 1u << 4;
constexpr uint16_t kVp9ExtraPlane = 1u << 5;
constexpr uint16_t kVp9RefreshFrameContext = 1u << 6;
constexpr uint16_t kVp9FrameParallel = 1u << 7;
constexpr uint16_t kVp9IntraOnly = 1u << 8;
constexpr uint16_t kVp9AllowHighPrecisionMv = 1u << 13;
constexpr int kVp9FrameContextIdxShift = 9, kVp9ResetFrameContextShift = 11;

static_assert(sizeof(PicEntryVpx) == 1 && sizeof(SegmentationVp9) == 83 && sizeof(PicParamsVp9) == 208 && sizeof(SliceVpxShort) == 10);
static_assert(offsetof(PicParamsVp9, wFormatAndPictureInfoFlags) == 2 && offsetof(PicParamsVp9, width) == 4 && offsetof(PicParamsVp9, height) == 8);
static_assert(offsetof(PicParamsVp9, BitDepthMinus8Luma) == 12 && offsetof(PicParamsVp9, interp_filter) == 14 && offsetof(PicParamsVp9, ref_frame_map) == 16);
static_assert(offsetof(PicParamsVp9, ref_frame_coded_width) == 24 && offsetof(PicParamsVp9, ref_frame_coded_height) == 56 && offsetof(PicParamsVp9, frame_refs) == 88);
static_assert(offsetof(PicParamsVp9, ref_frame_sign_bias) == 91 && offsetof(PicParamsVp9, filter_level) == 95 && offsetof(PicParamsVp9, wControlInfoFlags) == 97);
static_assert(offsetof(PicParamsVp9, ref_deltas) == 98 && offsetof(PicParamsVp9, mode_deltas) == 102 && offsetof(PicParamsVp9, base_qindex) == 104);
static_assert(offsetof(PicParamsVp9, uv_ac_delta_q) == 108 && offsetof(PicParamsVp9, stVP9Segments) == 109 && offsetof(PicParamsVp9, log2_tile_cols) == 192);
static_assert(offsetof(PicParamsVp9, uncompressed_header_size_byte_aligned) == 194 && offsetof(PicParamsVp9, first_partition_size) == 196);
static_assert(offsetof(PicParamsVp9, Reserved32Bits) == 200 && offsetof(PicParamsVp9, StatusReportFeedbackNumber) == 204);
static_assert(offsetof(SegmentationVp9, tree_probs) == 1 && offsetof(SegmentationVp9, pred_probs) == 8 && offsetof(SegmentationVp9, feature_data) == 11 &&
              offsetof(SegmentationVp9, feature_mask) == 75);
static_assert(offsetof(SliceVpxShort, SliceBytesInBuffer) == 4 && offsetof(SliceVpxShort, wBadSliceChopping) == 8);

// Copies the structure at `offset` of a buffer a program gave into `out`. False if the buffer is too short. The structures are made of integers only (any bytes are a
// valid value), packed, and the host is little-endian as Windows is: the bytes are the structure.
template <class T>
[[nodiscard]] bool read_vp9_struct(ConstByteView buffer, uint64_t offset, T* out) {
  ConstByteView source;
  if (!buffer.try_subview(offset, sizeof(T), &source)) return false;
  return ByteView(reinterpret_cast<uint8_t*>(out), sizeof(T)).copy_from(source);   // (the one cast: a packed integer structure seen as its bytes)
}

}  // namespace dxva
}  // namespace hardware
