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

/* d3d12.dll: the HEVC and AV1 video encoders (included at the end of encode.inc; the hooks are declared in encode_codecs.h). Both run in the kernel on the CPU
 * (src/Kernel/Dxgkrnl/VideoEncodeCodecs.hpp: the EncodeFrame arguments and metadata; src/Hardware/VideoCodec/Software/H265PictureEncoder.hpp and Av1PictureEncoder.hpp:
 * what is coded and the headers the program writes).
 *
 * HEVC: Main (NV12) and Main 10 (P010); coding tree blocks of 16, 32 or 64, coding blocks from 8, transform blocks 4 .. 32 (luma at least 8), the hierarchy depths as
 * configured; SAO, AMP (allowed: 2Nx2N is coded), transform skip (allowed: never chosen), long-term references, constrained intra prediction, loop filter across slices
 * off; I / P / B / IDR pictures, up to 15 references (8 before, 8 after, 8 long-term used by a picture), list modifications given or derived, temporal layers; slices by
 * coding tree blocks, rows or count; CQP, CBR, VBR, QVBR and absolute QP maps with QP ranges, initial QP, VBV sizes, frame size limits and delta QP maps (one a coding
 * tree block). The program writes VPS / SPS / PPS; EncodeFrame writes the slice NAL units. Sizes 16 .. 4096 x 16 .. 2304, multiples of the minimum coding block.
 * AV1: Main (NV12, P010); the features order hints, CDEF, intra edge filter, reduced transform set, high precision vectors, loop filter deltas, quantizer deltas; key,
 * inter, intra-only and switch frames, single-reference prediction from up to 7 references, every interpolation filter (switchable too), TX_MODE_LARGEST / SELECT,
 * error resilient mode, CDF updates and frame end updates on or off; tiles as one, a uniform grid or a configurable grid; the rate control modes as HEVC's in quantizer
 * indices 1 .. 255 (delta / absolute QP maps one a 64x64 superblock). EncodeFrame writes the tiles' data, one after the other; the program writes the OBUs from the
 * post-encode values of the resolved metadata (primary_ref_frame is always none, the quantizer the one coded). Sizes 16 .. 4096, even.
 * Not done (the queries say so): intra refresh, slices by bytes, intra constrained slices, the CPU / GPU input maps of EncodeFrame1 but the QP map, PSNR metadata,
 * frame analysis; for AV1 compound prediction, skip mode, palette, intra block copy, warped motion, OBMC, super-resolution, loop restoration, segmentation, delta LF,
 * quantizer matrices, 128x128 superblocks, temporal / spatial layers. */

#define VENCX_MAX_SIZE 4096
#define VENCX_HEVC_MAX_HEIGHT 2304   /* (the decoder that makes the reconstruction holds up to 4096 x 2304 / 256 coding tree blocks) */
#define VENCX_MIN_SIZE 16
#define VENCX_HEVC_MAX_REFS 15
#define VENCX_AV1_SB 64
#define VENCX_AV1_TAIL (1048 + 1216)   /* the resolved layout's tile layout and post-encode values */

/* the kernel's argument words (VideoEncodeCodecs.hpp) */
enum { VENCX_CODEC_HEVC = 1, VENCX_CODEC_AV1 = 2 };
#define VENCX_AV1_FIXED_WORDS (7 + 8 * 4 + 6 + 18 + 3 + 10 + 2 + 34)

#define VENCX_HEVC_CONFIG_FLAGS (D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_DISABLE_LOOP_FILTER_ACROSS_SLICES | D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_ENABLE_SAO_FILTER | \
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_ENABLE_LONG_TERM_REFERENCES | D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_ASYMETRIC_MOTION_PARTITION | \
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_ENABLE_TRANSFORM_SKIPPING | D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_USE_CONSTRAINED_INTRAPREDICTION)
#define VENCX_AV1_FEATURES (D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_INTRA_EDGE_FILTER | D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_CDEF_FILTERING | D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ORDER_HINT_TOOLS | \
    D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_LOOP_FILTER_DELTAS | D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_QUANTIZATION_DELTAS | D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_REDUCED_TX_SET | \
    D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_ALLOW_HIGH_PRECISION_MV)
#define VENCX_AV1_PICTURE_FLAGS (D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ENABLE_ERROR_RESILIENT_MODE | D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_DISABLE_CDF_UPDATE | \
    D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_DISABLE_FRAME_END_UPDATE_CDF | D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_REDUCED_TX_SET | \
    D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_FLAG_ALLOW_HIGH_PRECISION_MV)
#define VENCX_AV1_POST_ENCODE_FLAGS (D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_QUANTIZATION | D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_QUANTIZATION_DELTA | \
    D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_LOOP_FILTER | D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_LOOP_FILTER_DELTA | D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_CDEF_DATA | \
    D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_CONTEXT_UPDATE_TILE_ID | D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_COMPOUND_PREDICTION_MODE | \
    D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_PRIMARY_REF_FRAME | D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAG_REFERENCE_INDICES)
/* (what VENC_SUPPORT_FLAGS says but PSNR metadata and intra refresh, which are H.264's only) */
#define VENCX_SUPPORT_FLAGS (D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK | D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_RECONFIGURATION_AVAILABLE | \
    D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RESOLUTION_RECONFIGURATION_AVAILABLE | D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_VBV_SIZE_CONFIG_AVAILABLE | \
    D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_DELTA_QP_AVAILABLE | D3D12_VIDEO_ENCODER_SUPPORT_FLAG_SUBREGION_LAYOUT_RECONFIGURATION_AVAILABLE | \
    D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_ADJUSTABLE_QP_RANGE_AVAILABLE | D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_INITIAL_QP_AVAILABLE | \
    D3D12_VIDEO_ENCODER_SUPPORT_FLAG_RATE_CONTROL_MAX_FRAME_SIZE_AVAILABLE | D3D12_VIDEO_ENCODER_SUPPORT_FLAG_SEQUENCE_GOP_RECONFIGURATION_AVAILABLE | \
    D3D12_VIDEO_ENCODER_SUPPORT_FLAG_MOTION_ESTIMATION_PRECISION_MODE_LIMIT_AVAILABLE | D3D12_VIDEO_ENCODER_SUPPORT_FLAG_READABLE_RECONSTRUCTED_PICTURE_LAYOUT_AVAILABLE_)


static UINT vencx_ilog2(UINT v) { UINT n = 0; while ((1u << n) < v) ++n; return n; }   /* ceil(log2(v)) */

/* ---- profiles, levels, configurations ------------------------------------------------------------------------------------------------------------------------------ */

static int vencx_hevc_profile(const D3D12_VIDEO_ENCODER_PROFILE_DESC *p, D3D12_VIDEO_ENCODER_PROFILE_HEVC *out) {
  if (p->DataSize != sizeof(D3D12_VIDEO_ENCODER_PROFILE_HEVC) || !p->pHEVCProfile) return 0;
  if (*p->pHEVCProfile != D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN && *p->pHEVCProfile != D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10) return 0;
  if (out) *out = *p->pHEVCProfile;
  return 1;
}
static int vencx_hevc_level(const D3D12_VIDEO_ENCODER_LEVEL_SETTING *l, D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC *out) {
  if (l->DataSize != sizeof(D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC) || !l->pHEVCLevelSetting || (UINT)l->pHEVCLevelSetting->Level > D3D12_VIDEO_ENCODER_LEVELS_HEVC_62 ||
      (UINT)l->pHEVCLevelSetting->Tier > D3D12_VIDEO_ENCODER_TIER_HEVC_HIGH)
    return 0;
  if (out) *out = *l->pHEVCLevelSetting;
  return 1;
}
static int vencx_av1_profile(const D3D12_VIDEO_ENCODER_PROFILE_DESC *p, D3D12_VIDEO_ENCODER_AV1_PROFILE *out) {
  if (p->DataSize != sizeof(D3D12_VIDEO_ENCODER_AV1_PROFILE) || !p->pAV1Profile || *p->pAV1Profile != D3D12_VIDEO_ENCODER_AV1_PROFILE_MAIN) return 0;
  if (out) *out = *p->pAV1Profile;
  return 1;
}
static int vencx_av1_level(const D3D12_VIDEO_ENCODER_LEVEL_SETTING *l, D3D12_VIDEO_ENCODER_AV1_LEVEL_TIER_CONSTRAINTS *out) {
  if (l->DataSize != sizeof(D3D12_VIDEO_ENCODER_AV1_LEVEL_TIER_CONSTRAINTS) || !l->pAV1LevelSetting || (UINT)l->pAV1LevelSetting->Level > D3D12_VIDEO_ENCODER_AV1_LEVELS_6_3 ||
      (UINT)l->pAV1LevelSetting->Tier > D3D12_VIDEO_ENCODER_AV1_TIER_HIGH)
    return 0;
  if (out) *out = *l->pAV1LevelSetting;
  return 1;
}
static int vencx_profile_ok(D3D12_VIDEO_ENCODER_CODEC c, const D3D12_VIDEO_ENCODER_PROFILE_DESC *p) {
  return c == D3D12_VIDEO_ENCODER_CODEC_HEVC ? vencx_hevc_profile(p, NULL) : c == D3D12_VIDEO_ENCODER_CODEC_AV1 ? vencx_av1_profile(p, NULL) : 0;
}
static int vencx_level_ok(D3D12_VIDEO_ENCODER_CODEC c, const D3D12_VIDEO_ENCODER_LEVEL_SETTING *l) {
  return c == D3D12_VIDEO_ENCODER_CODEC_HEVC ? vencx_hevc_level(l, NULL) : c == D3D12_VIDEO_ENCODER_CODEC_AV1 ? vencx_av1_level(l, NULL) : 0;
}
/* The input formats: HEVC Main NV12, Main 10 P010; AV1 Main either. */
static int vencx_format_ok(D3D12_VIDEO_ENCODER_CODEC c, const D3D12_VIDEO_ENCODER_PROFILE_DESC *p, DXGI_FORMAT f) {
  if (c == D3D12_VIDEO_ENCODER_CODEC_HEVC) {
    D3D12_VIDEO_ENCODER_PROFILE_HEVC hp;
    if (!vencx_hevc_profile(p, &hp)) return 0;
    return f == (hp == D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12);
  }
  return c == D3D12_VIDEO_ENCODER_CODEC_AV1 && vencx_av1_profile(p, NULL) && (f == DXGI_FORMAT_NV12 || f == DXGI_FORMAT_P010);
}

