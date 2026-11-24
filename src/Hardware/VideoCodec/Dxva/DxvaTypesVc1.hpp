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
// DXVA's VC-1 structures (dxva.h; the DXVA specification for Windows Media Video v8, v9 and vA decoding, SMPTE 421M VC-1), as a program hands them over with
// D3D11_DECODER_PROFILE_VC1_VLD (DXVA_ModeVC1_D) and D3D11_DECODER_PROFILE_VC1_D2010: VC-1 uses MPEG-2's DXVA_PictureParameters and DXVA_SliceInfo (the same bytes,
// DxvaTypesMpeg2.hpp) with other meanings for many of the fields. What each means for VC-1 is below, as ffmpeg's libavcodec/dxva2_vc1.c fills them (what real programs
// send). The sequence and entry point layers of the stream are not sent: their fields are in the picture parameters.
#pragma once
#include <stdint.h>
#include "Hardware/VideoCodec/Dxva/DxvaTypesMpeg2.hpp"

namespace hardware {
namespace dxva {

using PictureParametersVc1 = PictureParametersMpeg2;   // DXVA_PictureParameters
using SliceInfoVc1 = SliceInfoMpeg2;                   // DXVA_SliceInfo

// wPicWidthInMBminus1 / wPicHeightInMBminus1: Simple and Main profile, the coded size in macroblocks minus 1; Advanced profile, the coded size in samples minus 1 (the
// entry point's CODED_WIDTH / CODED_HEIGHT, or the sequence's MAX_CODED_*).
// bPicStructure 3 (frame), 1 / 2 (a field of a field interlace frame); bSecondField: the second field.
// bPicIntra: an I or BI picture; bPicBackwardPrediction: a B picture.
// bBidirectionalAveragingMode:
constexpr uint8_t kVc1AveragingVc1Rounding = 1u << 7;         // always set
constexpr uint8_t kVc1AveragingIntraResidUnsigned = 1u << 6;  // ConfigIntraResidUnsigned of the configuration (IDCT modes; not VLD)
constexpr uint8_t kVc1AveragingResidAccel = 1u << 5;          // ConfigResidDiffAccelerator (IDCT modes)
constexpr uint8_t kVc1AveragingIntensityComp = 1u << 4;       // a P picture with intensity compensation (scale and shift not the identity)
constexpr uint8_t kVc1AveragingAdvanced = 1u << 3;            // the Advanced profile
// bMVprecisionAndChromaRelation: bit 3 the half sample bilinear mode (MVMODE 1MV-HPEL-BILINEAR), bit 2 set (VC-1 chroma rounding), bit 0 half sample precision.
// bPicScanFixed << 8 | bPicScanMethod: the status report feedback number (VC-1 has no scan choice to give).
// bRcontrol: the rounding control (RND) of the picture.
// bPicSpatialResid8:
constexpr uint8_t kVc1SpatialPanScan = 1u << 7, kVc1SpatialRefDist = 1u << 6, kVc1SpatialLoopFilter = 1u << 5, kVc1SpatialFastUvMc = 1u << 4,
                  kVc1SpatialExtendedMv = 1u << 3, kVc1SpatialVsTransform = 1u << 0;
constexpr int kVc1SpatialDquantShift = 1;                      // DQUANT, 2 bits
// bPicOverflowBlocks: QUANTIZER in bits 7-6, then MULTIRES, SYNCMARKER, RANGERED, and MAXBFRAMES in bits 2-0.
constexpr int kVc1OverflowQuantizerShift = 6;
constexpr uint8_t kVc1OverflowMultires = 1u << 5, kVc1OverflowSyncMarker = 1u << 4, kVc1OverflowRangeRed = 1u << 3;
constexpr uint8_t kVc1OverflowMaxBFramesMask = 7;
// bPicExtrapolation: 1 progressive, 2 interlaced (frame or field coded) picture.
// bPicDeblocked:
constexpr uint8_t kVc1DeblockedOverlap = 1u << 6;              // OVERLAP (not given for B pictures, which have none)
constexpr uint8_t kVc1DeblockedRangeRedFrame = 1u << 5;        // RANGEREDFRM of a Simple or Main profile picture
constexpr uint8_t kVc1DeblockedDeringing = 1u << 3, kVc1DeblockedDeblocking = 1u << 2;   // post-processing into wDeblockedPictureIndex
constexpr uint8_t kVc1DeblockedLoopFilter = 1u << 1;           // LOOPFILTER
// bPicDeblockConfined:
constexpr uint8_t kVc1ConfinedPostProc = 1u << 7, kVc1ConfinedPulldown = 1u << 6, kVc1ConfinedInterlace = 1u << 5, kVc1ConfinedTfcntr = 1u << 4,
                  kVc1ConfinedFinterp = 1u << 3, kVc1ConfinedReference = 1u << 2, kVc1ConfinedPsf = 1u << 1, kVc1ConfinedExtendedDmv = 1u << 0;
// bPic4MVallowed: the picture may have 4MV macroblocks. bPicOBMC (Advanced profile): RANGE_MAPY_FLAG << 7 | RANGE_MAPY << 4 | RANGE_MAPUV_FLAG << 3 | RANGE_MAPUV.
// bMV_RPS: field pictures (REFDIST + 9 for B fields). bReservedBits: PQUANT.
// wBitstreamFcodes / wBitstreamPCEelements: LUMSCALE / LUMSHIFT of intensity compensation (32 / 0 without), for fields top << 8 | bottom.
// DXVA_SliceInfo: wVerticalPosition the first macroblock row; dwSliceDataLocation / dwSliceBitsInBuffer the slice's bytes (Advanced profile: from its start code - 00 00 01
// 0D for the frame, 0C a second field, 0B a slice - which the program puts there; Simple and Main profile: the frame as stored); wMBbitOffset where the macroblock layer
// starts in it (0xFFFF: a skipped P picture); bReservedBits BFRACTION's index + 9 for B pictures; wQuantizerScaleCode PQUANT.

// The VC-1 status report (DXVA_Status_VC1; StatusReportFeedbackNumber is bPicScanFixed << 8 | bPicScanMethod of the picture).
struct __attribute__((packed)) StatusVc1 {
  uint16_t StatusReportFeedbackNumber;
  uint16_t wDecodedPictureIndex;
  uint16_t wDeblockedPictureIndex;
  uint8_t bPicStructure;
  uint8_t bBufType;
  uint8_t bStatus;
  uint8_t bReserved8Bits;
  uint16_t wNumMbsAffected;
};
static_assert(sizeof(StatusVc1) == 12);

}  // namespace dxva
}  // namespace hardware
