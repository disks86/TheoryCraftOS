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
#include "Utility/ArrayView.hpp"
#include "Utility/Bounds.hpp"

// Shared types of the AV1 decoder (Av1*.cpp). The header structures, the block and transform enumerations and the layout of the
// tables follow dav1d (BSD-2-Clause, Copyright (c) VideoLAN and dav1d authors), the reference the decoder was checked against.
// Pictures are always held as 16-bit samples, whatever the bit depth is; kernels take the bit depth as a parameter.

namespace hardware {
namespace av1 {

// ---- syntax-level constants and the sequence and frame headers ---------------------------------------------------------------------
constexpr int MAX_CDEF_STRENGTHS = 8;
constexpr int MAX_OPERATING_POINTS = 32;
constexpr int MAX_TILE_COLS = 64;
constexpr int MAX_TILE_ROWS = 64;
constexpr int MAX_SEGMENTS = 8;
constexpr int NUM_REF_FRAMES = 8;
constexpr int PRIMARY_REF_NONE = 7;
constexpr int REFS_PER_FRAME = 7;
constexpr int TOTAL_REFS_PER_FRAME = 8;

enum ObuType {
    OBU_SEQ_HDR   = 1,
    OBU_TD        = 2,
    OBU_FRAME_HDR = 3,
    OBU_TILE_GRP  = 4,
    OBU_METADATA  = 5,
    OBU_FRAME     = 6,
    OBU_REDUNDANT_FRAME_HDR = 7,
    OBU_PADDING   = 15,
};

enum TxfmMode {
    TX_4X4_ONLY,
    TX_LARGEST,
    TX_SWITCHABLE,
    N_TX_MODES,
};

enum FilterMode {
    FILTER_8TAP_REGULAR,
    FILTER_8TAP_SMOOTH,
    FILTER_8TAP_SHARP,
    N_SWITCHABLE_FILTERS,
    FILTER_BILINEAR = N_SWITCHABLE_FILTERS,
    N_FILTERS,
    FILTER_SWITCHABLE = N_FILTERS,
};

enum AdaptiveBoolean {
    OFF = 0,
    ON = 1,
    ADAPTIVE = 2,
};

enum RestorationType {
    RESTORATION_NONE,
    RESTORATION_SWITCHABLE,
    RESTORATION_WIENER,
    RESTORATION_SGRPROJ,
};

enum WarpedMotionType {
    WM_TYPE_IDENTITY,
    WM_TYPE_TRANSLATION,
    WM_TYPE_ROT_ZOOM,
    WM_TYPE_AFFINE,
};

struct WarpedMotionParams {
    WarpedMotionType type;
    int32_t matrix[6];
    union {
        struct {
            int16_t alpha, beta, gamma, delta;
        } p;
        int16_t abcd[4];
    } u;
};

enum PixelLayout {
    PIXEL_LAYOUT_I400, ///< monochrome
    PIXEL_LAYOUT_I420, ///< 4:2:0 planar
    PIXEL_LAYOUT_I422, ///< 4:2:2 planar
    PIXEL_LAYOUT_I444, ///< 4:4:4 planar
};

enum FrameType {
    FRAME_TYPE_KEY = 0,    ///< Key Intra frame
    FRAME_TYPE_INTER = 1,  ///< Inter frame
    FRAME_TYPE_INTRA = 2,  ///< Non key Intra frame
    FRAME_TYPE_SWITCH = 3, ///< Switch Inter frame
};

enum ColorPrimaries {
    COLOR_PRI_BT709 = 1,
    COLOR_PRI_UNKNOWN = 2,
    COLOR_PRI_BT470M = 4,
    COLOR_PRI_BT470BG = 5,
    COLOR_PRI_BT601 = 6,
    COLOR_PRI_SMPTE240 = 7,
    COLOR_PRI_FILM = 8,
    COLOR_PRI_BT2020 = 9,
    COLOR_PRI_XYZ = 10,
    COLOR_PRI_SMPTE431 = 11,
    COLOR_PRI_SMPTE432 = 12,
    COLOR_PRI_EBU3213 = 22,
    COLOR_PRI_RESERVED = 255,
};

enum TransferCharacteristics {
    TRC_BT709 = 1,
    TRC_UNKNOWN = 2,
    TRC_BT470M = 4,
    TRC_BT470BG = 5,
    TRC_BT601 = 6,
    TRC_SMPTE240 = 7,
    TRC_LINEAR = 8,
    TRC_LOG100 = 9,         ///< logarithmic (100:1 range)
    TRC_LOG100_SQRT10 = 10, ///< lograithmic (100*sqrt(10):1 range)
    TRC_IEC61966 = 11,
    TRC_BT1361 = 12,
    TRC_SRGB = 13,
    TRC_BT2020_10BIT = 14,
    TRC_BT2020_12BIT = 15,
    TRC_SMPTE2084 = 16,     ///< PQ
    TRC_SMPTE428 = 17,
    TRC_HLG = 18,           ///< hybrid log/gamma (BT.2100 / ARIB STD-B67)
    TRC_RESERVED = 255,
};

enum MatrixCoefficients {
    MC_IDENTITY = 0,
    MC_BT709 = 1,
    MC_UNKNOWN = 2,
    MC_FCC = 4,
    MC_BT470BG = 5,
    MC_BT601 = 6,
    MC_SMPTE240 = 7,
    MC_SMPTE_YCGCO = 8,
    MC_BT2020_NCL = 9,
    MC_BT2020_CL = 10,
    MC_SMPTE2085 = 11,
    MC_CHROMAT_NCL = 12, ///< Chromaticity-derived
    MC_CHROMAT_CL = 13,
    MC_ICTCP = 14,
    MC_RESERVED = 255,
};

enum ChromaSamplePosition {
    CHR_UNKNOWN = 0,
    CHR_VERTICAL = 1,  ///< Horizontally co-located with luma(0, 0)
                           ///< sample, between two vertical samples
    CHR_COLOCATED = 2, ///< Co-located with luma(0, 0) sample
};

struct ContentLightLevel {
    uint16_t max_content_light_level;
    uint16_t max_frame_average_light_level;
};

struct MasteringDisplay {
    uint16_t primaries[3][2]; ///< 0.16 fixed point
    uint16_t white_point[2]; ///< 0.16 fixed point
    uint32_t max_luminance; ///< 24.8 fixed point
    uint32_t min_luminance; ///< 18.14 fixed point
};


struct SequenceHeader {
    /**
     * Stream profile, 0 for 8-10 bits/component 4:2:0 or monochrome;
     * 1 for 8-10 bits/component 4:4:4; 2 for 4:2:2 at any bits/component,
     * or 12 bits/component at any chroma subsampling.
     */
    uint8_t profile;
    /**
     * Maximum dimensions for this stream. In non-scalable streams, these
     * are often the actual dimensions of the stream, although that is not
     * a normative requirement.
     */
    int max_width, max_height;
    PixelLayout layout; ///< format of the picture
    ColorPrimaries pri; ///< color primaries (av1)
    TransferCharacteristics trc; ///< transfer characteristics (av1)
    MatrixCoefficients mtrx; ///< matrix coefficients (av1)
    ChromaSamplePosition chr; ///< chroma sample position (av1)
    /**
     * 0, 1 and 2 mean 8, 10 or 12 bits/component, respectively. This is not
     * exactly the same as 'hbd' from the spec; the spec's hbd distinguishes
     * between 8 (0) and 10-12 (1) bits/component, and another element
     * (twelve_bit) to distinguish between 10 and 12 bits/component. To get
     * the spec's hbd, use !!our_hbd, and to get twelve_bit, use hbd == 2.
     */
    uint8_t hbd;
    /**
     * Pixel data uses JPEG pixel range ([0,255] for 8bits) instead of
     * MPEG pixel range ([16,235] for 8bits luma, [16,240] for 8bits chroma).
     */
    uint8_t color_range;

