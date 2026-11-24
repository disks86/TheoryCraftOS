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
// DXVA's MPEG-4 Part 2 structures (dxva.h of the Windows SDK; the DXVA specification of off-host VLD mode for MPEG-4 Part 2 video decoding), as a program hands them
// over with D3D11_DECODER_PROFILE_MPEG4PT2_VLD_SIMPLE, _ADVSIMPLE_NOGMC and _ADVSIMPLE_GMC: DXVA_PicParams_MPEG4_PART2 (the picture parameters), DXVA_QmatrixData (the
// quantiser matrices, as for MPEG-2: in zigzag scan order, DxvaTypesMpeg2.hpp) and an array of DXVA_SliceInfo (the slice control). mingw-w64's dxva.h does not have
// the picture parameters; the layout is the SDK's (#pragma pack(1): the same bytes in 64-bit and 32-bit programs). The decoder reads the fields from the untrusted
// buffer (read_*); the structure is also what a program (a test) fills.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Hardware/VideoCodec/Dxva/DxvaTypesMpeg2.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {
namespace dxva {

struct __attribute__((packed)) PicParamsMpeg4Part2 {   // DXVA_PicParams_MPEG4_PART2
  uint8_t short_video_header;          // 1: an H.263 baseline picture (the short video header)
  uint8_t vop_coding_type;             // 0 I, 1 P, 2 B, 3 S (GMC; also sent as 1 by a host that follows the specification's "shall not be 3": DxvaMpeg4.cpp)
  uint8_t vop_quant;
  uint16_t wDecodedPictureIndex;
  uint16_t wDeblockedPictureIndex;
  uint16_t wForwardRefPictureIndex;    // 0xFFFF: none
  uint16_t wBackwardRefPictureIndex;
  uint16_t vop_time_increment_resolution;
  uint32_t TRB[2];                     // B: the distances of direct mode; [0]: of frame direct mode, in ticks of the time base or (the specification's text)
                                       // in field periods; [1]: of field direct mode (interlaced), in field periods (DxvaMpeg4.cpp)
  uint32_t TRD[2];
  uint16_t wPicFlagBitFields;          // kMpeg4Pic*
  uint8_t profile_and_level_indication;
  uint8_t video_object_layer_verid;
  uint16_t vop_width;                  // samples
  uint16_t vop_height;
  uint16_t wSpriteBitFields;           // sprite_enable 1-0 (1 static: refused, 2 GMC), no_of_sprite_warping_points 7-2, sprite_warping_accuracy 9-8
  int16_t warping_mv[4][2];            // S: the sprite trajectory's du, dv per warping point, as coded
  uint8_t wFcodeBitFields;             // vop_fcode_forward 2-0, vop_fcode_backward 5-3
  uint16_t StatusReportFeedbackNumber;
  uint16_t Reserved16BitsA;
  uint16_t Reserved16BitsB;
};

// wPicFlagBitFields (the bit fields of the union, from bit 0).
constexpr uint16_t kMpeg4PicPostProcMask = 3;              // unPicPostProc: post-processing into wDeblockedPictureIndex (0 none)
constexpr uint16_t kMpeg4PicInterlaced = 1u << 2, kMpeg4PicQuantType = 1u << 3, kMpeg4PicQuarterSample = 1u << 4, kMpeg4PicResyncMarkerDisable = 1u << 5,
                   kMpeg4PicDataPartitioned = 1u << 6, kMpeg4PicReversibleVlc = 1u << 7, kMpeg4PicReducedResolution = 1u << 8, kMpeg4PicVopCoded = 1u << 9,
                   kMpeg4PicRoundingType = 1u << 10, kMpeg4PicTopFieldFirst = 1u << 14, kMpeg4PicAlternateVerticalScan = 1u << 15;
constexpr int kMpeg4PicIntraDcVlcThrShift = 11;            // intra_dc_vlc_thr, 3 bits
constexpr uint16_t kMpeg4SpriteEnableMask = 3;             // 0 none, 1 static, 2 GMC
constexpr int kMpeg4SpritePointsShift = 2, kMpeg4SpriteAccuracyShift = 8;

// The same size and offsets as the Windows SDK's dxva.h (checked with x86_64-w64-mingw32-gcc and i686-w64-mingw32-gcc on the SDK's declaration).
static_assert(sizeof(PicParamsMpeg4Part2) == 62);
static_assert(offsetof(PicParamsMpeg4Part2, wDecodedPictureIndex) == 3 && offsetof(PicParamsMpeg4Part2, vop_time_increment_resolution) == 11 &&
              offsetof(PicParamsMpeg4Part2, TRB) == 13 && offsetof(PicParamsMpeg4Part2, TRD) == 21 && offsetof(PicParamsMpeg4Part2, wPicFlagBitFields) == 29 &&
              offsetof(PicParamsMpeg4Part2, vop_width) == 33 && offsetof(PicParamsMpeg4Part2, wSpriteBitFields) == 37 && offsetof(PicParamsMpeg4Part2, warping_mv) == 39 &&
              offsetof(PicParamsMpeg4Part2, wFcodeBitFields) == 55 && offsetof(PicParamsMpeg4Part2, StatusReportFeedbackNumber) == 56);

[[nodiscard]] inline bool read_mpeg4_picture_parameters(ConstByteView bytes, PicParamsMpeg4Part2* p) {
  ByteReader r(bytes);
  p->short_video_header = r.u8();
  p->vop_coding_type = r.u8();
  p->vop_quant = r.u8();
  p->wDecodedPictureIndex = r.le16();
  p->wDeblockedPictureIndex = r.le16();
  p->wForwardRefPictureIndex = r.le16();
  p->wBackwardRefPictureIndex = r.le16();
  p->vop_time_increment_resolution = r.le16();
  // (index loops: the fields are packed, so no reference to their elements may be taken)
  for (int i = 0; i < 2; ++i) p->TRB[i] = r.le32();
  for (int i = 0; i < 2; ++i) p->TRD[i] = r.le32();
  p->wPicFlagBitFields = r.le16();
  p->profile_and_level_indication = r.u8();
  p->video_object_layer_verid = r.u8();
  p->vop_width = r.le16();
  p->vop_height = r.le16();
  p->wSpriteBitFields = r.le16();
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 2; ++c) p->warping_mv[i][c] = static_cast<int16_t>(r.le16());
  p->wFcodeBitFields = r.u8();
  p->StatusReportFeedbackNumber = r.le16();
  p->Reserved16BitsA = r.le16();
  p->Reserved16BitsB = r.le16();
  return r.ok();
}

}  // namespace dxva
}  // namespace hardware
