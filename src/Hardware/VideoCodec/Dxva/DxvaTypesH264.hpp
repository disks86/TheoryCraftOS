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
// DXVA's H.264 structures (dxva.h, #pragma pack(1): the same bytes in 64-bit and 32-bit programs), as a program hands them over with D3D11_DECODER_PROFILE_H264_VLD_NOFGT
// and the short slice format (ConfigBitstreamRaw 2): DXVA_PicParams_H264 (the picture parameters buffer), DXVA_Qmatrix_H264 (the inverse quantization matrix buffer) and
// an array of DXVA_Slice_H264_Short (the slice control buffer). The decoder reads them field by field from the untrusted buffer (read_*); the structures are also what a
// program (a test) fills. The bit fields of wBitFields are reached through the helpers below rather than C bit fields, whose layout is the compiler's.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Utility/ByteStream.hpp"

namespace hardware {
namespace dxva {

// DXVA_PicEntry_H264: Index7Bits (the surface) in bits 0..6, AssociatedFlag in bit 7 (CurrPic: bottom field; a reference: long-term). 0xFF: no picture.
constexpr uint8_t kH264NoPicture = 0xFF;
inline uint8_t h264_pic_index(uint8_t entry) { return entry & 0x7F; }
inline bool h264_pic_flag(uint8_t entry) { return (entry & 0x80) != 0; }

struct __attribute__((packed)) PicParamsH264 {   // DXVA_PicParams_H264
  uint16_t wFrameWidthInMbsMinus1;
  uint16_t wFrameHeightInMbsMinus1;     // of the frame (both fields)
  uint8_t CurrPic;                      // DXVA_PicEntry_H264
  uint8_t num_ref_frames;
  uint16_t wBitFields;                  // the H264_* bits below
  uint8_t bit_depth_luma_minus8;
  uint8_t bit_depth_chroma_minus8;
  uint16_t Reserved16Bits;
  uint32_t StatusReportFeedbackNumber;
  uint8_t RefFrameList[16];             // DXVA_PicEntry_H264 each
  int32_t CurrFieldOrderCnt[2];
  int32_t FieldOrderCntList[16][2];
  int8_t pic_init_qs_minus26;
  int8_t chroma_qp_index_offset;
  int8_t second_chroma_qp_index_offset;
  uint8_t ContinuationFlag;
  int8_t pic_init_qp_minus26;
  uint8_t num_ref_idx_l0_active_minus1;
  uint8_t num_ref_idx_l1_active_minus1;
  uint8_t Reserved8BitsA;
  uint16_t FrameNumList[16];            // FrameNum, or LongTermFrameIdx of a long-term reference
  uint32_t UsedForReferenceFlags;       // two bits per entry: top field, bottom field
  uint16_t NonExistingFrameFlags;
  uint16_t frame_num;
  uint8_t log2_max_frame_num_minus4;
  uint8_t pic_order_cnt_type;
  uint8_t log2_max_pic_order_cnt_lsb_minus4;
  uint8_t delta_pic_order_always_zero_flag;
  uint8_t direct_8x8_inference_flag;
  uint8_t entropy_coding_mode_flag;
  uint8_t pic_order_present_flag;       // bottom_field_pic_order_in_frame_present_flag
  uint8_t num_slice_groups_minus1;
  uint8_t slice_group_map_type;
  uint8_t deblocking_filter_control_present_flag;
  uint8_t redundant_pic_cnt_present_flag;
  uint8_t Reserved8BitsB;
  uint16_t slice_group_change_rate_minus1;
  uint8_t SliceGroupMap[810];
};
static_assert(sizeof(PicParamsH264) == 1040, "DXVA_PicParams_H264");
static_assert(offsetof(PicParamsH264, RefFrameList) == 16 && offsetof(PicParamsH264, CurrFieldOrderCnt) == 32 && offsetof(PicParamsH264, FieldOrderCntList) == 40,
              "DXVA_PicParams_H264");
static_assert(offsetof(PicParamsH264, pic_init_qs_minus26) == 168 && offsetof(PicParamsH264, FrameNumList) == 176 && offsetof(PicParamsH264, UsedForReferenceFlags) == 208,
              "DXVA_PicParams_H264");
static_assert(offsetof(PicParamsH264, frame_num) == 214 && offsetof(PicParamsH264, slice_group_change_rate_minus1) == 228 && offsetof(PicParamsH264, SliceGroupMap) == 230,
              "DXVA_PicParams_H264");

// wBitFields
enum : uint16_t {
  kH264FieldPic = 1u << 0,
  kH264MbaffFrame = 1u << 1,
  kH264ResidualColourTransform = 1u << 2,
  kH264SpForSwitch = 1u << 3,
  kH264ChromaFormatShift = 4,            // two bits: chroma_format_idc
  kH264RefPic = 1u << 6,
  kH264ConstrainedIntraPred = 1u << 7,
  kH264WeightedPred = 1u << 8,
  kH264WeightedBipredShift = 9,          // two bits: weighted_bipred_idc
  kH264MbsConsecutive = 1u << 11,
  kH264FrameMbsOnly = 1u << 12,
  kH264Transform8x8 = 1u << 13,
  kH264MinLumaBipredSize8x8 = 1u << 14,
  kH264IntraPic = 1u << 15,
};

struct __attribute__((packed)) QmatrixH264 {   // DXVA_Qmatrix_H264: the lists in the order the bitstream sends them (zigzag scan)
  uint8_t bScalingLists4x4[6][16];
  uint8_t bScalingLists8x8[2][64];        // intra Y, inter Y
};
static_assert(sizeof(QmatrixH264) == 224, "DXVA_Qmatrix_H264");

struct __attribute__((packed)) SliceH264Short {   // DXVA_Slice_H264_Short
  uint32_t BSNALunitDataLocation;         // where the slice's NAL unit (with its start code) is in the bitstream buffer
  uint32_t SliceBytesInBuffer;
  uint16_t wBadSliceChopping;             // 0: the whole slice is in the buffer
};
static_assert(sizeof(SliceH264Short) == 10 && offsetof(SliceH264Short, wBadSliceChopping) == 8, "DXVA_Slice_H264_Short");

[[nodiscard]] inline bool read_h264_picture_parameters(ConstByteView bytes, PicParamsH264* p) {
  ByteReader r(bytes);
  p->wFrameWidthInMbsMinus1 = r.le16();
  p->wFrameHeightInMbsMinus1 = r.le16();
  p->CurrPic = r.u8();
  p->num_ref_frames = r.u8();
  p->wBitFields = r.le16();
  p->bit_depth_luma_minus8 = r.u8();
  p->bit_depth_chroma_minus8 = r.u8();
  p->Reserved16Bits = r.le16();
  p->StatusReportFeedbackNumber = r.le32();
  for (uint8_t& e : p->RefFrameList) e = r.u8();
  // (index loops: a reference cannot bind to a member of a packed structure)
  for (int i = 0; i < 2; ++i) p->CurrFieldOrderCnt[i] = static_cast<int32_t>(r.le32());
  for (int i = 0; i < 16; ++i)
    for (int k = 0; k < 2; ++k) p->FieldOrderCntList[i][k] = static_cast<int32_t>(r.le32());
  p->pic_init_qs_minus26 = static_cast<int8_t>(r.u8());
  p->chroma_qp_index_offset = static_cast<int8_t>(r.u8());
  p->second_chroma_qp_index_offset = static_cast<int8_t>(r.u8());
  p->ContinuationFlag = r.u8();
  p->pic_init_qp_minus26 = static_cast<int8_t>(r.u8());
  p->num_ref_idx_l0_active_minus1 = r.u8();
  p->num_ref_idx_l1_active_minus1 = r.u8();
  p->Reserved8BitsA = r.u8();
  for (int i = 0; i < 16; ++i) p->FrameNumList[i] = r.le16();
  p->UsedForReferenceFlags = r.le32();
  p->NonExistingFrameFlags = r.le16();
  p->frame_num = r.le16();
  uint8_t* const byte_fields[] = {&p->log2_max_frame_num_minus4, &p->pic_order_cnt_type, &p->log2_max_pic_order_cnt_lsb_minus4, &p->delta_pic_order_always_zero_flag,
                                  &p->direct_8x8_inference_flag, &p->entropy_coding_mode_flag, &p->pic_order_present_flag, &p->num_slice_groups_minus1,
                                  &p->slice_group_map_type, &p->deblocking_filter_control_present_flag, &p->redundant_pic_cnt_present_flag, &p->Reserved8BitsB};
  for (uint8_t* f : byte_fields) *f = r.u8();
  p->slice_group_change_rate_minus1 = r.le16();
  for (uint8_t& b : p->SliceGroupMap) b = r.u8();
  return r.ok();
}

[[nodiscard]] inline bool read_h264_qmatrix(ConstByteView bytes, QmatrixH264* q) {
  ByteReader r(bytes);
  for (auto& list : q->bScalingLists4x4)
    for (uint8_t& v : list) v = r.u8();
  for (auto& list : q->bScalingLists8x8)
    for (uint8_t& v : list) v = r.u8();
  return r.ok();
}

[[nodiscard]] inline bool read_h264_slice(ByteReader& r, SliceH264Short* s) {
  s->BSNALunitDataLocation = r.le32();
  s->SliceBytesInBuffer = r.le32();
  s->wBadSliceChopping = r.le16();
  return r.ok();
}

}  // namespace dxva
}  // namespace hardware