    uint8_t num_operating_points;
    struct SequenceHeaderOperatingPoint {
        uint8_t major_level, minor_level;
        uint8_t initial_display_delay;
        uint16_t idc;
        uint8_t tier;
        uint8_t decoder_model_param_present;
        uint8_t display_model_param_present;
    } operating_points[MAX_OPERATING_POINTS];

    uint8_t still_picture;
    uint8_t reduced_still_picture_header;
    uint8_t timing_info_present;
    uint32_t num_units_in_tick;
    uint32_t time_scale;
    uint8_t equal_picture_interval;
    uint32_t num_ticks_per_picture;
    uint8_t decoder_model_info_present;
    uint8_t encoder_decoder_buffer_delay_length;
    uint32_t num_units_in_decoding_tick;
    uint8_t buffer_removal_delay_length;
    uint8_t frame_presentation_delay_length;
    uint8_t display_model_info_present;
    uint8_t width_n_bits, height_n_bits;
    uint8_t frame_id_numbers_present;
    uint8_t delta_frame_id_n_bits;
    uint8_t frame_id_n_bits;
    uint8_t sb128;
    uint8_t filter_intra;
    uint8_t intra_edge_filter;
    uint8_t inter_intra;
    uint8_t masked_compound;
    uint8_t warped_motion;
    uint8_t dual_filter;
    uint8_t order_hint;
    uint8_t jnt_comp;
    uint8_t ref_frame_mvs;
    AdaptiveBoolean screen_content_tools;
    AdaptiveBoolean force_integer_mv;
    uint8_t order_hint_n_bits;
    uint8_t super_res;
    uint8_t cdef;
    uint8_t restoration;
    uint8_t ss_hor, ss_ver, monochrome;
    uint8_t color_description_present;
    uint8_t separate_uv_delta_q;
    uint8_t film_grain_present;

