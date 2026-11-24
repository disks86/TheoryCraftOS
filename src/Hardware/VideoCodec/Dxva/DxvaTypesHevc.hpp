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
// The HEVC structures of dxva.h (DXVA_PicParams_HEVC, DXVA_Qmatrix_HEVC, DXVA_Slice_HEVC_Short), byte for byte as 64-bit and 32-bit Windows programs lay them out:
// dxva.h declares them inside #pragma pack(push, 1), so there is no padding and the layout is the same in both. The bit-field unions of dxva.h are kept here as the
// word they share (Windows compilers fill bit fields from the least significant bit up); the masks below name the fields. Checked against mingw-w64's dxva.h with
// x86_64-w64-mingw32-gcc and i686-w64-mingw32-gcc (sizes and the offset of every field).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hardware {
namespace dxva {

#pragma pack(push, 1)

// DXVA_PicEntry_HEVC: Index7Bits (bits 0-6, a surface index; 0x7F / 0xFF: no entry), AssociatedFlag (bit 7: a long-term reference picture in RefPicList).
struct PicParamsHevc {
  uint16_t PicWidthInMinCbsY;
  uint16_t PicHeightInMinCbsY;
  uint16_t wFormatAndSequenceInfoFlags;   // chroma_format_idc:2, separate_colour_plane_flag:1, bit_depth_luma_minus8:3, bit_depth_chroma_minus8:3,
                                          // log2_max_pic_order_cnt_lsb_minus4:4, NoPicReorderingFlag:1, NoBiPredFlag:1, ReservedBits1:1
  uint8_t CurrPic;
  uint8_t sps_max_dec_pic_buffering_minus1;
  uint8_t log2_min_luma_coding_block_size_minus3;
  uint8_t log2_diff_max_min_luma_coding_block_size;
  uint8_t log2_min_transform_block_size_minus2;
  uint8_t log2_diff_max_min_transform_block_size;
  uint8_t max_transform_hierarchy_depth_inter;
  uint8_t max_transform_hierarchy_depth_intra;
  uint8_t num_short_term_ref_pic_sets;
  uint8_t num_long_term_ref_pics_sps;
  uint8_t num_ref_idx_l0_default_active_minus1;
  uint8_t num_ref_idx_l1_default_active_minus1;
  int8_t init_qp_minus26;
  uint8_t ucNumDeltaPocsOfRefRpsIdx;
  uint16_t wNumBitsForShortTermRPSInSlice;
  uint16_t ReservedBits2;
  uint32_t dwCodingParamToolFlags;               // see kHevcTool*
  uint32_t dwCodingSettingPicturePropertyFlags;  // see kHevcPic*
  int8_t pps_cb_qp_offset;
  int8_t pps_cr_qp_offset;
  uint8_t num_tile_columns_minus1;
  uint8_t num_tile_rows_minus1;
  uint16_t column_width_minus1[19];
  uint16_t row_height_minus1[21];
  uint8_t diff_cu_qp_delta_depth;
  int8_t pps_beta_offset_div2;
  int8_t pps_tc_offset_div2;
  uint8_t log2_parallel_merge_level_minus2;
  int32_t CurrPicOrderCntVal;
  uint8_t RefPicList[15];
  uint8_t ReservedBits5;
  int32_t PicOrderCntValList[15];
  uint8_t RefPicSetStCurrBefore[8];   // indexes into RefPicList; 0xFF: no entry
  uint8_t RefPicSetStCurrAfter[8];
  uint8_t RefPicSetLtCurr[8];
  uint16_t ReservedBits6;
  uint16_t ReservedBits7;
  uint32_t StatusReportFeedbackNumber;
};

struct QmatrixHevc {
  uint8_t ucScalingLists0[6][16];   // 4x4, up-right diagonal scan order
  uint8_t ucScalingLists1[6][64];   // 8x8
  uint8_t ucScalingLists2[6][64];   // 16x16 (the 8x8 coded matrix)
  uint8_t ucScalingLists3[2][64];   // 32x32: matrixId 0 and (as Windows numbers them) 1 = the spec's 3
  uint8_t ucScalingListDCCoefSizeID2[6];
  uint8_t ucScalingListDCCoefSizeID3[2];
};

// DXVA_PicParams_HEVC_RangeExt (the DXVA HEVC range extension decoding specification; dxva.h): DXVA_PicParams_HEVC and then the SPS / PPS range extension fields,
// which the range extension profiles (DXVA_ModeHEVC_VLD_Main12 .. _Main16, _Monochrome, _Monochrome10) take as their picture parameter buffer.
struct PicParamsHevcRangeExt {
  PicParamsHevc params;
  uint16_t dwRangeExtensionFlags;                 // see hevc_rext
  uint8_t diff_cu_chroma_qp_offset_depth;
  uint8_t log2_sao_offset_scale_luma;
  uint8_t log2_sao_offset_scale_chroma;
  uint8_t log2_max_transform_skip_block_size_minus2;
  int8_t cb_qp_offset_list[6];
  int8_t cr_qp_offset_list[6];
  uint8_t chroma_qp_offset_list_len_minus1;
  uint16_t ReservedBits9;
};

struct SliceHevcShort {
  uint32_t BSNALunitDataLocation;
  uint32_t SliceBytesInBuffer;
  uint16_t wBadSliceChopping;   // 0 whole, 1 start only, 2 middle (neither start nor end), 3 end only
};

#pragma pack(pop)

static_assert(sizeof(PicParamsHevc) == 232, "DXVA_PicParams_HEVC is 232 bytes");
static_assert(offsetof(PicParamsHevc, CurrPic) == 6 && offsetof(PicParamsHevc, wNumBitsForShortTermRPSInSlice) == 20 &&
                  offsetof(PicParamsHevc, dwCodingParamToolFlags) == 24 && offsetof(PicParamsHevc, dwCodingSettingPicturePropertyFlags) == 28 &&
                  offsetof(PicParamsHevc, column_width_minus1) == 36 && offsetof(PicParamsHevc, row_height_minus1) == 74 &&
                  offsetof(PicParamsHevc, CurrPicOrderCntVal) == 120 && offsetof(PicParamsHevc, RefPicList) == 124 &&
                  offsetof(PicParamsHevc, PicOrderCntValList) == 140 && offsetof(PicParamsHevc, RefPicSetStCurrBefore) == 200 &&
                  offsetof(PicParamsHevc, StatusReportFeedbackNumber) == 228,
              "DXVA_PicParams_HEVC layout");
static_assert(sizeof(PicParamsHevcRangeExt) == 253 && offsetof(PicParamsHevcRangeExt, dwRangeExtensionFlags) == 232 &&
                  offsetof(PicParamsHevcRangeExt, cb_qp_offset_list) == 238 && offsetof(PicParamsHevcRangeExt, cr_qp_offset_list) == 244 &&
                  offsetof(PicParamsHevcRangeExt, chroma_qp_offset_list_len_minus1) == 250,
              "DXVA_PicParams_HEVC_RangeExt layout (mingw-w64's dxva.h: 253 bytes)");
static_assert(sizeof(QmatrixHevc) == 1000, "DXVA_Qmatrix_HEVC is 1000 bytes");
static_assert(sizeof(SliceHevcShort) == 10, "DXVA_Slice_HEVC_Short is 10 bytes");

// wFormatAndSequenceInfoFlags
constexpr uint32_t field(uint32_t word, int shift, int bits) { return (word >> shift) & ((1u << bits) - 1); }
namespace hevc_format {
constexpr int kChromaFormatIdc = 0, kSeparateColourPlane = 2, kBitDepthLumaMinus8 = 3, kBitDepthChromaMinus8 = 6, kLog2MaxPocLsbMinus4 = 9;
}
// dwCodingParamToolFlags: bit positions
namespace hevc_tool {
constexpr int kScalingListEnabled = 0, kAmp = 1, kSao = 2, kPcm = 3, kPcmBitDepthLumaMinus1 = 4, kPcmBitDepthChromaMinus1 = 8, kLog2MinPcmMinus3 = 12,
              kLog2DiffMaxMinPcm = 14, kPcmLoopFilterDisabled = 16, kLongTermRefPicsPresent = 17, kSpsTemporalMvp = 18, kStrongIntraSmoothing = 19,
              kDependentSliceSegments = 20, kOutputFlagPresent = 21, kNumExtraSliceHeaderBits = 22, kSignDataHiding = 25, kCabacInitPresent = 26;
}
// dwCodingSettingPicturePropertyFlags: bit positions
namespace hevc_pic {
constexpr int kConstrainedIntraPred = 0, kTransformSkip = 1, kCuQpDelta = 2, kSliceChromaQpOffsetsPresent = 3, kWeightedPred = 4, kWeightedBipred = 5,
              kTransquantBypass = 6, kTiles = 7, kEntropyCodingSync = 8, kUniformSpacing = 9, kLoopFilterAcrossTiles = 10, kLoopFilterAcrossSlices = 11,
              kDeblockingOverride = 12, kDeblockingDisabled = 13, kListsModificationPresent = 14, kSliceHeaderExtensionPresent = 15, kIrapPic = 16, kIdrPic = 17,
              kIntraPic = 18;
}

// DXVA_PicParams_HEVC_RangeExt::dwRangeExtensionFlags: bit positions (note the order: persistent_rice before high_precision_offsets, unlike the SPS)
namespace hevc_rext {
constexpr int kTransformSkipRotation = 0, kTransformSkipContext = 1, kImplicitRdpcm = 2, kExplicitRdpcm = 3, kExtendedPrecision = 4, kIntraSmoothingDisabled = 5,
              kPersistentRice = 6, kHighPrecisionOffsets = 7, kCabacBypassAlignment = 8, kCrossComponentPrediction = 9, kChromaQpOffsetList = 10;
}

}  // namespace dxva
}  // namespace hardware
