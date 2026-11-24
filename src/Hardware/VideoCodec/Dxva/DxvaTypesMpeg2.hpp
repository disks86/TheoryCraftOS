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
// DXVA's MPEG-1 / MPEG-2 structures (dxva.h, #pragma pack(1): the same bytes in 64-bit and 32-bit programs), as the program hands them over with
// D3D11_DECODER_PROFILE_MPEG2_VLD and D3D11_DECODER_PROFILE_MPEG2and1_VLD: DXVA_PictureParameters (the picture parameters buffer), DXVA_QmatrixData (the inverse
// quantization matrix buffer) and an array of DXVA_SliceInfo (the slice control buffer). The decoder reads them field by field from the untrusted buffer (read_*); the
// structures are also what a program (a test) fills.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Utility/ByteStream.hpp"

namespace hardware {
namespace dxva {

struct __attribute__((packed)) PictureParametersMpeg2 {   // DXVA_PictureParameters
  uint16_t wDecodedPictureIndex;
  uint16_t wDeblockedPictureIndex;
  uint16_t wForwardRefPictureIndex;    // 0xFFFF: none
  uint16_t wBackwardRefPictureIndex;
  uint16_t wPicWidthInMBminus1;
  uint16_t wPicHeightInMBminus1;       // of the picture: a field picture's is half the frame's
  uint8_t bMacroblockWidthMinus1;      // 15
  uint8_t bMacroblockHeightMinus1;     // 15
  uint8_t bBlockWidthMinus1;           // 7
  uint8_t bBlockHeightMinus1;          // 7
  uint8_t bBPPminus1;                  // 7
  uint8_t bPicStructure;               // 1 top field, 2 bottom field, 3 frame
  uint8_t bSecondField;
  uint8_t bPicIntra;
  uint8_t bPicBackwardPrediction;
  uint8_t bBidirectionalAveragingMode; // 0: MPEG-2 rounding of averaged predictions
  uint8_t bMVprecisionAndChromaRelation;
  uint8_t bChromaFormat;               // 1: 4:2:0
  uint8_t bPicScanFixed;
  uint8_t bPicScanMethod;              // 0 zigzag, 1 alternate vertical
  uint8_t bPicReadbackRequests;
  uint8_t bRcontrol;
  uint8_t bPicSpatialResid8;
  uint8_t bPicOverflowBlocks;
  uint8_t bPicExtrapolation;
  uint8_t bPicDeblocked;
  uint8_t bPicDeblockConfined;
  uint8_t bPic4MVallowed;
  uint8_t bPicOBMC;
  uint8_t bPicBinPB;
  uint8_t bMV_RPS;
  uint8_t bReservedBits;
  uint16_t wBitstreamFcodes;           // f_code[0][0] << 12 | f_code[0][1] << 8 | f_code[1][0] << 4 | f_code[1][1]
  uint16_t wBitstreamPCEelements;      // the picture coding extension's flags (kMpeg2Pce*)
  uint8_t bBitstreamConcealmentNeed;
  uint8_t bBitstreamConcealmentMethod;
};

struct __attribute__((packed)) SliceInfoMpeg2 {   // DXVA_SliceInfo
  uint16_t wHorizontalPosition;
  uint16_t wVerticalPosition;
  uint32_t dwSliceBitsInBuffer;
  uint32_t dwSliceDataLocation;        // the slice's start code, in bytes from the start of the bitstream buffer
  uint8_t bStartCodeBitOffset;
  uint8_t bReservedBits;
  uint16_t wMBbitOffset;
  uint16_t wNumberMBsInSlice;
  uint16_t wQuantizerScaleCode;
  uint16_t wBadSliceChopping;          // 0: the whole slice is in this buffer
};

struct QmatrixMpeg2 {   // DXVA_QmatrixData (naturally aligned: the words start at offset 4)
  uint8_t bNewQmatrix[4];              // intra luma, non-intra luma, intra chroma, non-intra chroma (0: keep the matrix in force)
  uint16_t Qmatrix[4][64];             // in zigzag scan order
};

// The same sizes and offsets as mingw-w64's dxva.h (checked with x86_64-w64-mingw32-gcc and i686-w64-mingw32-gcc).
static_assert(sizeof(PictureParametersMpeg2) == 44 && sizeof(SliceInfoMpeg2) == 22 && sizeof(QmatrixMpeg2) == 516);
static_assert(offsetof(PictureParametersMpeg2, wForwardRefPictureIndex) == 4 && offsetof(PictureParametersMpeg2, wPicWidthInMBminus1) == 8 &&
              offsetof(PictureParametersMpeg2, bBPPminus1) == 16 && offsetof(PictureParametersMpeg2, bPicStructure) == 17 && offsetof(PictureParametersMpeg2, bChromaFormat) == 23 &&
              offsetof(PictureParametersMpeg2, bPicScanMethod) == 25 && offsetof(PictureParametersMpeg2, bMV_RPS) == 36 && offsetof(PictureParametersMpeg2, wBitstreamFcodes) == 38 &&
              offsetof(PictureParametersMpeg2, wBitstreamPCEelements) == 40 && offsetof(PictureParametersMpeg2, bBitstreamConcealmentMethod) == 43);
static_assert(offsetof(SliceInfoMpeg2, dwSliceBitsInBuffer) == 4 && offsetof(SliceInfoMpeg2, dwSliceDataLocation) == 8 && offsetof(SliceInfoMpeg2, bStartCodeBitOffset) == 12 &&
              offsetof(SliceInfoMpeg2, wMBbitOffset) == 14 && offsetof(SliceInfoMpeg2, wQuantizerScaleCode) == 18 && offsetof(SliceInfoMpeg2, wBadSliceChopping) == 20);
static_assert(offsetof(QmatrixMpeg2, Qmatrix) == 4);

// wBitstreamPCEelements: intra_dc_precision in bits 15-14, picture_structure in 13-12, then one bit each (what ffmpeg's dxva2_mpeg2.c fills in).
constexpr uint16_t kMpeg2PceTopFieldFirst = 1u << 11, kMpeg2PceFramePredFrameDct = 1u << 10, kMpeg2PceConcealmentMotionVectors = 1u << 9, kMpeg2PceQScaleType = 1u << 8,
                   kMpeg2PceIntraVlcFormat = 1u << 7, kMpeg2PceAlternateScan = 1u << 6, kMpeg2PceRepeatFirstField = 1u << 5, kMpeg2PceChroma420Type = 1u << 4,
                   kMpeg2PceProgressiveFrame = 1u << 3;
// MPEG-1 pictures with D3D11_DECODER_PROFILE_MPEG2and1_VLD: an MPEG-1 picture has no picture coding extension, and a picture says so with wBitstreamPCEelements 0
// (picture_structure 0 is reserved in MPEG-2, so no MPEG-2 picture has it); its f_codes are then forward_f_code in the two high nibbles and backward_f_code in the
// two low ones.

// Reads the structures from a buffer (little-endian, packed). False if it is too short.
[[nodiscard]] inline bool read_mpeg2_picture_parameters(ConstByteView bytes, PictureParametersMpeg2* p) {
  ByteReader r(bytes);
  p->wDecodedPictureIndex = r.le16();
  p->wDeblockedPictureIndex = r.le16();
  p->wForwardRefPictureIndex = r.le16();
  p->wBackwardRefPictureIndex = r.le16();
  p->wPicWidthInMBminus1 = r.le16();
  p->wPicHeightInMBminus1 = r.le16();
  uint8_t* const bytes_fields[] = {&p->bMacroblockWidthMinus1, &p->bMacroblockHeightMinus1, &p->bBlockWidthMinus1, &p->bBlockHeightMinus1, &p->bBPPminus1,
                                   &p->bPicStructure, &p->bSecondField, &p->bPicIntra, &p->bPicBackwardPrediction, &p->bBidirectionalAveragingMode,
                                   &p->bMVprecisionAndChromaRelation, &p->bChromaFormat, &p->bPicScanFixed, &p->bPicScanMethod, &p->bPicReadbackRequests,
                                   &p->bRcontrol, &p->bPicSpatialResid8, &p->bPicOverflowBlocks, &p->bPicExtrapolation, &p->bPicDeblocked,
                                   &p->bPicDeblockConfined, &p->bPic4MVallowed, &p->bPicOBMC, &p->bPicBinPB, &p->bMV_RPS, &p->bReservedBits};
  for (uint8_t* field : bytes_fields) *field = r.u8();
  p->wBitstreamFcodes = r.le16();
  p->wBitstreamPCEelements = r.le16();
  p->bBitstreamConcealmentNeed = r.u8();
  p->bBitstreamConcealmentMethod = r.u8();
  return r.ok();
}

[[nodiscard]] inline bool read_mpeg2_slice_info(ByteReader& r, SliceInfoMpeg2* s) {
  s->wHorizontalPosition = r.le16();
  s->wVerticalPosition = r.le16();
  s->dwSliceBitsInBuffer = r.le32();
  s->dwSliceDataLocation = r.le32();
  s->bStartCodeBitOffset = r.u8();
  s->bReservedBits = r.u8();
  s->wMBbitOffset = r.le16();
  s->wNumberMBsInSlice = r.le16();
  s->wQuantizerScaleCode = r.le16();
  s->wBadSliceChopping = r.le16();
  return r.ok();
}

[[nodiscard]] inline bool read_mpeg2_qmatrix(ConstByteView bytes, QmatrixMpeg2* q) {
  ByteReader r(bytes);
  for (uint8_t& b : q->bNewQmatrix) b = r.u8();
  for (auto& matrix : q->Qmatrix)
    for (uint16_t& v : matrix) v = r.le16();
  return r.ok();
}

}  // namespace dxva
}  // namespace hardware