    // SequenceHeaders of the same sequence are required to be
    // bit-identical until this offset. See 7.5 "Ordering of OBUs":
    //   Within a particular coded video sequence, the contents of
    //   sequence_header_obu must be bit-identical each time the
    //   sequence header appears except for the contents of
    //   operating_parameters_info.
    struct SequenceHeaderOperatingParameterInfo {
        uint32_t decoder_buffer_delay;
        uint32_t encoder_buffer_delay;
        uint8_t low_delay_mode;
    } operating_parameter_info[MAX_OPERATING_POINTS];
};

struct SegmentationData {
    int16_t delta_q;
    int8_t delta_lf_y_v, delta_lf_y_h, delta_lf_u, delta_lf_v;
    int8_t ref;
    uint8_t skip;
    uint8_t globalmv;
};

struct SegmentationDataSet {
    SegmentationData d[MAX_SEGMENTS];
    uint8_t preskip;
    int8_t last_active_segid;
};

struct LoopfilterModeRefDeltas {
    int8_t mode_delta[2 /* is_zeromv */];
    int8_t ref_delta[TOTAL_REFS_PER_FRAME];
};

struct FilmGrainData {
    unsigned seed;
    int num_y_points;
    uint8_t y_points[14][2 /* value, scaling */];
    int chroma_scaling_from_luma;
    int num_uv_points[2];
    uint8_t uv_points[2][10][2 /* value, scaling */];
    int scaling_shift;
    int ar_coeff_lag;
    int8_t ar_coeffs_y[24];
    int8_t ar_coeffs_uv[2][25 + 3 /* padding for alignment purposes */];
    uint64_t ar_coeff_shift;
    int grain_scale_shift;
    int uv_mult[2];
    int uv_luma_mult[2];
    int uv_offset[2];
    int overlap_flag;
    int clip_to_restricted_range;
};

struct FrameHeader {
    struct {
        FilmGrainData data;
        uint8_t present, update;
    } film_grain; ///< film grain parameters
    FrameType frame_type; ///< type of the picture
    int width[2 /* { coded_width, superresolution_upscaled_width } */], height;
    uint8_t frame_offset; ///< frame number
    uint8_t temporal_id; ///< temporal id of the frame for SVC
    uint8_t spatial_id; ///< spatial id of the frame for SVC