/* The sizes of an HEVC configuration, log2 (H265PictureEncoder::sequence_valid's ranges). */
typedef struct VencxHevcSizes { UINT min_cb, ctb, min_tb, max_tb, depth_inter, depth_intra; } VencxHevcSizes;
static int vencx_hevc_sizes(const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC *c, VencxHevcSizes *s) {
  if ((UINT)c->MinLumaCodingUnitSize > 3 || (UINT)c->MaxLumaCodingUnitSize > 3 || (UINT)c->MinLumaTransformUnitSize > 3 || (UINT)c->MaxLumaTransformUnitSize > 3) return 0;
  s->min_cb = c->MinLumaCodingUnitSize + 3;
  s->ctb = c->MaxLumaCodingUnitSize + 3;
  s->min_tb = c->MinLumaTransformUnitSize + 2;
  s->max_tb = c->MaxLumaTransformUnitSize + 2;
  s->depth_inter = c->max_transform_hierarchy_depth_inter;
  s->depth_intra = c->max_transform_hierarchy_depth_intra;
  return s->ctb >= 4 && s->min_cb <= s->ctb && s->min_tb < s->min_cb && s->max_tb >= 3 && s->max_tb >= s->min_tb && s->max_tb <= s->ctb && s->depth_inter <= s->ctb - s->min_tb &&
         s->depth_intra <= s->ctb - s->min_tb;
}
static int vencx_hevc_config(const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION *c, D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC *out) {
  VencxHevcSizes s;
  if (c->DataSize != sizeof(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC) || !c->pHEVCConfig || (c->pHEVCConfig->ConfigurationFlags & ~VENCX_HEVC_CONFIG_FLAGS) ||
      !vencx_hevc_sizes(c->pHEVCConfig, &s))
    return 0;
  if (out) *out = *c->pHEVCConfig;
  return 1;
}
static int vencx_av1_config(const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION *c, D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION *out) {
  if (c->DataSize != sizeof(D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION) || !c->pAV1Config || (c->pAV1Config->FeatureFlags & ~VENCX_AV1_FEATURES) || c->pAV1Config->OrderHintBitsMinus1 > 7)
    return 0;
  if (out) *out = *c->pAV1Config;
  return 1;
}
static int vencx_config_ok(D3D12_VIDEO_ENCODER_CODEC codec, const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION *c) {
  return codec == D3D12_VIDEO_ENCODER_CODEC_HEVC ? vencx_hevc_config(c, NULL) : vencx_av1_config(c, NULL);
}
/* A picture size the codec takes: HEVC's a multiple of the minimum coding block (8 when no configuration is named), AV1's even. */
static int vencx_resolution_ok(D3D12_VIDEO_ENCODER_CODEC codec, const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC *hevc, const D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC *r) {
  if (r->Width < VENCX_MIN_SIZE || r->Height < VENCX_MIN_SIZE || r->Width > VENCX_MAX_SIZE || r->Height > VENCX_MAX_SIZE) return 0;
  if (codec == D3D12_VIDEO_ENCODER_CODEC_AV1) return r->Width % 2 == 0 && r->Height % 2 == 0;
  const UINT multiple = hevc ? 1u << (hevc->MinLumaCodingUnitSize + 3) : 8;
  return r->Height <= VENCX_HEVC_MAX_HEIGHT && r->Width % multiple == 0 && r->Height % multiple == 0;
}
static int vencx_gop(D3D12_VIDEO_ENCODER_CODEC codec, const D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE *g) {
  if (codec == D3D12_VIDEO_ENCODER_CODEC_HEVC)
    return g->DataSize == sizeof(D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC) && g->pHEVCGroupOfPictures && g->pHEVCGroupOfPictures->log2_max_pic_order_cnt_lsb_minus4 <= 12;
  return g->DataSize == sizeof(D3D12_VIDEO_ENCODER_AV1_SEQUENCE_STRUCTURE) && g->pAV1SequenceStructure;
}

/* The rate control words 10 .. 30 (as venc_rate_control writes them for H.264) with QPs up to max_qp (HEVC 51, AV1 255: quantizer indices, 0 - lossless - not taken). */
static int vencx_rate_control(const D3D12_VIDEO_ENCODER_RATE_CONTROL *rc, UINT *w, UINT max_qp) {
  memset(w + 10, 0, 21 * sizeof *w);
  if ((UINT)rc->Mode > D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_QVBR || (rc->Flags & ~VENC_RC_FLAGS)) return 0;
  const UINT min_cqp = max_qp > 51 ? 1 : 0;
  w[10] = rc->Mode;
  w[11] = rc->Flags;
  w[12] = rc->TargetFrameRate.Numerator;
  w[13] = rc->TargetFrameRate.Denominator;
  w[19] = max_qp;
  const UINT qp_range = (rc->Flags & D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_QP_RANGE) != 0;
#define RCX_U64(at, v) do { w[at] = (UINT)(v); w[(at) + 1] = (UINT)((UINT64)(v) >> 32); } while (0)
  switch (rc->Mode) {
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_ABSOLUTE_QP_MAP: return rc->ConfigParams.DataSize == 0 || rc->ConfigParams.pConfiguration_CQP == NULL;
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP: {
      const D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP *c = rc->ConfigParams.pConfiguration_CQP;
      if (rc->ConfigParams.DataSize != sizeof *c || !c) return 0;
      if (c->ConstantQP_FullIntracodedFrame > max_qp || c->ConstantQP_InterPredictedFrame_PrevRefOnly > max_qp || c->ConstantQP_InterPredictedFrame_BiDirectionalRef > max_qp ||
          c->ConstantQP_FullIntracodedFrame < min_cqp || c->ConstantQP_InterPredictedFrame_PrevRefOnly < min_cqp || c->ConstantQP_InterPredictedFrame_BiDirectionalRef < min_cqp)
        return 0;
      w[14] = c->ConstantQP_FullIntracodedFrame;
      w[15] = c->ConstantQP_InterPredictedFrame_PrevRefOnly;
      w[16] = c->ConstantQP_InterPredictedFrame_BiDirectionalRef;
      return 1;
    }
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR: {
      const D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR *c = rc->ConfigParams.pConfiguration_CBR;
      if (rc->ConfigParams.DataSize != sizeof *c || !c) return 0;
      w[17] = c->InitialQP; w[18] = qp_range ? c->MinQP : 0; w[19] = qp_range ? c->MaxQP : max_qp;
      RCX_U64(20, c->TargetBitRate); RCX_U64(24, c->VBVCapacity); RCX_U64(26, c->InitialVBVFullness); RCX_U64(28, c->MaxFrameBitSize);
      break;
    }
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_VBR: {
      const D3D12_VIDEO_ENCODER_RATE_CONTROL_VBR *c = rc->ConfigParams.pConfiguration_VBR;
      if (rc->ConfigParams.DataSize != sizeof *c || !c) return 0;
      w[17] = c->InitialQP; w[18] = qp_range ? c->MinQP : 0; w[19] = qp_range ? c->MaxQP : max_qp;
      RCX_U64(20, c->TargetAvgBitRate); RCX_U64(22, c->PeakBitRate); RCX_U64(24, c->VBVCapacity); RCX_U64(26, c->InitialVBVFullness); RCX_U64(28, c->MaxFrameBitSize);
      break;
    }
    case D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_QVBR: {
      const D3D12_VIDEO_ENCODER_RATE_CONTROL_QVBR *c = rc->ConfigParams.pConfiguration_QVBR;
      if (rc->ConfigParams.DataSize != sizeof *c || !c) return 0;
      w[17] = c->InitialQP; w[18] = qp_range ? c->MinQP : 0; w[19] = qp_range ? c->MaxQP : max_qp;
      RCX_U64(20, c->TargetAvgBitRate); RCX_U64(22, c->PeakBitRate); RCX_U64(28, c->MaxFrameBitSize);
      w[30] = c->ConstantQualityTarget;
      break;
    }
  }
#undef RCX_U64
  /* (what the kernel's rate_control_valid checks, so that the query answers what EncodeFrame does) */
  const UINT64 target = w[20] | (UINT64)w[21] << 32, peak = w[22] | (UINT64)w[23] << 32, cap = w[24] | (UINT64)w[25] << 32, init = w[26] | (UINT64)w[27] << 32;
  const UINT64 max_rate = (UINT64)1 << 38;
  if (!w[12] || !w[13] || w[12] > (1u << 24) || w[13] > (1u << 24) || !target || target > max_rate) return 0;
  if (rc->Mode != D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR && (peak < target || peak > max_rate)) return 0;
  if (rc->Mode == D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_QVBR && (w[30] == 0 || w[30] > max_qp)) return 0;
  if (qp_range && (w[18] > w[19] || w[19] > max_qp)) return 0;
  if ((rc->Flags & D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_INITIAL_QP) && w[17] > max_qp) return 0;
  if ((rc->Flags & D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES) && (cap > max_rate * 16 || init > cap)) return 0;
  if ((rc->Flags & D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_MAX_FRAME_SIZE) && !(w[28] | w[29])) return 0;
  return 1;
}
static UINT vencx_max_qp(D3D12_VIDEO_ENCODER_CODEC c) { return c == D3D12_VIDEO_ENCODER_CODEC_AV1 ? 255 : 51; }

/* ---- subregions: HEVC slices, AV1 tiles ---------------------------------------------------------------------------------------------------------------------------- */

static int vencx_mode_ok(D3D12_VIDEO_ENCODER_CODEC c, D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE m) {
  if (m == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME) return 1;
  if (c == D3D12_VIDEO_ENCODER_CODEC_AV1)
    return m == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_GRID_PARTITION_ || m == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_CONFIGURABLE_GRID_PARTITION_;
  return m == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_SQUARE_UNITS_PER_SUBREGION_ROW_UNALIGNED || m == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION ||
         m == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME;
}
/* How many slices an HEVC layout makes of a picture (H265PictureEncoder::slice_count); 0: not one the encoder takes. */
static UINT vencx_hevc_slices(D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE mode, const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA *data, UINT log2_ctb, UINT w, UINT h) {
  const UINT ctb = 1u << log2_ctb, cw = (w + ctb - 1) / ctb, ch = (h + ctb - 1) / ctb, ctbs = cw * ch;
  if (!ctbs) return 0;
  if (mode == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME) return 1;
  if (!data || data->DataSize != sizeof(D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES) || !data->pSlicesPartition_HEVC) return 0;
  const UINT v = data->pSlicesPartition_HEVC->NumberOfCodingUnitsPerSlice;
  switch (mode) {
    case D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_SQUARE_UNITS_PER_SUBREGION_ROW_UNALIGNED: return v ? (ctbs + v - 1) / v : 0;
    case D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION: return v ? (ch + v - 1) / v : 0;
    case D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME: return v && v <= ctbs ? v : 0;
    default: return 0;
  }
}

/* An AV1 tile layout as the frame header's tile_info() makes it (5.9.15): a uniform grid of at least cols x rows tiles (completed: the real counts and sizes), or
 * explicit sizes in superblocks checked against the limits (tiles at most 4096 samples wide, of at most 4096 x 2304 samples). Av1PictureEncoder::tile_layout does
 * the same for the kernel. 0: the layout is not one AV1 has. */
