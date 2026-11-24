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
// The AV1 structures of dxva.h (DXVA_PicEntry_AV1, DXVA_PicParams_AV1, DXVA_Tile_AV1), byte for byte as 64-bit and 32-bit Windows programs lay them out (dxva.h
// declares them inside #pragma pack(push, 1): no padding, the same layout in both). The bit-field unions are kept as the word they share (filled from the least
// significant bit up); the bit positions below name the fields. Checked against mingw-w64's dxva.h with x86_64-w64-mingw32-gcc and i686-w64-mingw32-gcc.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hardware {
namespace dxva {

#pragma pack(push, 1)

struct PicEntryAv1 {
  uint32_t width;
  uint32_t height;
  int32_t wmmat[6];
  uint8_t GlobalMotionFlags;   // wminvalid:1, wmtype:2
  uint8_t Index;               // frame_refs: which of RefFrameMapTextureIndex (0..7; 0xFF none)
  uint16_t Reserved16Bits;
};

struct PicParamsAv1 {
  uint32_t width;
  uint32_t height;
  uint32_t max_width;
  uint32_t max_height;
  uint8_t CurrPicTextureIndex;
  uint8_t superres_denom;
  uint8_t bitdepth;
  uint8_t seq_profile;
  struct {
    uint8_t cols;
    uint8_t rows;
    uint16_t context_update_id;
    uint16_t widths[64];    // in superblocks
    uint16_t heights[64];
  } tiles;
  uint32_t CodingParamToolFlags;       // see av1_coding
  uint8_t FormatAndPictureInfoFlags;   // frame_type:2, show_frame:1, showable_frame:1, subsampling_x:1, subsampling_y:1, mono_chrome:1
  uint8_t primary_ref_frame;
  uint8_t order_hint;
  uint8_t order_hint_bits;
  PicEntryAv1 frame_refs[7];
  uint8_t RefFrameMapTextureIndex[8];
  struct {
    uint8_t filter_level[2];
    uint8_t filter_level_u;
    uint8_t filter_level_v;
    uint8_t sharpness_level;
    uint8_t ControlFlags;   // mode_ref_delta_enabled:1, mode_ref_delta_update:1, delta_lf_multi:1, delta_lf_present:1
    int8_t ref_deltas[8];
    int8_t mode_deltas[2];
    uint8_t delta_lf_res;
    uint8_t frame_restoration_type[3];   // 0 none, 1 Wiener, 2 self-guided, 3 switchable
    uint16_t log2_restoration_unit_size[3];
    uint16_t Reserved16Bits;
  } loop_filter;
  struct {
    uint8_t ControlFlags;   // delta_q_present:1, delta_q_res:2
    uint8_t base_qindex;
    int8_t y_dc_delta_q;
    int8_t u_dc_delta_q;
    int8_t v_dc_delta_q;
    int8_t u_ac_delta_q;
    int8_t v_ac_delta_q;
    uint8_t qm_y;   // 0xFF: no quantizer matrix
    uint8_t qm_u;
    uint8_t qm_v;
    uint16_t Reserved16Bits;
  } quantization;
  struct {
    uint8_t ControlFlags;       // damping (minus 3):2, bits:2
    uint8_t y_strengths[8];     // primary:6, secondary:2
    uint8_t uv_strengths[8];
  } cdef;
  uint8_t interp_filter;
  struct {
    uint8_t ControlFlags;   // enabled:1, update_map:1, update_data:1, temporal_update:1
    uint8_t Reserved24Bits[3];
    uint8_t feature_mask[8];   // alt_q, alt_lf_y_v, alt_lf_y_h, alt_lf_u, alt_lf_v, ref_frame, skip, globalmv
    int16_t feature_data[8][8];
  } segmentation;
  struct {
    uint16_t ControlFlags;   // apply_grain:1, scaling_shift_minus8:2, chroma_scaling_from_luma:1, ar_coeff_lag:2, ar_coeff_shift_minus6:2, grain_scale_shift:2,
                             // overlap_flag:1, clip_to_restricted_range:1, matrix_coeff_is_identity:1
    uint16_t grain_seed;
    uint8_t scaling_points_y[14][2];
    uint8_t num_y_points;
    uint8_t scaling_points_cb[10][2];
    uint8_t num_cb_points;
    uint8_t scaling_points_cr[10][2];
    uint8_t num_cr_points;
    uint8_t ar_coeffs_y[24];    // plus 128
    uint8_t ar_coeffs_cb[25];
    uint8_t ar_coeffs_cr[25];
    uint8_t cb_mult;            // as coded (the multiplier plus 128)
    uint8_t cb_luma_mult;
    uint8_t cr_mult;
    uint8_t cr_luma_mult;
    uint8_t Reserved8Bits;
    int16_t cb_offset;          // as coded (the offset plus 256)
    int16_t cr_offset;
  } film_grain;
  uint32_t Reserved32Bits;
  uint32_t StatusReportFeedbackNumber;
};

struct TileAv1 {
  uint32_t DataOffset;
  uint32_t DataSize;
  uint16_t row;
  uint16_t column;
  uint16_t Reserved16Bits;
  uint8_t anchor_frame;   // 0xFF: not a large-scale tile
  uint8_t Reserved8Bits;
};

#pragma pack(pop)

static_assert(sizeof(PicEntryAv1) == 36, "DXVA_PicEntry_AV1 is 36 bytes");
static_assert(sizeof(PicParamsAv1) == 912, "DXVA_PicParams_AV1 is 912 bytes");
static_assert(offsetof(PicParamsAv1, tiles) == 20 && offsetof(PicParamsAv1, CodingParamToolFlags) == 280 && offsetof(PicParamsAv1, frame_refs) == 288 &&
                  offsetof(PicParamsAv1, RefFrameMapTextureIndex) == 540 && offsetof(PicParamsAv1, loop_filter) == 548 &&
                  offsetof(PicParamsAv1, quantization) == 576 && offsetof(PicParamsAv1, cdef) == 588 && offsetof(PicParamsAv1, interp_filter) == 605 &&
                  offsetof(PicParamsAv1, segmentation) == 606 && offsetof(PicParamsAv1, film_grain) == 746 && offsetof(PicParamsAv1, Reserved32Bits) == 904,
              "DXVA_PicParams_AV1 layout");
static_assert(sizeof(TileAv1) == 16, "DXVA_Tile_AV1 is 16 bytes");

// CodingParamToolFlags: bit positions
namespace av1_coding {
constexpr int kUse128x128Superblock = 0, kIntraEdgeFilter = 1, kInterintraCompound = 2, kMaskedCompound = 3, kWarpedMotion = 4, kDualFilter = 5, kJntComp = 6,
              kScreenContentTools = 7, kIntegerMv = 8, kCdef = 9, kRestoration = 10, kFilmGrain = 11, kIntrabc = 12, kHighPrecisionMv = 13,
              kSwitchableMotionMode = 14, kFilterIntra = 15, kDisableFrameEndUpdateCdf = 16, kDisableCdfUpdate = 17, kReferenceMode = 18, kSkipMode = 19,
              kReducedTxSet = 20, kSuperres = 21, kTxMode = 22 /* 2 bits */, kUseRefFrameMvs = 24, kEnableRefFrameMvs = 25, kReferenceFrameUpdate = 26;
}

}  // namespace dxva
}  // namespace hardware