    uint8_t show_existing_frame;
    uint8_t existing_frame_idx;
    uint32_t frame_id;
    uint32_t frame_presentation_delay;
    uint8_t show_frame;
    uint8_t showable_frame;
    uint8_t error_resilient_mode;
    uint8_t disable_cdf_update;
    uint8_t allow_screen_content_tools;
    uint8_t force_integer_mv;
    uint8_t frame_size_override;
    uint8_t primary_ref_frame;
    uint8_t buffer_removal_time_present;
    struct FrameHeaderOperatingPoint {
        uint32_t buffer_removal_time;
    } operating_points[MAX_OPERATING_POINTS];
    uint8_t refresh_frame_flags;
    int render_width, render_height;
    struct {
        uint8_t width_scale_denominator;
        uint8_t enabled;
    } super_res;
    uint8_t have_render_size;
    uint8_t allow_intrabc;
    uint8_t frame_ref_short_signaling;
    int8_t refidx[REFS_PER_FRAME];
    uint8_t hp;
    FilterMode subpel_filter_mode;
    uint8_t switchable_motion_mode;
    uint8_t use_ref_frame_mvs;
    uint8_t refresh_context;
    struct {
        uint8_t uniform;
        uint8_t n_bytes;
        uint8_t min_log2_cols, max_log2_cols, log2_cols, cols;
        uint8_t min_log2_rows, max_log2_rows, log2_rows, rows;
        uint16_t col_start_sb[MAX_TILE_COLS + 1];
        uint16_t row_start_sb[MAX_TILE_ROWS + 1];
        uint16_t update;
    } tiling;
    struct {
        uint8_t yac;
        int8_t ydc_delta;
        int8_t udc_delta, uac_delta, vdc_delta, vac_delta;
        uint8_t qm, qm_y, qm_u, qm_v;
    } quant;
    struct {
        uint8_t enabled, update_map, temporal, update_data;
        SegmentationDataSet seg_data;
        uint8_t lossless[MAX_SEGMENTS], qidx[MAX_SEGMENTS];
    } segmentation;
    struct {
        struct {
            uint8_t present;
            uint8_t res_log2;
        } q;
        struct {
            uint8_t present;
            uint8_t res_log2;
            uint8_t multi;
        } lf;
    } delta;
    uint8_t all_lossless;
    struct {
        uint8_t level_y[2 /* dir */];
        uint8_t level_u, level_v;
        uint8_t mode_ref_delta_enabled;
        uint8_t mode_ref_delta_update;
        LoopfilterModeRefDeltas mode_ref_deltas;
        uint8_t sharpness;
    } loopfilter;
    struct {
        uint8_t damping;
        uint8_t n_bits;
        uint8_t y_strength[MAX_CDEF_STRENGTHS];
        uint8_t uv_strength[MAX_CDEF_STRENGTHS];
    } cdef;
    struct {
        enum RestorationType type[3 /* plane */];
        uint8_t unit_size[2 /* y, uv */];
    } restoration;
    TxfmMode txfm_mode;
    uint8_t switchable_comp_refs;
    uint8_t skip_mode_allowed, skip_mode_enabled;
    int8_t skip_mode_refs[2];
    uint8_t warp_motion;
    uint8_t reduced_txtp_set;
    WarpedMotionParams gmv[REFS_PER_FRAME];
};


// ---- blocks, transforms and prediction modes ----
enum ObuMetaType {
    OBU_META_HDR_CLL     = 1,
    OBU_META_HDR_MDCV    = 2,
    OBU_META_SCALABILITY = 3,
    OBU_META_ITUT_T35    = 4,
    OBU_META_TIMECODE    = 5,
};

enum TxfmSize {
    TX_4X4,
    TX_8X8,
    TX_16X16,
    TX_32X32,
    TX_64X64,
    N_TX_SIZES,
};

enum BlockLevel {
    BL_128X128,
    BL_64X64,
    BL_32X32,
    BL_16X16,
    BL_8X8,
    N_BL_LEVELS,
};

enum RectTxfmSize {
    RTX_4X8 = N_TX_SIZES,
    RTX_8X4,
    RTX_8X16,
    RTX_16X8,
    RTX_16X32,
    RTX_32X16,
    RTX_32X64,
    RTX_64X32,
    RTX_4X16,
    RTX_16X4,
    RTX_8X32,
    RTX_32X8,
    RTX_16X64,
    RTX_64X16,
    N_RECT_TX_SIZES
};

enum TxfmType {
    DCT_DCT,    // DCT  in both horizontal and vertical
    ADST_DCT,   // ADST in vertical, DCT in horizontal
    DCT_ADST,   // DCT  in vertical, ADST in horizontal
    ADST_ADST,  // ADST in both directions
    FLIPADST_DCT,
    DCT_FLIPADST,
    FLIPADST_FLIPADST,
    ADST_FLIPADST,
    FLIPADST_ADST,
    IDTX,
    V_DCT,
    H_DCT,
    V_ADST,
    H_ADST,
    V_FLIPADST,
    H_FLIPADST,
    N_TX_TYPES,
    WHT_WHT = N_TX_TYPES,
    N_TX_TYPES_PLUS_LL,
};

enum TxClass {
    TX_CLASS_2D,
    TX_CLASS_H,
    TX_CLASS_V,
};

enum IntraPredMode {
    DC_PRED,
    VERT_PRED,
    HOR_PRED,
    DIAG_DOWN_LEFT_PRED,
    DIAG_DOWN_RIGHT_PRED,
    VERT_RIGHT_PRED,
    HOR_DOWN_PRED,
    HOR_UP_PRED,
    VERT_LEFT_PRED,
    SMOOTH_PRED,
    SMOOTH_V_PRED,
    SMOOTH_H_PRED,
    PAETH_PRED,
    N_INTRA_PRED_MODES,
    CFL_PRED = N_INTRA_PRED_MODES,
    N_UV_INTRA_PRED_MODES,
    N_IMPL_INTRA_PRED_MODES = N_UV_INTRA_PRED_MODES,
    LEFT_DC_PRED = DIAG_DOWN_LEFT_PRED,
    TOP_DC_PRED,
    DC_128_PRED,
    Z1_PRED,
    Z2_PRED,
    Z3_PRED,
    FILTER_PRED = N_INTRA_PRED_MODES,
};

enum InterIntraPredMode {
    II_DC_PRED,
    II_VERT_PRED,
    II_HOR_PRED,
    II_SMOOTH_PRED,
    N_INTER_INTRA_PRED_MODES,
};

enum BlockPartition {
    PARTITION_NONE,     // [ ] <-.
    PARTITION_H,        // [-]   |
    PARTITION_V,        // [|]   |
    PARTITION_SPLIT,    // [+] --'
    PARTITION_T_TOP_SPLIT,    // [⊥] i.e. split top, H bottom
    PARTITION_T_BOTTOM_SPLIT, // [т] i.e. H top, split bottom
    PARTITION_T_LEFT_SPLIT,   // [-|] i.e. split left, V right
    PARTITION_T_RIGHT_SPLIT,  // [|-] i.e. V left, split right
    PARTITION_H4,       // [Ⲷ]
    PARTITION_V4,       // [Ⲽ]
    N_PARTITIONS,
    N_SUB8X8_PARTITIONS = PARTITION_T_TOP_SPLIT,
};

enum BlockSize {
    BS_128x128,
    BS_128x64,
    BS_64x128,
    BS_64x64,
    BS_64x32,
    BS_64x16,
    BS_32x64,
    BS_32x32,
    BS_32x16,
    BS_32x8,
    BS_16x64,
    BS_16x32,
    BS_16x16,
    BS_16x8,
    BS_16x4,
    BS_8x32,
    BS_8x16,
    BS_8x8,
    BS_8x4,
    BS_4x16,
    BS_4x8,
    BS_4x4,
    N_BS_SIZES,
};

enum Filter2d { // order is horizontal, vertical
    FILTER_2D_8TAP_REGULAR,
    FILTER_2D_8TAP_REGULAR_SMOOTH,
    FILTER_2D_8TAP_REGULAR_SHARP,
    FILTER_2D_8TAP_SHARP_REGULAR,
    FILTER_2D_8TAP_SHARP_SMOOTH,
    FILTER_2D_8TAP_SHARP,
    FILTER_2D_8TAP_SMOOTH_REGULAR,
    FILTER_2D_8TAP_SMOOTH,
    FILTER_2D_8TAP_SMOOTH_SHARP,
    FILTER_2D_BILINEAR,
    N_2D_FILTERS,
};

enum MVJoint {
    MV_JOINT_ZERO,
    MV_JOINT_H,
    MV_JOINT_V,
    MV_JOINT_HV,
    N_MV_JOINTS,
};

enum InterPredMode {
    NEARESTMV,
    NEARMV,
    GLOBALMV,
    NEWMV,
    N_INTER_PRED_MODES,
};

enum DRL_PROXIMITY {
    NEAREST_DRL,
    NEARER_DRL,
    NEAR_DRL,
    NEARISH_DRL
};

enum CompInterPredMode {
    NEARESTMV_NEARESTMV,
    NEARMV_NEARMV,
    NEARESTMV_NEWMV,
    NEWMV_NEARESTMV,
    NEARMV_NEWMV,
    NEWMV_NEARMV,
    GLOBALMV_GLOBALMV,
    NEWMV_NEWMV,
    N_COMP_INTER_PRED_MODES,
};

enum CompInterType {
    COMP_INTER_NONE,
    COMP_INTER_WEIGHTED_AVG,
    COMP_INTER_AVG,
    COMP_INTER_SEG,
    COMP_INTER_WEDGE,
};

enum InterIntraType {
    INTER_INTRA_NONE,
    INTER_INTRA_BLEND,
    INTER_INTRA_WEDGE,
};

union Mv {
    struct {
        int16_t y, x;
    };
    uint32_t n;
};

enum MotionMode {
    MM_TRANSLATION,
    MM_OBMC,
    MM_WARP,
};

constexpr int QINDEX_RANGE = 256;

struct Av1Block {
    uint8_t bl, bs, bp;
    uint8_t intra, seg_id, skip_mode, skip, uvtx;
    union {
        struct {
            uint8_t y_mode, uv_mode, tx, pal_sz[2];
            int8_t y_angle, uv_angle, cfl_alpha[2];
        }; // intra
        struct {
            union {
                struct {
                    Mv mv[2];
                    uint8_t wedge_idx, mask_sign, interintra_mode;
                };
                struct {
                    Mv mv2d;
                    int16_t matrix[4];
                };
            };
            uint8_t comp_type, inter_mode, motion_mode, drl_idx;
            int8_t ref[2];
            uint8_t max_ytx, filter2d, interintra_type, tx_split0;
            uint16_t tx_split1;
        }; // inter
    };
};


// ---- pixel access --------------------------------------------------------------------------------------------------------------------

using Pixel = uint16_t;

// A window onto one plane of a picture: `origin` is the index of the sample at (0, 0) of the window, rows are `stride` samples apart.
// Coordinates may be negative as long as they stay inside the plane (the intra edges and the filters read the neighbours of a block).
// Every access is bounds checked against the plane.
struct PixelView {
  ArrayView<Pixel> plane;
  int64_t origin = 0;
  int64_t stride = 0;

  Pixel& at(int x, int y) const {
    const int64_t index = origin + static_cast<int64_t>(y) * stride + x;
    if (index < 0) bounds_violation("PixelView::at", 0, plane.size());
    return plane[static_cast<uint64_t>(index)];
  }
  // The window that starts at (x, y) of this one.
  PixelView offset(int x, int y) const { return PixelView{plane, origin + static_cast<int64_t>(y) * stride + x, stride}; }
  // True if (x, y) is inside the plane.
  bool contains(int x, int y) const {
    const int64_t index = origin + static_cast<int64_t>(y) * stride + x;
    return index >= 0 && static_cast<uint64_t>(index) < plane.size();
  }
};

}  // namespace av1
}  // namespace hardware