static UINT vencx_tile_log2(UINT blk, UINT target) { UINT k = 0; while ((blk << k) < target) ++k; return k; }
static int vencx_av1_tiles(UINT w, UINT h, int uniform, D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_TILES *t) {
  const UINT sb_cols = (w + VENCX_AV1_SB - 1) / VENCX_AV1_SB, sb_rows = (h + VENCX_AV1_SB - 1) / VENCX_AV1_SB;
  const UINT max_width_sb = 4096 / VENCX_AV1_SB, max_area_sb = 4096 * 2304 / (VENCX_AV1_SB * VENCX_AV1_SB);
  const UINT min_log2_cols = vencx_tile_log2(max_width_sb, sb_cols);
  const UINT max_log2_cols = vencx_tile_log2(1, sb_cols < 64 ? sb_cols : 64), max_log2_rows = vencx_tile_log2(1, sb_rows < 64 ? sb_rows : 64);
  const UINT min_log2 = min_log2_cols > vencx_tile_log2(max_area_sb, sb_rows * sb_cols) ? min_log2_cols : vencx_tile_log2(max_area_sb, sb_rows * sb_cols);
  if (t->RowCount == 0 || t->ColCount == 0 || t->RowCount > 64 || t->ColCount > 64) return 0;
  if (uniform) {
    UINT cl = vencx_ilog2((UINT)t->ColCount);
    if (cl < min_log2_cols) cl = min_log2_cols;
    if (cl > max_log2_cols) cl = max_log2_cols;
    const UINT tw = (sb_cols + (1u << cl) - 1) >> cl;
    const UINT cols = (sb_cols + tw - 1) / tw;
    const UINT min_log2_rows = min_log2 > cl ? min_log2 - cl : 0;
    UINT rl = vencx_ilog2((UINT)t->RowCount);
    if (rl < min_log2_rows) rl = min_log2_rows;
    if (rl > max_log2_rows) rl = max_log2_rows;
    const UINT th = (sb_rows + (1u << rl) - 1) >> rl;
    const UINT rows = (sb_rows + th - 1) / th;
    memset(t->ColWidths, 0, sizeof t->ColWidths);
    memset(t->RowHeights, 0, sizeof t->RowHeights);
    for (UINT i = 0; i < cols; ++i) t->ColWidths[i] = i + 1 < cols ? tw : sb_cols - tw * (cols - 1);
    for (UINT i = 0; i < rows; ++i) t->RowHeights[i] = i + 1 < rows ? th : sb_rows - th * (rows - 1);
    t->ColCount = cols;
    t->RowCount = rows;
  } else {
    UINT64 sum = 0, widest = 0;
    for (UINT i = 0; i < t->ColCount; ++i) {
      if (t->ColWidths[i] == 0 || t->ColWidths[i] > max_width_sb) return 0;
      sum += t->ColWidths[i];
      if (t->ColWidths[i] > widest) widest = t->ColWidths[i];
    }
    if (sum != sb_cols) return 0;
    const UINT64 max_area = min_log2 ? ((UINT64)sb_rows * sb_cols) >> (min_log2 + 1) : (UINT64)sb_rows * sb_cols;
    const UINT64 max_height = max_area / widest > 1 ? max_area / widest : 1;
    sum = 0;
    for (UINT i = 0; i < t->RowCount; ++i) {
      if (t->RowHeights[i] == 0 || t->RowHeights[i] > max_height) return 0;
      sum += t->RowHeights[i];
    }
    if (sum != sb_rows) return 0;
  }
  return t->ContextUpdateTileId < t->RowCount * t->ColCount;
}
static UINT vencx_av1_max_tiles(UINT w, UINT h) {
  const UINT sb_cols = (w + VENCX_AV1_SB - 1) / VENCX_AV1_SB, sb_rows = (h + VENCX_AV1_SB - 1) / VENCX_AV1_SB;
  return (sb_cols < 64 ? sb_cols : 64) * (sb_rows < 64 ? sb_rows : 64);
}
/* How many subregions a layout makes (slices or tiles); 0 if it is not one the encoder takes. `hevc` names the configuration (the coding tree block size). */
static UINT vencx_subregions(D3D12_VIDEO_ENCODER_CODEC codec, const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC *hevc, D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE mode,
                             const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA *data, UINT w, UINT h) {
  if (!vencx_mode_ok(codec, mode)) return 0;
  if (codec == D3D12_VIDEO_ENCODER_CODEC_HEVC) return vencx_hevc_slices(mode, data, hevc ? hevc->MaxLumaCodingUnitSize + 3 : 5, w, h);
  if (mode == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME) return 1;
  if (!data || data->DataSize != sizeof(D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_TILES) || !data->pTilesPartition_AV1) return 0;
  D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_TILES t = *data->pTilesPartition_AV1;
  if (!vencx_av1_tiles(w, h, mode == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_GRID_PARTITION_, &t)) return 0;
  return (UINT)(t.RowCount * t.ColCount);
}

/* ---- levels a size and rate need ------------------------------------------------------------------------------------------------------------------------------------ */

/* HEVC Table A.8: MaxLumaPs and MaxLumaSr (Main tier). */
static D3D12_VIDEO_ENCODER_LEVELS_HEVC vencx_hevc_suggested_level(UINT w, UINT h, const DXGI_RATIONAL *rate) {
  static const struct { D3D12_VIDEO_ENCODER_LEVELS_HEVC level; UINT64 ps, sr; } levels[] = {
      {D3D12_VIDEO_ENCODER_LEVELS_HEVC_1, 36864, 552960}, {D3D12_VIDEO_ENCODER_LEVELS_HEVC_2, 122880, 3686400}, {D3D12_VIDEO_ENCODER_LEVELS_HEVC_21, 245760, 7372800},
      {D3D12_VIDEO_ENCODER_LEVELS_HEVC_3, 552960, 16588800}, {D3D12_VIDEO_ENCODER_LEVELS_HEVC_31, 983040, 33177600}, {D3D12_VIDEO_ENCODER_LEVELS_HEVC_4, 2228224, 66846720},
      {D3D12_VIDEO_ENCODER_LEVELS_HEVC_41, 2228224, 133693440}, {D3D12_VIDEO_ENCODER_LEVELS_HEVC_5, 8912896, 267386880}, {D3D12_VIDEO_ENCODER_LEVELS_HEVC_51, 8912896, 534773760},
      {D3D12_VIDEO_ENCODER_LEVELS_HEVC_52, 8912896, 1069547520}, {D3D12_VIDEO_ENCODER_LEVELS_HEVC_6, 35651584, 1069547520}, {D3D12_VIDEO_ENCODER_LEVELS_HEVC_61, 35651584, 2139095040},
      {D3D12_VIDEO_ENCODER_LEVELS_HEVC_62, 35651584, 4278190080u}};
  const UINT64 ps = (UINT64)w * h, sr = rate && rate->Denominator ? ps * rate->Numerator / rate->Denominator : ps * 30;
  for (UINT i = 0; i < sizeof levels / sizeof levels[0]; ++i)
    if (ps <= levels[i].ps && sr <= levels[i].sr) return levels[i].level;
  return D3D12_VIDEO_ENCODER_LEVELS_HEVC_62;
}
/* AV1 Annex A.3: MaxPicSize and MaxDisplayRate. */
static D3D12_VIDEO_ENCODER_AV1_LEVELS vencx_av1_suggested_level(UINT w, UINT h, const DXGI_RATIONAL *rate) {
  static const struct { D3D12_VIDEO_ENCODER_AV1_LEVELS level; UINT64 ps, sr; } levels[] = {
      {D3D12_VIDEO_ENCODER_AV1_LEVELS_2_0, 147456, 4423680}, {D3D12_VIDEO_ENCODER_AV1_LEVELS_2_1, 278784, 8363520}, {D3D12_VIDEO_ENCODER_AV1_LEVELS_3_0, 665856, 19975680},
      {D3D12_VIDEO_ENCODER_AV1_LEVELS_3_1, 1065024, 31950720}, {D3D12_VIDEO_ENCODER_AV1_LEVELS_4_0, 2359296, 70778880}, {D3D12_VIDEO_ENCODER_AV1_LEVELS_4_1, 2359296, 141557760},
      {D3D12_VIDEO_ENCODER_AV1_LEVELS_5_0, 8912896, 267386880}, {D3D12_VIDEO_ENCODER_AV1_LEVELS_5_1, 8912896, 534773760}, {D3D12_VIDEO_ENCODER_AV1_LEVELS_5_2, 8912896, 1069547520},
      {D3D12_VIDEO_ENCODER_AV1_LEVELS_6_0, 35651584, 1069547520}, {D3D12_VIDEO_ENCODER_AV1_LEVELS_6_1, 35651584, 2139095040}, {D3D12_VIDEO_ENCODER_AV1_LEVELS_6_2, 35651584, 4278190080u}};
  const UINT64 ps = (UINT64)w * h, sr = rate && rate->Denominator ? ps * rate->Numerator / rate->Denominator : ps * 30;
  for (UINT i = 0; i < sizeof levels / sizeof levels[0]; ++i)
    if (ps <= levels[i].ps && sr <= levels[i].sr) return levels[i].level;
  return D3D12_VIDEO_ENCODER_AV1_LEVELS_6_3;
}

/* ---- the queries ------------------------------------------------------------------------------------------------------------------------------------------------------ */

/* The per-resolution limits: HEVC a slice and a QP value a coding tree block, AV1 a tile and a QP value a 64x64 superblock. */
static void vencx_limits(D3D12_VIDEO_ENCODER_CODEC codec, const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC *hevc, const D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC *r,
                         D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS *lim) {
  const UINT unit = codec == D3D12_VIDEO_ENCODER_CODEC_AV1 ? VENCX_AV1_SB : 1u << (hevc->MaxLumaCodingUnitSize + 3);
  lim->MaxSubregionsNumber = codec == D3D12_VIDEO_ENCODER_CODEC_AV1 ? vencx_av1_max_tiles(r->Width, r->Height) : ((r->Width + unit - 1) / unit) * ((r->Height + unit - 1) / unit);
  lim->MaxIntraRefreshFrameDuration = 0;
  lim->SubregionBlockPixelsSize = unit;
  lim->QPMapRegionPixelsSize = unit;
}

/* D3D12_FEATURE_VIDEO_ENCODER_SUPPORT, SUPPORT1 and SUPPORT2 of HEVC and AV1 (as venc_support for H.264). */
static HRESULT vencx_support(D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT *f, const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA *layout, const D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT2 *f2) {
  if (f->NodeIndex) return E_INVALIDARG;
  const D3D12_VIDEO_ENCODER_CODEC codec = f->Codec;
  UINT v = 0, w[VENC_PARAM_WORDS];
  f->SupportFlags = D3D12_VIDEO_ENCODER_SUPPORT_FLAG_NONE;
  const int config = vencx_config_ok(codec, &f->CodecConfiguration);
  const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC *hevc = codec == D3D12_VIDEO_ENCODER_CODEC_HEVC && config ? f->CodecConfiguration.pHEVCConfig : NULL;
  if (f->InputFormat != DXGI_FORMAT_NV12 && f->InputFormat != DXGI_FORMAT_P010) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_INPUT_FORMAT_NOT_SUPPORTED;
  if (!config) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_CODEC_CONFIGURATION_NOT_SUPPORTED;
  if (!vencx_gop(codec, &f->CodecGopSequence)) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_GOP_STRUCTURE_NOT_SUPPORTED;
  if ((UINT)f->RateControl.Mode > D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_QVBR) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RATE_CONTROL_MODE_NOT_SUPPORTED;
  else if (!vencx_rate_control(&f->RateControl, w, vencx_max_qp(codec))) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RATE_CONTROL_CONFIGURATION_NOT_SUPPORTED;
  if (f->IntraRefresh != D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_INTRA_REFRESH_MODE_NOT_SUPPORTED;
  if (!vencx_mode_ok(codec, f->SubregionFrameEncoding)) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_SUBREGION_LAYOUT_MODE_NOT_SUPPORTED;
  if (!f->ResolutionsListCount || !f->pResolutionList) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RESOLUTION_NOT_SUPPORTED_IN_LIST;
  if (f2) {
    if (f2->QPMap.Enabled && f2->QPMap.MapSource != D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_CPU_BUFFER) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_QPMAP_NOT_SUPPORTED_;
    if (f2->DirtyRegions.Enabled) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_DIRTY_REGIONS_NOT_SUPPORTED_;
    if (f2->MotionSearch.Enabled) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_MOTION_SEARCH_NOT_SUPPORTED_;
    if (f2->FrameAnalysis.Enabled) v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_FRAME_ANALYSIS_NOT_SUPPORTED_;
  }
  for (UINT i = 0; f->pResolutionList && i < f->ResolutionsListCount; ++i) {
    const D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC *r = &f->pResolutionList[i];
    if (!vencx_resolution_ok(codec, hevc, r)) { v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_RESOLUTION_NOT_SUPPORTED_IN_LIST; continue; }
    if (layout && vencx_mode_ok(codec, f->SubregionFrameEncoding) && (codec == D3D12_VIDEO_ENCODER_CODEC_AV1 || hevc) &&
        !vencx_subregions(codec, hevc, f->SubregionFrameEncoding, layout, r->Width, r->Height))
      v |= D3D12_VIDEO_ENCODER_VALIDATION_FLAG_SUBREGION_LAYOUT_DATA_NOT_SUPPORTED_;
    if (codec == D3D12_VIDEO_ENCODER_CODEC_HEVC && !hevc) continue;   /* (no limits without the coding tree block size) */
    if (f2 && f2->pResolutionDependentSupport) {
      D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS l0;
      D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS1 *lim = &f2->pResolutionDependentSupport[i];
      vencx_limits(codec, hevc, r, &l0);
      memset(lim, 0, sizeof *lim);
      lim->MaxSubregionsNumber = l0.MaxSubregionsNumber;
      lim->SubregionBlockPixelsSize = l0.SubregionBlockPixelsSize;
      lim->QPMapRegionPixelsSize = l0.QPMapRegionPixelsSize;
    } else if (!f2 && f->pResolutionDependentSupport) {
      vencx_limits(codec, hevc, r, &f->pResolutionDependentSupport[i]);
    }
  }
  f->MaxReferenceFramesInDPB = codec == D3D12_VIDEO_ENCODER_CODEC_AV1 ? 8 : VENCX_HEVC_MAX_REFS;
  f->ValidationFlags = (D3D12_VIDEO_ENCODER_VALIDATION_FLAGS)v;
  if (v) return S_OK;
  f->SupportFlags = VENCX_SUPPORT_FLAGS;
  UINT mw = 0, mh = 0;
  for (UINT i = 0; i < f->ResolutionsListCount; ++i)
    if ((UINT64)f->pResolutionList[i].Width * f->pResolutionList[i].Height > (UINT64)mw * mh) { mw = f->pResolutionList[i].Width; mh = f->pResolutionList[i].Height; }
  if (codec == D3D12_VIDEO_ENCODER_CODEC_HEVC) {
    if (f->SuggestedProfile.DataSize == sizeof(D3D12_VIDEO_ENCODER_PROFILE_HEVC) && f->SuggestedProfile.pHEVCProfile)
      *f->SuggestedProfile.pHEVCProfile = f->InputFormat == DXGI_FORMAT_P010 ? D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10 : D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN;
    if (f->SuggestedLevel.DataSize == sizeof(D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC) && f->SuggestedLevel.pHEVCLevelSetting) {
      f->SuggestedLevel.pHEVCLevelSetting->Level = vencx_hevc_suggested_level(mw, mh, &f->RateControl.TargetFrameRate);
      f->SuggestedLevel.pHEVCLevelSetting->Tier = D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN;
    }
  } else {
    if (f->SuggestedProfile.DataSize == sizeof(D3D12_VIDEO_ENCODER_AV1_PROFILE) && f->SuggestedProfile.pAV1Profile) *f->SuggestedProfile.pAV1Profile = D3D12_VIDEO_ENCODER_AV1_PROFILE_MAIN;
    if (f->SuggestedLevel.DataSize == sizeof(D3D12_VIDEO_ENCODER_AV1_LEVEL_TIER_CONSTRAINTS) && f->SuggestedLevel.pAV1LevelSetting) {
      f->SuggestedLevel.pAV1LevelSetting->Level = vencx_av1_suggested_level(mw, mh, &f->RateControl.TargetFrameRate);
      f->SuggestedLevel.pAV1LevelSetting->Tier = D3D12_VIDEO_ENCODER_AV1_TIER_MAIN;
    }
  }
  return S_OK;
}

/* The session an input map query describes: one the encoder takes. */
static int vencx_session_ok(const D3D12_VIDEO_ENCODER_INPUT_MAP_SESSION_INFO *si) {
  if (!vencx_codec(si->Codec) || !vencx_format_ok(si->Codec, &si->Profile, si->InputFormat) || !vencx_level_ok(si->Codec, &si->Level) || !vencx_config_ok(si->Codec, &si->CodecConfiguration))
    return 0;
  const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC *hevc = si->Codec == D3D12_VIDEO_ENCODER_CODEC_HEVC ? si->CodecConfiguration.pHEVCConfig : NULL;
  return vencx_resolution_ok(si->Codec, hevc, &si->InputResolution) &&
         vencx_subregions(si->Codec, hevc, si->SubregionFrameEncoding, &si->SubregionFrameEncodingData, si->InputResolution.Width, si->InputResolution.Height);
}
static UINT64 vencx_metadata_bytes(D3D12_VIDEO_ENCODER_CODEC codec, UINT w, UINT h) {
  if (codec == D3D12_VIDEO_ENCODER_CODEC_AV1) return VENC_METADATA_HEADER + (UINT64)vencx_av1_max_tiles(w, h) * VENC_SUBREGION_BYTES + VENCX_AV1_TAIL;
  return VENC_METADATA_HEADER + (UINT64)((w + 15) / 16) * ((h + 15) / 16) * VENC_SUBREGION_BYTES;   /* (slices at most one a coding tree block of 16) */
}
static int vencx_heap_ok(const D3D12_VIDEO_ENCODER_HEAP_DESC1 *h) {
  if (h->NodeMask > 1 || h->Flags != D3D12_VIDEO_ENCODER_HEAP_FLAG_NONE || h->Pow2DownscaleFactor != 0 || !vencx_profile_ok(h->EncodeCodec, &h->EncodeProfile) ||
      !vencx_level_ok(h->EncodeCodec, &h->EncodeLevel) || !h->ResolutionsListCount || h->ResolutionsListCount > 256 || !h->pResolutionList)
    return 0;
  /* (the heap names no configuration: HEVC sizes in whole 8x8 blocks, EncodeFrame checks them against the encoder's minimum coding block) */
  for (UINT i = 0; i < h->ResolutionsListCount; ++i)
    if (!vencx_resolution_ok(h->EncodeCodec, NULL, &h->pResolutionList[i])) return 0;
  return 1;
}

static HRESULT vencx_feature(Dev *d, D3D12_FEATURE_VIDEO feature, void *data, UINT size) {
#define XFEATURE(type) do { if (size != sizeof(type)) return E_INVALIDARG; } while (0)
  (void)d;
  switch (feature) {
    case D3D12_FEATURE_VIDEO_ENCODER_CODEC: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC *f = data;
      if (f->NodeIndex || !vencx_codec(f->Codec)) return S_FALSE;
      f->IsSupported = TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL *f = data;
      if (f->NodeIndex || !vencx_codec(f->Codec)) return S_FALSE;
      f->IsSupported = FALSE;
      if (f->Codec == D3D12_VIDEO_ENCODER_CODEC_HEVC) {
        if (f->Profile.DataSize != sizeof(D3D12_VIDEO_ENCODER_PROFILE_HEVC) || !f->Profile.pHEVCProfile || f->MinSupportedLevel.DataSize != sizeof(D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC) ||
            !f->MinSupportedLevel.pHEVCLevelSetting || f->MaxSupportedLevel.DataSize != sizeof(D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC) || !f->MaxSupportedLevel.pHEVCLevelSetting)
          return E_INVALIDARG;
        f->IsSupported = vencx_hevc_profile(&f->Profile, NULL);
        if (f->IsSupported) {
          f->MinSupportedLevel.pHEVCLevelSetting->Level = D3D12_VIDEO_ENCODER_LEVELS_HEVC_1;
          f->MinSupportedLevel.pHEVCLevelSetting->Tier = D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN;
          f->MaxSupportedLevel.pHEVCLevelSetting->Level = D3D12_VIDEO_ENCODER_LEVELS_HEVC_62;
          f->MaxSupportedLevel.pHEVCLevelSetting->Tier = D3D12_VIDEO_ENCODER_TIER_HEVC_HIGH;
        }
      } else {
        if (f->Profile.DataSize != sizeof(D3D12_VIDEO_ENCODER_AV1_PROFILE) || !f->Profile.pAV1Profile || f->MinSupportedLevel.DataSize != sizeof(D3D12_VIDEO_ENCODER_AV1_LEVEL_TIER_CONSTRAINTS) ||
            !f->MinSupportedLevel.pAV1LevelSetting || f->MaxSupportedLevel.DataSize != sizeof(D3D12_VIDEO_ENCODER_AV1_LEVEL_TIER_CONSTRAINTS) || !f->MaxSupportedLevel.pAV1LevelSetting)
          return E_INVALIDARG;
        f->IsSupported = vencx_av1_profile(&f->Profile, NULL);
        if (f->IsSupported) {
          f->MinSupportedLevel.pAV1LevelSetting->Level = D3D12_VIDEO_ENCODER_AV1_LEVELS_2_0;
          f->MinSupportedLevel.pAV1LevelSetting->Tier = D3D12_VIDEO_ENCODER_AV1_TIER_MAIN;
          f->MaxSupportedLevel.pAV1LevelSetting->Level = D3D12_VIDEO_ENCODER_AV1_LEVELS_6_3;
          f->MaxSupportedLevel.pAV1LevelSetting->Tier = D3D12_VIDEO_ENCODER_AV1_TIER_HIGH;
        }
      }
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT *f = data;
      if (f->NodeIndex || !vencx_codec(f->Codec)) return S_FALSE;
      f->ResolutionRatiosCount = 0;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION *f = data;
      if (f->NodeIndex || !vencx_codec(f->Codec)) return S_FALSE;
      if (f->ResolutionRatiosCount != 0) return E_INVALIDARG;
      const int av1 = f->Codec == D3D12_VIDEO_ENCODER_CODEC_AV1;
      f->IsSupported = TRUE;
      /* (HEVC: whole minimum coding blocks - 8 at the least - the program's SPS crops what it shows; AV1: any even size) */
      f->MinResolutionSupported.Width = f->MinResolutionSupported.Height = VENCX_MIN_SIZE;
      f->MaxResolutionSupported.Width = VENCX_MAX_SIZE;
      f->MaxResolutionSupported.Height = av1 ? VENCX_MAX_SIZE : VENCX_HEVC_MAX_HEIGHT;
      f->ResolutionWidthMultipleRequirement = f->ResolutionHeightMultipleRequirement = av1 ? 2 : 8;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT *f = data;
      if (f->NodeIndex || !vencx_codec(f->Codec)) return S_FALSE;
      f->IsSupported = vencx_format_ok(f->Codec, &f->Profile, f->Format);
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_RATE_CONTROL_MODE: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_RATE_CONTROL_MODE);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_RATE_CONTROL_MODE *f = data;
      if (f->NodeIndex || !vencx_codec(f->Codec)) return S_FALSE;
      f->IsSupported = (UINT)f->RateControlMode <= D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_QVBR;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_INTRA_REFRESH_MODE: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_INTRA_REFRESH_MODE);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_INTRA_REFRESH_MODE *f = data;
      if (f->NodeIndex || !vencx_codec(f->Codec)) return S_FALSE;
      f->IsSupported = vencx_profile_ok(f->Codec, &f->Profile) && vencx_level_ok(f->Codec, &f->Level) && f->IntraRefreshMode == D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE *f = data;
      if (f->NodeIndex || !vencx_codec(f->Codec)) return S_FALSE;
      f->IsSupported = vencx_profile_ok(f->Codec, &f->Profile) && vencx_level_ok(f->Codec, &f->Level) && vencx_mode_ok(f->Codec, f->SubregionMode);
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_CONFIG: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_CONFIG);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_CONFIG *f = data;
      if (f->NodeIndex) return E_INVALIDARG;
      f->IsSupported = FALSE;
      if (f->Codec != D3D12_VIDEO_ENCODER_CODEC_AV1) return S_OK;   /* (the query is AV1's: H.264 and HEVC have no tile configurations) */
      if (f->CodecSupport.DataSize != sizeof(D3D12_VIDEO_ENCODER_AV1_FRAME_SUBREGION_LAYOUT_CONFIG_SUPPORT) || !f->CodecSupport.pAV1Support) return E_INVALIDARG;
      D3D12_VIDEO_ENCODER_AV1_FRAME_SUBREGION_LAYOUT_CONFIG_SUPPORT *s = f->CodecSupport.pAV1Support;
      const UINT fw = f->FrameResolution.Width, fh = f->FrameResolution.Height;
      const D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC *r = &f->FrameResolution;
      const UINT sb_cols = (fw + VENCX_AV1_SB - 1) / VENCX_AV1_SB, sb_rows = (fh + VENCX_AV1_SB - 1) / VENCX_AV1_SB;
      s->Use128SuperBlocks = FALSE;
      s->TileSizeBytesMinus1 = 3;
      s->MinTileRows = s->MinTileCols = 1;
      s->MaxTileRows = sb_rows < 64 ? sb_rows : 64;
      s->MaxTileCols = sb_cols < 64 ? sb_cols : 64;
      s->MinTileWidth = VENCX_AV1_SB;
      s->MaxTileWidth = 4096;
      s->MinTileArea = VENCX_AV1_SB * VENCX_AV1_SB;
      s->MaxTileArea = 4096 * 2304;
      s->ValidationFlags = D3D12_VIDEO_ENCODER_AV1_FRAME_SUBREGION_LAYOUT_CONFIG_VALIDATION_FLAG_NONE;
      if (!vencx_av1_profile(&f->Profile, NULL) || !vencx_av1_level(&f->Level, NULL) || !vencx_resolution_ok(f->Codec, NULL, r) || !vencx_mode_ok(f->Codec, f->SubregionMode)) {
        s->ValidationFlags = D3D12_VIDEO_ENCODER_AV1_FRAME_SUBREGION_LAYOUT_CONFIG_VALIDATION_FLAG_NOT_SPECIFIED;
        return S_OK;
      }
      if (f->SubregionMode == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME) {
        memset(&s->TilesConfiguration, 0, sizeof s->TilesConfiguration);
        s->TilesConfiguration.RowCount = s->TilesConfiguration.ColCount = 1;
      }
      D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_TILES t = s->TilesConfiguration;
      if (t.RowCount > 64) s->ValidationFlags |= D3D12_VIDEO_ENCODER_AV1_FRAME_SUBREGION_LAYOUT_CONFIG_VALIDATION_FLAG_ROWS_COUNT;
      if (t.ColCount > 64) s->ValidationFlags |= D3D12_VIDEO_ENCODER_AV1_FRAME_SUBREGION_LAYOUT_CONFIG_VALIDATION_FLAG_COLS_COUNT;
      if (s->ValidationFlags || !vencx_av1_tiles(fw, fh, f->SubregionMode != D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_CONFIGURABLE_GRID_PARTITION_, &t)) {
        s->ValidationFlags |= D3D12_VIDEO_ENCODER_AV1_FRAME_SUBREGION_LAYOUT_CONFIG_VALIDATION_FLAG_CODEC_CONSTRAINT;
        return S_OK;
      }
      s->TilesConfiguration = t;   /* (a uniform grid with the counts and sizes AV1's spacing gives) */
      f->IsSupported = TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_HEAP_SIZE: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_HEAP_SIZE);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_HEAP_SIZE *f = data;
      if (!vencx_codec(f->HeapDesc.EncodeCodec)) return S_FALSE;
      D3D12_VIDEO_ENCODER_HEAP_DESC1 h1;
      memset(&h1, 0, sizeof h1);
      h1.NodeMask = f->HeapDesc.NodeMask; h1.Flags = f->HeapDesc.Flags; h1.EncodeCodec = f->HeapDesc.EncodeCodec; h1.EncodeProfile = f->HeapDesc.EncodeProfile;
      h1.EncodeLevel = f->HeapDesc.EncodeLevel; h1.ResolutionsListCount = f->HeapDesc.ResolutionsListCount; h1.pResolutionList = f->HeapDesc.pResolutionList;
      f->IsSupported = vencx_heap_ok(&h1);
      f->MemoryPoolL0Size = 0;
      f->MemoryPoolL1Size = f->IsSupported ? VENC_HEAP_BYTES : 0;   /* (the rate controller's state: the pictures are the program's) */
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_HEAP_SIZE1: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_HEAP_SIZE1);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_HEAP_SIZE1 *f = data;
      if (!vencx_codec(f->HeapDesc.EncodeCodec)) return S_FALSE;
      f->IsSupported = vencx_heap_ok(&f->HeapDesc);
      f->MemoryPoolL0Size = 0;
      f->MemoryPoolL1Size = f->IsSupported ? VENC_HEAP_BYTES : 0;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT *f = data;
      if (f->NodeIndex || !vencx_codec(f->Codec)) return S_FALSE;
      f->IsSupported = vencx_profile_ok(f->Codec, &f->Profile);
      if (f->Codec == D3D12_VIDEO_ENCODER_CODEC_HEVC) {
        /* (SUPPORT_HEVC1 is SUPPORT_HEVC with the range extension's values after it: none of them here) */
        const int v1 = f->CodecSupportLimits.DataSize == sizeof(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC1);
        if ((!v1 && f->CodecSupportLimits.DataSize != sizeof(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC)) || !f->CodecSupportLimits.pHEVCSupport) return E_INVALIDARG;
        if (v1) memset(f->CodecSupportLimits.pHEVCSupport1, 0, sizeof *f->CodecSupportLimits.pHEVCSupport1);
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC *s = f->CodecSupportLimits.pHEVCSupport;
        memset(s, 0, sizeof *s);
        if (!f->IsSupported) return S_OK;
        s->SupportFlags = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_BFRAME_LTR_COMBINED_SUPPORT | D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_CONSTRAINED_INTRAPREDICTION_SUPPORT |
                          D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_SAO_FILTER_SUPPORT | D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_ASYMETRIC_MOTION_PARTITION_SUPPORT |
                          D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_TRANSFORM_SKIP_SUPPORT | D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_SUPPORT_HEVC_FLAG_DISABLING_LOOP_FILTER_ACROSS_SLICES_SUPPORT;
        s->MinLumaCodingUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_8x8;
        s->MaxLumaCodingUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_64x64;
        s->MinLumaTransformUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_4x4;
        s->MaxLumaTransformUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_32x32;
        s->max_transform_hierarchy_depth_inter = 4;   /* (64 down to 4) */
        s->max_transform_hierarchy_depth_intra = 4;
      } else {
        if (f->CodecSupportLimits.DataSize != sizeof(D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION_SUPPORT) || !f->CodecSupportLimits.pAV1Support) return E_INVALIDARG;
        D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION_SUPPORT *s = f->CodecSupportLimits.pAV1Support;
        memset(s, 0, sizeof *s);
        if (!f->IsSupported) return S_OK;
        s->SupportedFeatureFlags = (D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAGS)VENCX_AV1_FEATURES;
        s->RequiredFeatureFlags = D3D12_VIDEO_ENCODER_AV1_FEATURE_FLAG_NONE;
        s->SupportedInterpolationFilters = D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_FLAG_EIGHTTAP | D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_FLAG_EIGHTTAP_SMOOTH |
                                           D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_FLAG_EIGHTTAP_SHARP | D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_FLAG_BILINEAR |
                                           D3D12_VIDEO_ENCODER_AV1_INTERPOLATION_FILTERS_FLAG_SWITCHABLE;
        /* (SupportedRestorationParams all NOT_SUPPORTED, SupportedSegmentationModes none: loop restoration and segmentation are not done) */
        for (int i = 0; i < 4; ++i) s->SupportedTxModes[i] = D3D12_VIDEO_ENCODER_AV1_TX_MODE_FLAG_LARGEST | D3D12_VIDEO_ENCODER_AV1_TX_MODE_FLAG_SELECT;
        s->SegmentationBlockSize = D3D12_VIDEO_ENCODER_AV1_SEGMENTATION_BLOCK_SIZE_64x64;
        s->PostEncodeValuesFlags = (D3D12_VIDEO_ENCODER_AV1_POST_ENCODE_VALUES_FLAGS)VENCX_AV1_POST_ENCODE_FLAGS;
        s->MaxTemporalLayers = s->MaxSpatialLayers = 1;
      }
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT *f = data;
      if (f->NodeIndex || !vencx_codec(f->Codec)) return S_FALSE;
      f->IsSupported = vencx_profile_ok(f->Codec, &f->Profile);
      if (f->Codec == D3D12_VIDEO_ENCODER_CODEC_HEVC) {
        if (f->PictureSupport.DataSize != sizeof(D3D12_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT_HEVC) || !f->PictureSupport.pHEVCSupport) return E_INVALIDARG;
        D3D12_VIDEO_ENCODER_CODEC_PICTURE_CONTROL_SUPPORT_HEVC *s = f->PictureSupport.pHEVCSupport;
        memset(s, 0, sizeof *s);
        if (f->IsSupported) {
          /* (a reference picture set of at most 8 short-term pictures before and 8 after the picture, 8 long-term; lists of up to 15 entries) */
          s->MaxL0ReferencesForP = 8;
          s->MaxL0ReferencesForB = 8;
          s->MaxL1ReferencesForB = 8;
          s->MaxLongTermReferences = 8;
          s->MaxDPBCapacity = VENCX_HEVC_MAX_REFS;
        }
      } else {
        if (f->PictureSupport.DataSize != sizeof(D3D12_VIDEO_ENCODER_CODEC_AV1_PICTURE_CONTROL_SUPPORT) || !f->PictureSupport.pAV1Support) return E_INVALIDARG;
        D3D12_VIDEO_ENCODER_CODEC_AV1_PICTURE_CONTROL_SUPPORT *s = f->PictureSupport.pAV1Support;
        /* (PredictionMode is the question: single-reference prediction is done, compound is not) */
        if (s->PredictionMode != D3D12_VIDEO_ENCODER_AV1_COMP_PREDICTION_TYPE_SINGLE_REFERENCE) f->IsSupported = FALSE;
        s->MaxUniqueReferencesPerFrame = f->IsSupported ? 7 : 0;
        s->SupportedFrameTypes = f->IsSupported ? D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_FLAG_KEY_FRAME | D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_FLAG_INTER_FRAME |
                                                      D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_FLAG_INTRA_ONLY_FRAME | D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_FLAG_SWITCH_FRAME
                                                : D3D12_VIDEO_ENCODER_AV1_FRAME_TYPE_FLAG_NONE;
        s->SupportedReferenceWarpedMotionFlags = D3D12_VIDEO_ENCODER_AV1_REFERENCE_WARPED_MOTION_TRANSFORMATION_FLAG_NONE;
      }
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_SUPPORT: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT *f = data;
      if (!vencx_codec(f->Codec)) return S_FALSE;
      return vencx_support(f, NULL, NULL);
    }
    case D3D12_FEATURE_VIDEO_ENCODER_SUPPORT1: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT1 *f = data;
      if (!vencx_codec(f->Codec)) return S_FALSE;
      const HRESULT hr = vencx_support((D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT *)f, &f->SubregionFrameEncodingData, NULL);
      f->MaxQualityVsSpeed = 0;
      return hr;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_SUPPORT2: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT2);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT2 *f = data;
      if (!vencx_codec(f->Codec)) return S_FALSE;
      const HRESULT hr = vencx_support((D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT *)f, &f->SubregionFrameEncodingData, f);
      f->MaxQualityVsSpeed = 0;
      return hr;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_QPMAP_INPUT: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_QPMAP_INPUT);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_QPMAP_INPUT *f = data;
      if (f->NodeIndex || !vencx_codec(f->SessionInfo.Codec)) return S_FALSE;
      /* (from the CPU: the picture control's pRateControlQPMap, a value a coding tree block / 64x64 superblock) */
      f->IsSupported = vencx_session_ok(&f->SessionInfo) && f->MapSource == D3D12_VIDEO_ENCODER_INPUT_MAP_SOURCE_CPU_BUFFER;
      f->MapSourcePreferenceRanking = 0;
      f->BlockSize = !f->IsSupported ? 0 : f->SessionInfo.Codec == D3D12_VIDEO_ENCODER_CODEC_AV1 ? VENCX_AV1_SB : 1u << (f->SessionInfo.CodecConfiguration.pHEVCConfig->MaxLumaCodingUnitSize + 3);
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_DIRTY_REGIONS: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_DIRTY_REGIONS);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_DIRTY_REGIONS *f = data;
      if (f->NodeIndex || !vencx_codec(f->SessionInfo.Codec)) return S_FALSE;
      f->SupportFlags = D3D12_VIDEO_ENCODER_DIRTY_REGIONS_SUPPORT_FLAG_NONE;   /* (H.264's only) */
      f->MapSourcePreferenceRanking = 0;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_MOTION_SEARCH: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_MOTION_SEARCH);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_MOTION_SEARCH *f = data;
      if (f->NodeIndex || !vencx_codec(f->SessionInfo.Codec)) return S_FALSE;
      f->SupportFlags = D3D12_VIDEO_ENCODER_MOTION_SEARCH_SUPPORT_FLAG_NONE;   /* (H.264's only) */
      f->MaxMotionHints = f->MinDeviation = f->MaxDeviation = 0;
      f->MapSourcePreferenceRanking = 0;
      f->MotionUnitPrecisionSupport = D3D12_VIDEO_ENCODER_FRAME_INPUT_MOTION_UNIT_PRECISION_SUPPORT_FLAG_NONE;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS *f = data;
      if (f->NodeIndex || !vencx_codec(f->Codec)) return S_FALSE;
      f->IsSupported = vencx_format_ok(f->Codec, &f->Profile, f->InputFormat) && vencx_resolution_ok(f->Codec, NULL, &f->PictureTargetResolution);
      f->CompressedBitstreamBufferAccessAlignment = f->EncoderMetadataBufferAccessAlignment = f->IsSupported ? 1 : 0;
      f->MaxEncoderOutputMetadataBufferSize = f->IsSupported ? (UINT)vencx_metadata_bytes(f->Codec, f->PictureTargetResolution.Width, f->PictureTargetResolution.Height) : 0;
      return S_OK;
    }
    case D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS1: {
      XFEATURE(D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS1);
      D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS1 *f = data;
      if (f->NodeIndex || !vencx_codec(f->Codec)) return S_FALSE;
      const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC *hevc =
          f->Codec == D3D12_VIDEO_ENCODER_CODEC_HEVC && f->CodecConfiguration.DataSize && vencx_hevc_config(&f->CodecConfiguration, NULL) ? f->CodecConfiguration.pHEVCConfig : NULL;
      /* (no optional metadata: the PSNRs and the per-block maps are H.264's / not there) */
      f->IsSupported = vencx_format_ok(f->Codec, &f->Profile, f->InputFormat) && vencx_resolution_ok(f->Codec, hevc, &f->PictureTargetResolution) &&
                       f->OptionalMetadata == D3D12_VIDEO_ENCODER_OPTIONAL_METADATA_ENABLE_FLAG_NONE && (f->CodecConfiguration.DataSize == 0 || vencx_config_ok(f->Codec, &f->CodecConfiguration));
      f->CompressedBitstreamBufferAccessAlignment = f->EncoderMetadataBufferAccessAlignment = f->IsSupported ? 1 : 0;
      f->MaxEncoderOutputMetadataBufferSize = f->IsSupported ? (UINT)vencx_metadata_bytes(f->Codec, f->PictureTargetResolution.Width, f->PictureTargetResolution.Height) : 0;
      memset(&f->EncoderOutputMetadataQPMapTextureDimensions, 0, sizeof f->EncoderOutputMetadataQPMapTextureDimensions);
      memset(&f->EncoderOutputMetadataSATDMapTextureDimensions, 0, sizeof f->EncoderOutputMetadataSATDMapTextureDimensions);
      memset(&f->EncoderOutputMetadataBitAllocationMapTextureDimensions, 0, sizeof f->EncoderOutputMetadataBitAllocationMapTextureDimensions);
      f->EncoderOutputMetadataFramePSNRComponentsNumber = f->EncoderOutputMetadataSubregionsPSNRComponentsNumber = 0;
      f->EncoderOutputMetadataSubregionsPSNRResolvedMetadataBufferSize = 0;
      return S_OK;
    }
    default: return S_FALSE;
  }
#undef XFEATURE
}

/* ---- encoders and heaps -------------------------------------------------------------------------------------------------------------------------------------------- */

static HRESULT vencx_make(Dev *d, const D3D12_VIDEO_ENCODER_DESC *desc, REFIID riid, void **out) {
  extern const ID3D12VideoEncoderVtbl venc_vtbl;
  VEncCodecData x;
  memset(&x, 0, sizeof x);
  if (desc->EncodeCodec == D3D12_VIDEO_ENCODER_CODEC_HEVC) {
    if (!vencx_hevc_profile(&desc->EncodeProfile, &x.hevc_profile) || !vencx_hevc_config(&desc->CodecConfiguration, &x.hevc_config)) return E_INVALIDARG;
  } else if (!vencx_av1_profile(&desc->EncodeProfile, &x.av1_profile) || !vencx_av1_config(&desc->CodecConfiguration, &x.av1_config)) {
    return E_INVALIDARG;
  }
  if (!vencx_format_ok(desc->EncodeCodec, &desc->EncodeProfile, desc->InputFormat) || (UINT)desc->MaxMotionEstimationPrecision > D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_EIGHTH_PIXEL)
    return E_INVALIDARG;
  VEnc *e = dobj_new(sizeof *e, d, &venc_vtbl, g_venc_iids, dobj_child_destroy);
  if (!e) return E_OUTOFMEMORY;
  e->desc = *desc;
  e->x = x;
  if (desc->EncodeCodec == D3D12_VIDEO_ENCODER_CODEC_HEVC) {
    e->desc.EncodeProfile.pHEVCProfile = &e->x.hevc_profile;
    e->desc.CodecConfiguration.pHEVCConfig = &e->x.hevc_config;
  } else {
    e->desc.EncodeProfile.pAV1Profile = &e->x.av1_profile;
    e->desc.CodecConfiguration.pAV1Config = &e->x.av1_config;
  }
  return query_out((IUnknown *)e, riid, out);
}
static HRESULT vencx_heap_make(Dev *d, const D3D12_VIDEO_ENCODER_HEAP_DESC1 *desc, REFIID riid, void **out) {
  extern const ID3D12VideoEncoderHeap1Vtbl veheap_vtbl;
  if (desc->Flags != D3D12_VIDEO_ENCODER_HEAP_FLAG_NONE || desc->Pow2DownscaleFactor != 0) return DXGI_ERROR_UNSUPPORTED;   /* (dirty regions and frame analysis: H.264's) */
  if (!vencx_heap_ok(desc)) return E_INVALIDARG;
  VEncHeap *h = dobj_new(sizeof *h, d, &veheap_vtbl, g_veheap_iids, veheap_destroy);
  if (!h) return E_OUTOFMEMORY;
  h->desc = *desc;
  if (desc->EncodeCodec == D3D12_VIDEO_ENCODER_CODEC_HEVC) {
    (void)vencx_hevc_profile(&desc->EncodeProfile, &h->x.hevc_profile);
    (void)vencx_hevc_level(&desc->EncodeLevel, &h->x.hevc_level);
    h->desc.EncodeProfile.pHEVCProfile = &h->x.hevc_profile;
    h->desc.EncodeLevel.pHEVCLevelSetting = &h->x.hevc_level;
  } else {
    (void)vencx_av1_profile(&desc->EncodeProfile, &h->x.av1_profile);
    (void)vencx_av1_level(&desc->EncodeLevel, &h->x.av1_level);
    h->desc.EncodeProfile.pAV1Profile = &h->x.av1_profile;
    h->desc.EncodeLevel.pAV1LevelSetting = &h->x.av1_level;
  }
  h->resolutions = mem_alloc(sizeof *h->resolutions * desc->ResolutionsListCount);
  h->alloc = kbuffer_new(d, VENC_HEAP_BYTES);
  if (!h->resolutions || !h->alloc) { dx_Release(OBJ(h)); return E_OUTOFMEMORY; }
  memcpy(h->resolutions, desc->pResolutionList, sizeof *h->resolutions * desc->ResolutionsListCount);
  h->desc.pResolutionList = h->resolutions;
  return query_out((IUnknown *)h, riid, out);
}

static HRESULT vencx_get_profile(D3D12_VIDEO_ENCODER_CODEC codec, const VEncCodecData *x, D3D12_VIDEO_ENCODER_PROFILE_DESC dst) {
  if (codec == D3D12_VIDEO_ENCODER_CODEC_HEVC) {
    if (dst.DataSize != sizeof(D3D12_VIDEO_ENCODER_PROFILE_HEVC) || !dst.pHEVCProfile) return E_INVALIDARG;
    *dst.pHEVCProfile = x->hevc_profile;
    return S_OK;
  }
  if (dst.DataSize != sizeof(D3D12_VIDEO_ENCODER_AV1_PROFILE) || !dst.pAV1Profile) return E_INVALIDARG;
  *dst.pAV1Profile = x->av1_profile;
  return S_OK;
}
static HRESULT vencx_get_config(D3D12_VIDEO_ENCODER_CODEC codec, const VEncCodecData *x, D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION dst) {
  if (codec == D3D12_VIDEO_ENCODER_CODEC_HEVC) {
    if (dst.DataSize != sizeof(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC) || !dst.pHEVCConfig) return E_INVALIDARG;
    *dst.pHEVCConfig = x->hevc_config;
    return S_OK;
  }
  if (dst.DataSize != sizeof(D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION) || !dst.pAV1Config) return E_INVALIDARG;
  *dst.pAV1Config = x->av1_config;
  return S_OK;
}
static HRESULT vencx_get_level(D3D12_VIDEO_ENCODER_CODEC codec, const VEncCodecData *x, D3D12_VIDEO_ENCODER_LEVEL_SETTING dst) {
  if (codec == D3D12_VIDEO_ENCODER_CODEC_HEVC) {
    if (dst.DataSize != sizeof(D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC) || !dst.pHEVCLevelSetting) return E_INVALIDARG;
    *dst.pHEVCLevelSetting = x->hevc_level;
    return S_OK;
  }
  if (dst.DataSize != sizeof(D3D12_VIDEO_ENCODER_AV1_LEVEL_TIER_CONSTRAINTS) || !dst.pAV1LevelSetting) return E_INVALIDARG;
  *dst.pAV1LevelSetting = x->av1_level;
  return S_OK;
}

/* ---- EncodeFrame ------------------------------------------------------------------------------------------------------------------------------------------------- */

/* The planes of a 4:2:0 texture of `format` (NV12 or P010) at subresource `sub`: 0 if it is not one. */
static int vencx_planes(Resource *r, UINT sub, DXGI_FORMAT format, UINT *luma, UINT *chroma) {
  if (!r || !r->planes || !r->fmt || r->fmt->dxgi != format || sub >= r->layers) return 0;
  *luma = r->plane_alloc[sub];
  *chroma = r->plane_alloc[r->layers + sub];
  return 1;
}

/* A growing array of words for the arguments. */
typedef struct VencxWords { UINT *w; UINT n, cap; int failed; } VencxWords;
static void vencx_put(VencxWords *a, UINT v) {
  if (a->failed) return;
  if (a->n == a->cap) {
    const UINT cap = a->cap ? a->cap * 2 : 256;
    UINT *n = mem_grow(a->w, cap * sizeof *n);
    if (!n) { a->failed = 1; return; }
    a->w = n;
    a->cap = cap;
  }
  a->w[a->n++] = v;
}

static void vencx_record(CmdList *l, VEnc *e, VEncHeap *h, const D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_DESC *seq, D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAGS pflags,
                         UINT codec_data_size, const void *pdata, const D3D12_VIDEO_ENCODE_REFERENCE_FRAMES *rf, Resource *input, UINT input_sub, Resource *bits, UINT64 bits_offset,
                         Resource *recon, UINT recon_sub, Resource *meta, UINT64 meta_offset, const VencExtras *x, UINT errors) {
  const D3D12_VIDEO_ENCODER_CODEC codec = e->desc.EncodeCodec;
  const DXGI_FORMAT format = e->desc.InputFormat;
  UINT il, ic, rl = 0, rc = 0;
  /* (what is not there to write into or read from makes no packet: the metadata could not say anything) */
  if (!bits || bits->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || bits_offset > bits->desc.Width || bits_offset > 0xFFFFFFFFu) return;
  if (!meta || meta->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || meta_offset > meta->desc.Width || meta_offset > 0xFFFFFFFFu) return;
  if (!vencx_planes(input, input_sub, format, &il, &ic)) return;
  if (recon && !vencx_planes(recon, recon_sub, format, &rl, &rc)) return;
  if (rf->NumTexture2Ds > VENC_MAX_REFERENCES || (rf->NumTexture2Ds && !rf->ppTexture2Ds)) return;
  UINT refs[2 * VENC_MAX_REFERENCES];
  for (UINT i = 0; i < rf->NumTexture2Ds; ++i)
    if (!vencx_planes((Resource *)rf->ppTexture2Ds[i], rf->pSubresources ? rf->pSubresources[i] : 0, format, &refs[2 * i], &refs[2 * i + 1])) return;

  /* the kernel's arguments (VideoEncodeCodecs.hpp); what is wrong with them goes to the metadata as errors */
  UINT w[VENC_PARAM_WORDS];
  memset(w, 0, sizeof w);
  const UINT width = seq->PictureTargetResolution.Width, height = seq->PictureTargetResolution.Height;
  const UINT bit_depth = format == DXGI_FORMAT_P010 ? 10 : 8;
  int listed = 0;
  for (UINT i = 0; i < h->desc.ResolutionsListCount; ++i) listed |= h->resolutions[i].Width == width && h->resolutions[i].Height == height;
  const int same = e->desc.EncodeCodec == h->desc.EncodeCodec &&
                   (codec == D3D12_VIDEO_ENCODER_CODEC_HEVC ? e->x.hevc_profile == h->x.hevc_profile : e->x.av1_profile == h->x.av1_profile);
  if (!same || !listed || input->desc.Width < width || input->desc.Height < height || seq->IntraRefreshConfig.Mode != D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE)
    errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
  if (recon && (recon->desc.Width < width || recon->desc.Height < height)) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_INVALID_REFERENCE_PICTURES;
  /* (EncodeFrame1's input maps from the CPU and the PSNR metadata are H.264's) */
  if (x && (x->dirty || x->moves || x->optional)) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
  if (!vencx_gop(codec, &seq->CodecGopSequence)) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
  if (!vencx_rate_control(&seq->RateControl, w, vencx_max_qp(codec))) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
  w[2] = width;
  w[3] = height;
  w[33] = e->desc.MaxMotionEstimationPrecision;
  const UINT64 room = meta->desc.Width - meta_offset;
  VencxWords a = {NULL, 0, 0, 0};
  UINT qp_bytes = 0;
  const void *qp_map = NULL;

  if (codec == D3D12_VIDEO_ENCODER_CODEC_HEVC) {
    const D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC *c = &e->x.hevc_config;
    w[0] = VENCX_CODEC_HEVC;
    w[1] = e->x.hevc_profile == D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10;
    w[4] = c->ConfigurationFlags;
    w[5] = c->MinLumaCodingUnitSize; w[6] = c->MaxLumaCodingUnitSize; w[7] = c->MinLumaTransformUnitSize; w[8] = c->MaxLumaTransformUnitSize;
    w[9] = c->max_transform_hierarchy_depth_inter | (UINT)c->max_transform_hierarchy_depth_intra << 8;
    if (!vencx_resolution_ok(codec, c, &seq->PictureTargetResolution)) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
    if (vencx_gop(codec, &seq->CodecGopSequence)) w[39] = seq->CodecGopSequence.pHEVCGroupOfPictures->log2_max_pic_order_cnt_lsb_minus4;
    w[31] = seq->SelectedLayoutMode;
    if (!vencx_mode_ok(codec, seq->SelectedLayoutMode)) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_SUBREGION_LAYOUT_CONFIGURATION_NOT_SUPPORTED;
    else if (seq->SelectedLayoutMode != D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME) {
      const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA *ld = &seq->FrameSubregionsLayoutData;
      if (ld->DataSize != sizeof(D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES) || !ld->pSlicesPartition_HEVC) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_SUBREGION_LAYOUT_CONFIGURATION_NOT_SUPPORTED;
      else w[32] = ld->pSlicesPartition_HEVC->NumberOfCodingUnitsPerSlice;
    }
    /* the picture: D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC, or HEVC1 (its range extension values must be the defaults: Main has none) */
    const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC *p = pdata;
    if (codec_data_size == sizeof(D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1) && p) {
      const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC1 *p1 = pdata;
      int rext = p1->diff_cu_chroma_qp_offset_depth || p1->log2_sao_offset_scale_luma || p1->log2_sao_offset_scale_chroma || p1->log2_max_transform_skip_block_size_minus2 ||
                 p1->chroma_qp_offset_list_len_minus1;
      for (int i = 0; i < 6; ++i) rext |= p1->cb_qp_offset_list[i] || p1->cr_qp_offset_list[i];
      if (rext) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
    } else if (codec_data_size != sizeof *p || !p) {
      errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
      p = NULL;
    }
    UINT nd = 0, n0 = 0, n1 = 0, nm0 = 0, nm1 = 0, nq = 0;
    if (p) {
      w[34] = p->FrameType;
      w[35] = p->slice_pic_parameter_set_id;
      w[36] = p->PictureOrderCountNumber;
      w[37] = p->TemporalLayerIndex;
      w[38] = ((pflags & D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_USED_AS_REFERENCE_PICTURE) ? 1 : 0) |
              ((p->Flags & D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC_FLAG_REQUEST_INTRA_CONSTRAINED_SLICES) ? 2 : 0);
      nd = p->ReferenceFramesReconPictureDescriptorsCount;
      n0 = p->List0ReferenceFramesCount;
      n1 = p->List1ReferenceFramesCount;
      nm0 = p->List0RefPicModificationsCount;
      nm1 = p->List1RefPicModificationsCount;
      nq = p->QPMapValuesCount;
      if (nd > VENC_MAX_REFERENCES || n0 > 15 || n1 > 15 || (nd && !p->pReferenceFramesReconPictureDescriptors) || (n0 && !p->pList0ReferenceFrames) || (n1 && !p->pList1ReferenceFrames)) {
        errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_INVALID_REFERENCE_PICTURES;
        nd = n0 = n1 = 0;
      }
      if (nm0 > 15 || nm1 > 15 || (nm0 && !p->pList0RefPicModifications) || (nm1 && !p->pList1RefPicModifications) || nq > (1u << 20) || (nq && !p->pRateControlQPMap)) {
        errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
        nm0 = nm1 = nq = 0;
      }
      for (UINT i = 0; i < nd; ++i)
        if (p->pReferenceFramesReconPictureDescriptors[i].ReconstructedPictureResourceIndex >= rf->NumTexture2Ds) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_INVALID_REFERENCE_PICTURES;
    }
    w[40] = nd; w[41] = n0; w[42] = n1; w[43] = nm0; w[44] = nm1; w[45] = nq;
    w[47] = room >= VENC_METADATA_HEADER ? (UINT)((room - VENC_METADATA_HEADER) / VENC_SUBREGION_BYTES) : 0;
    w[48] = bit_depth;
    for (UINT i = 0; i < VENC_PARAM_WORDS; ++i) vencx_put(&a, i == 46 ? errors : w[i]);
    for (UINT i = 0; i < nd; ++i) {
      const D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_HEVC *r = &p->pReferenceFramesReconPictureDescriptors[i];
      vencx_put(&a, r->ReconstructedPictureResourceIndex); vencx_put(&a, (UINT)r->IsRefUsedByCurrentPic); vencx_put(&a, (UINT)r->IsLongTermReference);
      vencx_put(&a, r->PictureOrderCountNumber); vencx_put(&a, r->TemporalLayerIndex);
    }
    for (UINT i = 0; i < n0; ++i) vencx_put(&a, p->pList0ReferenceFrames[i]);
    for (UINT i = 0; i < n1; ++i) vencx_put(&a, p->pList1ReferenceFrames[i]);
    for (UINT i = 0; i < nm0; ++i) vencx_put(&a, p->pList0RefPicModifications[i]);
    for (UINT i = 0; i < nm1; ++i) vencx_put(&a, p->pList1RefPicModifications[i]);
    qp_bytes = nq;   /* (signed bytes) */
    qp_map = nq ? (const void *)p->pRateControlQPMap : NULL;
  } else {
    const D3D12_VIDEO_ENCODER_AV1_CODEC_CONFIGURATION *c = &e->x.av1_config;
    w[0] = VENCX_CODEC_AV1;
    w[1] = 0;
    w[4] = c->FeatureFlags;
    w[5] = c->OrderHintBitsMinus1;
    w[6] = bit_depth;
    if (!vencx_resolution_ok(codec, NULL, &seq->PictureTargetResolution)) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
    /* the tiles */
    const D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_TILES *tiles = NULL;
    w[7] = seq->SelectedLayoutMode;
    w[8] = w[9] = 1;
    if (!vencx_mode_ok(codec, seq->SelectedLayoutMode)) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_SUBREGION_LAYOUT_CONFIGURATION_NOT_SUPPORTED;
    else if (seq->SelectedLayoutMode != D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME) {
      const D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA *ld = &seq->FrameSubregionsLayoutData;
      if (ld->DataSize != sizeof(D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_TILES) || !ld->pTilesPartition_AV1 || ld->pTilesPartition_AV1->RowCount == 0 ||
          ld->pTilesPartition_AV1->ColCount == 0 || ld->pTilesPartition_AV1->RowCount > 64 || ld->pTilesPartition_AV1->ColCount > 64)
        errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_SUBREGION_LAYOUT_CONFIGURATION_NOT_SUPPORTED;
      else {
        tiles = ld->pTilesPartition_AV1;
        w[8] = (UINT)tiles->RowCount;
        w[9] = (UINT)tiles->ColCount;
        w[31] = (UINT)tiles->ContextUpdateTileId;
      }
    }
    if (seq->SelectedLayoutMode == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_CONFIGURABLE_GRID_PARTITION_ && !tiles) w[7] = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
    const D3D12_VIDEO_ENCODER_AV1_PICTURE_CONTROL_CODEC_DATA *p = pdata;
    if (codec_data_size != sizeof *p || !p) { errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED; p = NULL; }
    UINT nq = 0;
    if (p) {
      w[34] = p->FrameType; w[35] = p->Flags; w[36] = p->CompoundPredictionType; w[37] = p->InterpolationFilter; w[38] = p->TxMode; w[39] = p->SuperResDenominator;
      w[40] = p->OrderHint; w[41] = p->PictureIndex; w[42] = (p->TemporalLayerIndexPlus1 & 0xFFFF) | p->SpatialLayerIndexPlus1 << 16; w[43] = p->PrimaryRefFrame;
      w[44] = p->RefreshFrameFlags;
      nq = p->QPMapValuesCount;
      if (nq > (1u << 20) || (nq && !p->pRateControlQPMap)) { errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED; nq = 0; }
      if (p->TemporalLayerIndexPlus1 > 0xFFFF || p->SpatialLayerIndexPlus1 > 0xFFFF) errors |= D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_CODEC_PICTURE_CONTROL_NOT_SUPPORTED;
    }
    w[45] = nq;
    w[47] = room >= VENC_METADATA_HEADER + VENCX_AV1_TAIL ? (UINT)((room - VENC_METADATA_HEADER - VENCX_AV1_TAIL) / VENC_SUBREGION_BYTES) : 0;
    for (UINT i = 0; i < VENC_PARAM_WORDS; ++i) vencx_put(&a, i == 46 ? errors : w[i]);
    /* the fixed part: references, restoration, loop filter, quantization, CDEF */
    for (UINT i = 0; i < 7; ++i) vencx_put(&a, p ? p->ReferenceIndices[i] : 0);
    for (UINT i = 0; i < 8; ++i) {
      const D3D12_VIDEO_ENCODER_AV1_REFERENCE_PICTURE_DESCRIPTOR *r = p ? &p->ReferenceFramesReconPictureDescriptors[i] : NULL;
      vencx_put(&a, r && r->ReconstructedPictureResourceIndex < rf->NumTexture2Ds ? r->ReconstructedPictureResourceIndex : 0xFFFFFFFFu);
      vencx_put(&a, r ? r->OrderHint : 0);
      vencx_put(&a, r ? (UINT)r->FrameType : 0);
      vencx_put(&a, r ? r->PictureIndex : 0);
    }
    for (UINT i = 0; i < 3; ++i) vencx_put(&a, p ? (UINT)p->FrameRestorationConfig.FrameRestorationType[i] : 0);
    for (UINT i = 0; i < 3; ++i) vencx_put(&a, p ? (UINT)p->FrameRestorationConfig.LoopRestorationPixelSize[i] : 0);
    const D3D12_VIDEO_ENCODER_CODEC_AV1_LOOP_FILTER_CONFIG *lf = p ? &p->LoopFilter : NULL;
    vencx_put(&a, lf ? (UINT)lf->LoopFilterLevel[0] : 0); vencx_put(&a, lf ? (UINT)lf->LoopFilterLevel[1] : 0);
    vencx_put(&a, lf ? (UINT)lf->LoopFilterLevelU : 0); vencx_put(&a, lf ? (UINT)lf->LoopFilterLevelV : 0);
    vencx_put(&a, lf ? (UINT)lf->LoopFilterSharpnessLevel : 0); vencx_put(&a, lf ? (UINT)lf->LoopFilterDeltaEnabled : 0); vencx_put(&a, lf ? (UINT)lf->UpdateRefDelta : 0);
    for (UINT i = 0; i < 8; ++i) vencx_put(&a, lf ? (UINT)lf->RefDeltas[i] : 0);
    vencx_put(&a, lf ? (UINT)lf->UpdateModeDelta : 0);
    for (UINT i = 0; i < 2; ++i) vencx_put(&a, lf ? (UINT)lf->ModeDeltas[i] : 0);
    vencx_put(&a, p ? (UINT)p->LoopFilterDelta.DeltaLFPresent : 0); vencx_put(&a, p ? (UINT)p->LoopFilterDelta.DeltaLFMulti : 0); vencx_put(&a, p ? (UINT)p->LoopFilterDelta.DeltaLFRes : 0);
    const D3D12_VIDEO_ENCODER_CODEC_AV1_QUANTIZATION_CONFIG *q = p ? &p->Quantization : NULL;
    vencx_put(&a, q ? (UINT)q->BaseQIndex : 0); vencx_put(&a, q ? (UINT)q->YDCDeltaQ : 0); vencx_put(&a, q ? (UINT)q->UDCDeltaQ : 0); vencx_put(&a, q ? (UINT)q->UACDeltaQ : 0);
    vencx_put(&a, q ? (UINT)q->VDCDeltaQ : 0); vencx_put(&a, q ? (UINT)q->VACDeltaQ : 0); vencx_put(&a, q ? (UINT)q->UsingQMatrix : 0);
    vencx_put(&a, q ? (UINT)q->QMY : 0); vencx_put(&a, q ? (UINT)q->QMU : 0); vencx_put(&a, q ? (UINT)q->QMV : 0);
    vencx_put(&a, p ? (UINT)p->QuantizationDelta.DeltaQPresent : 0); vencx_put(&a, p ? (UINT)p->QuantizationDelta.DeltaQRes : 0);
    const D3D12_VIDEO_ENCODER_AV1_CDEF_CONFIG *cd = p ? &p->CDEF : NULL;
    vencx_put(&a, cd ? (UINT)cd->CdefBits : 0); vencx_put(&a, cd ? (UINT)cd->CdefDampingMinus3 : 0);
    for (UINT i = 0; i < 8; ++i) vencx_put(&a, cd ? (UINT)cd->CdefYPriStrength[i] : 0);
    for (UINT i = 0; i < 8; ++i) vencx_put(&a, cd ? (UINT)cd->CdefUVPriStrength[i] : 0);
    for (UINT i = 0; i < 8; ++i) vencx_put(&a, cd ? (UINT)cd->CdefYSecStrength[i] : 0);
    for (UINT i = 0; i < 8; ++i) vencx_put(&a, cd ? (UINT)cd->CdefUVSecStrength[i] : 0);
    if (w[7] == D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_CONFIGURABLE_GRID_PARTITION_ && tiles) {
      for (UINT i = 0; i < w[8]; ++i) vencx_put(&a, (UINT)tiles->RowHeights[i]);
      for (UINT i = 0; i < w[9]; ++i) vencx_put(&a, (UINT)tiles->ColWidths[i]);
    }
    qp_bytes = nq * 2;   /* (16-bit values) */
    qp_map = nq ? (const void *)p->pRateControlQPMap : NULL;
  }
  if (a.failed) { mem_free(a.w); return; }
  const UINT bytes = a.n * 4 + (qp_bytes + 3) / 4 * 4;
  BYTE *blob = mem_alloc(bytes);
  if (!blob) { mem_free(a.w); return; }
  memset(blob, 0, bytes);
  memcpy(blob, a.w, a.n * 4);
  mem_free(a.w);
  if (qp_bytes) memcpy(blob + a.n * 4, qp_map, qp_bytes);
  const UINT args = list_temp_buffer(l, blob, bytes);
  mem_free(blob);
  if (!args) return;
  ensure_room(l, 128 + 8 * rf->NumTexture2Ds, 12 + 2 * rf->NumTexture2Ds);
  UINT pw[14 + 2 * VENC_MAX_REFERENCES];
  pw[0] = alloc_index(l, args); pw[1] = 0; pw[2] = bytes;
  pw[3] = alloc_index(l, h->alloc); pw[4] = 0;
  pw[5] = alloc_index(l, il); pw[6] = alloc_index(l, ic);
  pw[7] = recon ? alloc_index(l, rl) : 0xFFFFFFFFu; pw[8] = recon ? alloc_index(l, rc) : 0xFFFFFFFFu;
  pw[9] = alloc_index(l, bits->alloc); pw[10] = (UINT)bits_offset;
  pw[11] = alloc_index(l, meta->alloc); pw[12] = (UINT)meta_offset;
  pw[13] = rf->NumTexture2Ds;
  for (UINT i = 0; i < 2 * rf->NumTexture2Ds; ++i) pw[14 + i] = alloc_index(l, refs[i]);
  packet(l, OP_ENCODE_FRAME, pw, 14 + 2 * rf->NumTexture2Ds);
  keep(l, (IUnknown *)e);
  keep(l, (IUnknown *)h);
}
