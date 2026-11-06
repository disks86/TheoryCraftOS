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


#include "Hardware/VideoCodec/Intel/IntelEncodeCommands.hpp"
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandBuilder.hpp"

namespace hardware {
namespace intel_enc {

namespace {

using namespace build;

#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImages.inc"
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImagesGen11.inc"
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImagesGen12.inc"
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImagesXeLpgHevc.inc"   // (the pipe buffer command and the weights are the same for H.264)
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImagesXeLpgAvc.inc"
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImagesXe2LpmAvc.inc"   // (only the image state differs from Xe-LPG's)
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImagesXe2HpmAvc.inc"   // (and the pipe mode select)
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImagesXeHpgAvc.inc"   // (Arc: its own CMD3 and image state; everything else is Xe-LPG's)
#include "Hardware/VideoCodec/Intel/IntelVdencTables.inc"

// log2(8) = 3 ... the media driver's 4.4 floating-point cost format: a 4-bit mantissa and a 4-bit exponent, saturating at `max`.
uint8_t map44(uint32_t value, uint8_t max) {
  if (value == 0) return 0;
  const uint32_t max_cost = static_cast<uint32_t>(max & 15) << (max >> 4);
  if (value >= max_cost) return max;
  int log2 = 0;
  while ((value >> (log2 + 1)) != 0) ++log2;
  const int d = log2 - 3 < 0 ? 0 : log2 - 3;
  uint8_t ret = static_cast<uint8_t>((d << 4) + ((value + (d == 0 ? 0 : (1u << (d - 1)))) >> d));
  if ((ret & 0xF) == 0) ret |= 8;
  return ret;
}

// The largest vertical motion vector range of the level, in quarter samples (CodecHalAvcEncode_GetMaxVmvR).
uint32_t max_vmv_r(uint32_t level_idc) {
  switch (level_idc) {
    case 10: case 9: return 64 * 4;
    case 11: case 12: case 13: case 20: return 128 * 4;
    case 21: case 22: case 30: return 256 * 4;
    case 31: case 32: case 40: case 41: case 42: case 50: case 51: case 52: return 512 * 4;
    default: return 128 * 4;
  }
}

// The target usage the picture state is tuned for: 4, "normal".
constexpr bool kForwardTransformSkip = true, kBlockBasedSkip = true;

void put_bytes(uint32_t (&image)[kVdencConstQptStateDwords], uint32_t first_dword, uint32_t index, uint32_t value, uint32_t bytes) {
  // byte-wide (bytes == 1) or half-word-wide (bytes == 2) array entries, little endian
  const uint32_t at = first_dword * 4 + index * bytes;
  for (uint32_t b = 0; b < bytes; ++b) {
    uint32_t& dw = image[(at + b) / 4];
    const uint32_t shift = ((at + b) % 4) * 8;
    dw = (dw & ~(0xFFu << shift)) | (((value >> (8 * b)) & 0xFFu) << shift);
  }
}

void mfx_surface(DwordWriter& w, uint32_t id, uint32_t width, uint32_t height, const Nv12TiledLayout& layout, uint32_t uv_alignment) {
  Command<kMfxSurfaceStateDwords> c(kMfxSurfaceStateDefault);
  c.set(kMfxSurfaceState_SurfaceId, id);
  c.set(kMfxSurfaceState_Height, height - 1);
  c.set(kMfxSurfaceState_Width, width - 1);
  c.set(kMfxSurfaceState_TileWalk, 1);   // Y major
  c.set(kMfxSurfaceState_TiledSurface, 1);
  c.set(kMfxSurfaceState_InterleaveChroma, 1);
  c.set(kMfxSurfaceState_SurfacePitch, layout.pitch - 1);
  c.set(kMfxSurfaceState_SurfaceFormat, intel_mfx::kSurfacePlanar420);
  const uint32_t chroma_y = align_up(layout.chroma_offset_rows, uv_alignment);
  c.set(kMfxSurfaceState_YOffsetForUCb, chroma_y);
  c.set(kMfxSurfaceState_YOffsetForVCr, chroma_y);
  c.emit(w);
}

// The three VDEnc surface state commands share their body (VDENC_Surface_State_Fields: dwords 2..5 of the command). Before Xe-LP the format is in bits 28 .. 31 behind an
// "interleave chroma" bit 27; Xe-LP's definition (and every later one) has a five-bit format from bit 27, so NV12 (4) is bit 29.
template <uint32_t N>
void vdenc_surface(DwordWriter& w, const uint32_t (&defaults)[N], uint32_t width, uint32_t height, const Nv12TiledLayout& layout, bool format_from_bit27, uint32_t chroma_filter = 0) {
  Command<N> c(defaults);
  c.raw(2, ((height - 1) << 18) | ((width - 1) << 4));                 // width, height (dword 0 of the body)
  const uint32_t format = format_from_bit27 ? intel_mfx::kSurfacePlanar420 << 27 : (1u << 27) | (intel_mfx::kSurfacePlanar420 << 28);
  c.raw(3, 1u | (1u << 1) | ((layout.pitch - 1) << 3) | (chroma_filter << 20) | format);       // Y-major tiled, pitch, format (interleaved chroma)
  c.raw(4, layout.chroma_offset_rows);                                 // Y offset of the Cb plane
  c.raw(5, layout.chroma_offset_rows);                                 // ... of the Cr plane (the same: interleaved)
  c.emit(w);
}

// VDENC_PIPE_BUF_ADDR_STATE. Dword layout: three dwords (low address, high address, attributes) per buffer. DS refs 1, 4, 7; original picture 10; stream-in 13; row store 16;
// co-located MV 19; forward references 22, 25, 28; backward reference 31; statistics stream-out 34. Ice Lake and Xe-LP append more buffers (HEVC, VP9, AV1) that H.264 leaves
// zero.
template <uint32_t N>
void vdenc_pipe_buf(DwordWriter& w, const uint32_t (&defaults)[N], const AvcEncodePicture& p, const AvcEncodeBuffers& b) {
  Command<N> c(defaults);
  if (p.p_picture) {
    c.buffer(1, b.reference_ds4, 0);
    c.buffer(22, b.reference, kMocs);
  }
  c.buffer(10, b.source, kMocs);
  if (b.vdenc_row_store) c.buffer(16, b.vdenc_row_store, kMocs);
  if (b.vdenc_statistics) c.buffer(34, b.vdenc_statistics, kMocs);
  if (p.b_picture) {
    c.buffer(31, b.backward_reference, kMocs);   // list 1: the later anchor, its down-scaled copy (DsBwdRef0) and its motion vectors (the colocated picture)
    c.buffer(7, b.backward_ds4, 0);
    c.buffer(19, b.colocated_mv_read, kMocs);
  }
  if (p.b_mode && p.p_picture && !p.b_picture && b.colocated_mv_write) c.buffer(83, b.colocated_mv_write, kMocs);   // a P picture leaves its motion vectors for the B picture before it
  if (p.xe_lpg || p.xe_hpg) {
    // Xe-LPG and Arc: the VDEnc engine writes the 4x down-scaled reconstruction itself ("scaled reference surface stage 1"), the MFX engine's intra row store is also given to it, and
    // with target usage 7 (performance mode, one reference) the second forward down-scaled reference is the first.
    if (b.recon_ds4) c.buffer(49, b.recon_ds4, kMocs);
    if (b.intra_row_store) c.buffer(77, b.intra_row_store, kMocs);
    if (p.p_picture) c.buffer(4, b.reference_ds4, 0);
  }
  c.emit(w);
}

void flat_matrices(DwordWriter& w) {
  // MFX_QM_STATE: 16 dwords of matrix (flat: every weight 16); MFX_FQM_STATE: 32 dwords of 16-bit reciprocals (65536 / 16).
  for (uint32_t type = 0; type < 4; ++type) {
    Command<kMfxQmStateDwords> q(kMfxQmStateDefault);
    q.raw(1, type);
    for (uint32_t i = 2; i < kMfxQmStateDwords; ++i) q.raw(i, 0x10101010u);
    q.emit(w);
  }
  for (uint32_t type = 0; type < 4; ++type) {
    Command<kMfxFqmStateDwords> q(kMfxFqmStateDefault);
    q.raw(1, type);
    for (uint32_t i = 2; i < kMfxFqmStateDwords; ++i) q.raw(i, 0x10001000u);
    q.emit(w);
  }
}

}  // namespace

bool avc_encode_layout(uint32_t width_mbs, uint32_t height_mbs, AvcEncodeLayout* out) {
  if (width_mbs == 0 || height_mbs == 0 || width_mbs > 256 || height_mbs > 256) return false;
  out->width = width_mbs * 16;
  out->height = height_mbs * 16;
  // The 4x copy: whole macroblocks of a quarter of the picture, at least 48 samples a side.
  out->quarter_width = (out->width / 4 + 15) / 16 * 16;
  out->quarter_height = (out->height / 4 + 15) / 16 * 16;
  if (out->quarter_width < 48) out->quarter_width = 48;
  if (out->quarter_height < 48) out->quarter_height = 48;
  return intel_mfx::nv12_tiled_layout(out->width, out->height, &out->full) && intel_mfx::nv12_tiled_layout(out->quarter_width, out->quarter_height, &out->quarter);
}

AvcEncodeScratchSizes avc_encode_scratch_sizes(uint32_t width_mbs, uint32_t height_mbs) {
  (void)height_mbs;
  AvcEncodeScratchSizes s;
  s.vdenc_row_store = width_mbs * 64;
  s.vdenc_statistics = align_up(80, 4096);
  s.pak_statistics = align_up(204, 4096);
  s.intra_row_store = width_mbs * 64;
  s.deblocking_row_store = width_mbs * 4 * 64;
  s.bsd_mpc_row_store = width_mbs * 2 * 64;
  s.mpr_row_store = 0;
  return s;
}

// Rounding of P slices for a GOP without B pictures (the media driver's adaptive tables G0_P / adaptiveRoundingInter_P_G1 and adaptiveRoundingIntra_P_G1, equal on both platforms
// that use them), by slice quantiser.
constexpr uint8_t kRoundingInterG0P[52] = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
constexpr uint8_t kRoundingIntraG0P[52] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0};

constexpr uint8_t kRoundingInterP[52] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2};
constexpr uint8_t kRoundingIntraP[52] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
constexpr uint8_t kRoundingInterB[52] = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
constexpr uint8_t kRoundingIntraB[52] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};

// The adaptive rounding of P and (non-reference) B slices by quantiser: the G0 tables for a stream without B pictures, the P and B tables for one with.
static uint32_t rounding_inter(const AvcEncodePicture& p, uint32_t qp) { return !p.b_mode ? kRoundingInterG0P[qp] : p.b_picture ? kRoundingInterB[qp] : kRoundingInterP[qp]; }
static uint32_t rounding_intra(const AvcEncodePicture& p, uint32_t qp) { return !p.b_mode ? kRoundingIntraG0P[qp] : p.b_picture ? kRoundingIntraB[qp] : kRoundingIntraP[qp]; }

// VDENC_CMD3 and VDENC_AVC_IMG_STATE of Xe-LPG (the images the media driver's own parameter code produces for I and P pictures; see tools/gen_intel_avc_xelpg.py).
static void xe_lpg_avc_image_state(DwordWriter& w, const AvcEncodePicture& p, uint32_t qp) {
  const uint32_t q = qp < 10 ? 0 : qp > 51 ? 41 : qp - 10;
  const bool large = p.width_mbs * 16 >= 1920 || p.height_mbs * 16 >= 1080;   // (below that size the driver offsets the lambda of a GOP with B pictures above QP 31)
  const uint32_t* cmd3;
  const uint32_t* image;
  if (p.b_mode) {
    const uint32_t t = p.b_picture ? 2 : p.p_picture ? 1 : 0;
    static const uint32_t* const kCmd3[3][2] = {{kXeLpgAvcVdencCmd3WithBISmall, kXeLpgAvcVdencCmd3WithBILarge}, {kXeLpgAvcVdencCmd3WithBPSmall, kXeLpgAvcVdencCmd3WithBPLarge},
                                                 {kXeLpgAvcVdencCmd3WithBBSmall, kXeLpgAvcVdencCmd3WithBBLarge}};
    cmd3 = kCmd3[t][large ? 1 : 0] + q * kXeLpgAvcVdencCmd3Dwords;   // (the other platforms of this family have the same CMD3)
    static const uint32_t* const kImg[3][3] = {{kXeLpgAvcVdencImgStateWithBI, kXeLpgAvcVdencImgStateWithBP, kXeLpgAvcVdencImgStateWithBB},
                                                {kXe2LpmAvcVdencImgStateWithBI, kXe2LpmAvcVdencImgStateWithBP, kXe2LpmAvcVdencImgStateWithBB},
                                                {kXe2HpmAvcVdencImgStateWithBI, kXe2HpmAvcVdencImgStateWithBP, kXe2HpmAvcVdencImgStateWithBB}};
    image = kImg[p.xe2][t];
  } else {
    cmd3 = (p.p_picture ? kXeLpgAvcVdencCmd3TableP : kXeLpgAvcVdencCmd3TableI) + q * kXeLpgAvcVdencCmd3Dwords;
    image = p.xe2 == 1 ? (p.p_picture ? kXe2LpmAvcVdencImgStateP : kXe2LpmAvcVdencImgStateI)
          : p.xe2 == 2 ? (p.p_picture ? kXe2HpmAvcVdencImgStateP : kXe2HpmAvcVdencImgStateI)
                       : (p.p_picture ? kXeLpgAvcVdencImgStateP : kXeLpgAvcVdencImgStateI);
  }
  for (uint32_t i = 0; i < kXeLpgAvcVdencCmd3Dwords; ++i) w.emit(cmd3[i]);
  static_assert(kXeLpgAvcVdencImgStateDwords == kXe2LpmAvcVdencImgStateDwords && kXeLpgAvcVdencImgStateDwords == kXe2HpmAvcVdencImgStateDwords, "26 dwords on every platform");
  uint32_t copy[kXeLpgAvcVdencImgStateDwords];
  for (uint32_t i = 0; i < kXeLpgAvcVdencImgStateDwords; ++i) copy[i] = image[i];
  Command<kXeLpgAvcVdencImgStateDwords> c(copy);
  c.raw(8, (p.p_picture ? kXeLpgAvcVdencImgStatePLevelDword8 : kXeLpgAvcVdencImgStateILevelDword8)[p.level_idc > 52 ? 52 : p.level_idc]);
  c.set(kXeLpgAvcVdencImgState_QpPrimeY, qp);
  c.set(kXeLpgAvcVdencImgState_PictureWidth, p.width_mbs);
  c.set(kXeLpgAvcVdencImgState_PictureHeightMinusOne, p.height_mbs - 1);
  c.set(kXeLpgAvcVdencImgState_PocNumberForCurrentPicture, p.poc & 0xFF);
  if (p.p_picture) {
    c.set(kXeLpgAvcVdencImgState_FwdRefIdx0ReferencePicture, p.reference_frame_store);
    c.set(kXeLpgAvcVdencImgState_PocNumberForFwdRef0, p.reference_poc & 0xFF);
  }
  if (p.b_picture) {
    c.set(kXeLpgAvcVdencImgState_BwdRefIdx0ReferencePicture, p.backward_reference_frame_store);
    c.set(kXeLpgAvcVdencImgState_PocNumberForBwdRef0, p.backward_poc & 0xFF);
  }
  c.emit(w);
}

// VDENC_CMD3 and the 20-dword VDENC_AVC_IMG_STATE of Arc (tools/gen_intel_avc_xehpg.py: the tables of the media driver's Xe_HPM interface).
static void xe_hpg_avc_image_state(DwordWriter& w, const AvcEncodePicture& p, uint32_t qp) {
  const uint32_t q = qp < 10 ? 0 : qp > 51 ? 41 : qp - 10;
  const uint32_t* cmd3 = (p.p_picture ? kXeHpgAvcVdencCmd3TableP : kXeHpgAvcVdencCmd3TableI) + q * kXeHpgAvcVdencCmd3Dwords;
  for (uint32_t i = 0; i < kXeHpgAvcVdencCmd3Dwords; ++i) w.emit(cmd3[i]);
  Command<kXeHpgAvcVdencImgStateDwords> c(p.p_picture ? kXeHpgAvcVdencImgStateP : kXeHpgAvcVdencImgStateI);
  c.raw(8, (c.get(8) & 0xffff) | kXeHpgAvcVdencImgStateLevelDword8[p.level_idc > 52 ? 52 : p.level_idc]);
  c.set(kXeHpgAvcVdencImgState_PictureType, p.p_picture ? 1 : 0);
  c.set(kXeHpgAvcVdencImgState_Transform8X8Flag, 0);
  c.set(kXeHpgAvcVdencImgState_SubpelMode, 3);
  c.set(kXeHpgAvcVdencImgState_PictureWidth, p.width_mbs);
  c.set(kXeHpgAvcVdencImgState_PictureHeightMinusOne, p.height_mbs - 1);
  c.set(kXeHpgAvcVdencImgState_MinQp, 0xa);
  c.set(kXeHpgAvcVdencImgState_MaxQp, 0x33);
  c.set(kXeHpgAvcVdencImgState_QpPrimeY, qp);
  c.set(kXeHpgAvcVdencImgState_PocNumberForCurrentPicture, p.poc & 0xFF);
  if (p.p_picture) {
    c.set(kXeHpgAvcVdencImgState_NumberOfL0ReferencesMinusOne, 0);
    c.set(kXeHpgAvcVdencImgState_NumberOfL1ReferencesMinusOne, 0);
    c.set(kXeHpgAvcVdencImgState_FwdRefIdx0ReferencePicture, p.reference_frame_store);
    c.set(kXeHpgAvcVdencImgState_FwdRefIdx1ReferencePicture, 0xf);
    c.set(kXeHpgAvcVdencImgState_FwdRefIdx2ReferencePicture, 0xf);
    c.set(kXeHpgAvcVdencImgState_PocNumberForFwdRef0, p.reference_poc & 0xFF);
  }
  c.emit(w);
}

void emit_avc_vdenc_picture(DwordWriter& w, const AvcEncodePicture& p, const AvcEncodeBuffers& b, const AvcEncodeLayout& layout) {
  const uint32_t mbs = p.width_mbs * p.height_mbs;
  const uint32_t qp = static_cast<uint32_t>(p.qp < 0 ? 0 : p.qp > static_cast<int32_t>(kMaxQp) ? kMaxQp : p.qp);

  const bool gen11_up = p.generation != EncodeGeneration::Gen9;
  // --- Xe-LP starts a VDEnc picture by initialising the VDEnc pipe.
  if (p.generation == EncodeGeneration::Gen12) {
    Command<kGen12VdencControlStateDwords> c(kGen12VdencControlStateDefault);
    c.set(kGen12VdencControlState_VdencInitialization, 1);
    c.emit(w);
  }
  // --- MFX_PIPE_MODE_SELECT: encode, VDEnc, the picture leaves through the post-deblocking path when the filter runs. Ice Lake and later wait for the MFX pipe on both sides.
  if (gen11_up) intel_mfx::emit_mfx_wait_sync(w);
  {
    Command<kMfxPipeModeSelectDwords> c(kMfxPipeModeSelectDefault);
    c.set(kMfxPipeModeSelect_StandardSelect, kMfxPipeModeSelect_STANDARD_SELECT_AVC);
    c.set(kMfxPipeModeSelect_CodecSelect, kMfxPipeModeSelect_CODEC_SELECT_ENCODE);
    c.set(kMfxPipeModeSelect_DecoderShortFormatMode, 1);   // (the encoder's "long format": the engine does not move the next slice's position)
    c.set(kMfxPipeModeSelect_VdencMode, 1);
    c.set(kMfxPipeModeSelect_ScaledSurfaceEnable, p.xe_lpg ? 0 : 1);   // (Xe-LPG: the VDEnc engine writes the down-scaled reconstruction itself)
    c.set(kMfxPipeModeSelect_StreamOutEnable, 0);
    c.set(kMfxPipeModeSelect_FrameStatisticsStreamoutEnable, 1);
    c.set(kMfxPipeModeSelect_PreDeblockingOutputEnablePredeblockoutenable, p.deblocking ? 0 : 1);
    c.set(kMfxPipeModeSelect_PostDeblockingOutputEnablePostdeblockoutenable, p.deblocking ? 1 : 0);
    c.emit(w);
  }
  if (gen11_up) intel_mfx::emit_mfx_wait_sync(w);
  // --- The surfaces: reconstruction, source, 4x down-scaled reconstruction (the raw ones are aligned to 4 rows, the others to 8).
  mfx_surface(w, kMfxSurfaceRecon, layout.width, layout.height, layout.full, 8);
  mfx_surface(w, kMfxSurfaceSource, layout.width, layout.height, layout.full, 4);
  mfx_surface(w, kMfxSurfaceDownscaledRecon, layout.quarter_width, layout.quarter_height, layout.quarter, 8);

  // --- MFX_PIPE_BUF_ADDR_STATE.
  {
    Command<kMfxPipeBufAddrStateDwords> c(kMfxPipeBufAddrStateDefault);
    if (p.deblocking) c.buffer(4, b.recon, kMocs); else c.buffer(1, b.recon, kMocs);
    c.buffer(7, b.source, kMocs);
    if (b.pak_statistics) {
      c.buffer(10, b.pak_statistics, kMocs);
      c.raw(52, b.pak_statistics);
      c.raw(53, 0);
      c.raw(54, (c.get(54) & ~0x7Fu) | kMocs);
    }
    if (b.intra_row_store) c.buffer(13, b.intra_row_store, kMocs);
    if (b.deblocking_row_store) c.buffer(16, b.deblocking_row_store, kMocs);
    if (p.p_picture && b.reference) {
      // The reference pictures are indexed by frame store id (two dwords each, from dword 19).
      c.raw(19 + 2 * p.reference_frame_store, b.reference);
      c.raw(20 + 2 * p.reference_frame_store, 0);
    }
    if (p.p_picture && p.b_picture && b.backward_reference) {
      c.raw(19 + 2 * p.backward_reference_frame_store, b.backward_reference);
      c.raw(20 + 2 * p.backward_reference_frame_store, 0);
    }
    c.raw(51, (c.get(51) & ~0x7Fu) | kMocs);
    if (b.recon_ds4 && !p.xe_lpg) {
      c.raw(62, b.recon_ds4);
      c.raw(63, 0);
      c.raw(64, (c.get(64) & ~0x7Fu) | kMocs);
    }
    c.emit(w);
  }

  // --- MFX_IND_OBJ_BASE_ADDR_STATE: only the PAK-BSE object (the bitstream the engine writes); MFX_BSP_BUF_BASE_ADDR_STATE: the BSD/MPC row store.
  {
    Command<kMfxIndObjBaseAddrStateDwords> c(kMfxIndObjBaseAddrStateDefault);
    c.raw(21, b.bitstream);
    c.raw(22, 0);
    c.raw(23, (c.get(23) & ~0x7Fu) | kMocs);
    c.raw(24, b.bitstream + align_up(b.bitstream_bytes, 4096));
    c.emit(w);
  }
  {
    Command<kMfxBspBufBaseAddrStateDwords> c(kMfxBspBufBaseAddrStateDefault);
    if (b.bsd_mpc_row_store) c.buffer(1, b.bsd_mpc_row_store, kMocs);
    c.emit(w);
  }

  // --- The VDEnc pipeline: mode, surfaces, buffers, the constant quantiser tables.
  if (p.xe_lpg || p.xe_hpg) {
    // (Arc's older driver also turns the frame statistics stream-out on when it uses adaptive rounding: bit 5 of the first dword.)
    const uint32_t random_access = p.b_picture ? 1u << 18 : 0u;   // IsRandomAccess (dword 1, bit 18) for a B picture
    if (p.xe2 == 2) for (uint32_t i = 0; i < kXe2HpmAvcVdencPipeModeSelectDwords; ++i) w.emit(kXe2HpmAvcVdencPipeModeSelectImage[i] | (i == 1 ? random_access : 0u));
    else for (uint32_t i = 0; i < kXeLpgAvcVdencPipeModeSelectDwords; ++i) w.emit(kXeLpgAvcVdencPipeModeSelectImage[i] | (i == 1 && p.xe_hpg ? 1u << 5 : 0u) | (i == 1 ? random_access : 0u));
  } else if (p.generation == EncodeGeneration::Gen12) {
    // Xe-LP prefetches the TLB entries of the source picture ahead of the engine (the driver's settings for AVC).
    Command<kGen12VdencPipeModeSelectDwords> c(kGen12VdencPipeModeSelectDefault);
    c.set(kGen12VdencPipeModeSelect_StandardSelect, kGen12VdencPipeModeSelect_STANDARD_SELECT_AVC);
    c.set(kGen12VdencPipeModeSelect_FrameStatisticsStreamOutEnable, 1);
    c.set(kGen12VdencPipeModeSelect_TlbPrefetchEnable, 1);
    c.set(kGen12VdencPipeModeSelect_PakThresholdCheckEnable, 0);
    c.set(kGen12VdencPipeModeSelect_VdencStreamInEnable, 0);
    c.set(kGen12VdencPipeModeSelect_OutputRangeControlAfterColorSpaceConversion, 1);
    c.set(kGen12VdencPipeModeSelect_HmeRegionPreFetchenable, 0);
    c.set(kGen12VdencPipeModeSelect_PreFetchoffsetforsource, 7);
    c.set(kGen12VdencPipeModeSelect_Numverticalreqminus1Src, 0);
    c.emit(w);
  } else {
    Command<kVdencPipeModeSelectDwords> c(kVdencPipeModeSelectDefault);
    c.set(kVdencPipeModeSelect_StandardSelect, kVdencPipeModeSelect_STANDARD_SELECT_AVC);
    c.set(kVdencPipeModeSelect_FrameStatisticsStreamOutEnable, 1);
    c.set(kVdencPipeModeSelect_TlbPrefetchEnable, 0);
    c.set(kVdencPipeModeSelect_PakThresholdCheckEnable, 0);
    c.set(kVdencPipeModeSelect_VdencStreamInEnable, 0);
    c.emit(w);
  }
  const bool gen12_surfaces = p.generation == EncodeGeneration::Gen12;
  vdenc_surface(w, kVdencSrcSurfaceStateDefault, layout.width, layout.height, layout.full, gen12_surfaces, gen12_surfaces ? 7 : 0);   // (Xe-LP and later: chroma downsample filter 7 on the source)
  vdenc_surface(w, kVdencRefSurfaceStateDefault, layout.width, layout.height, layout.full, gen12_surfaces);
  if (gen11_up) vdenc_surface(w, kGen11VdencDsRefSurfaceStateDefault, layout.quarter_width, layout.quarter_height, layout.quarter, gen12_surfaces);   // (room for two surfaces; one is used)
  else vdenc_surface(w, kVdencDsRefSurfaceStateDefault, layout.quarter_width, layout.quarter_height, layout.quarter, false);
  if (p.xe_lpg || p.xe_hpg) {
    vdenc_pipe_buf(w, kXeLpgVdencPipeBufAddrStateImage, p, b);   // (Arc's is the same image)
  } else switch (p.generation) {
    case EncodeGeneration::Gen9: vdenc_pipe_buf(w, kVdencPipeBufAddrStateDefault, p, b); break;
    case EncodeGeneration::Gen11: vdenc_pipe_buf(w, kGen11VdencPipeBufAddrStateDefault, p, b); break;
    case EncodeGeneration::Gen12: vdenc_pipe_buf(w, kGen12VdencPipeBufAddrStateDefault, p, b); break;
  }
  if (!p.xe_lpg && !p.xe_hpg)
  {
    Command<kVdencConstQptStateDwords> c(kVdencConstQptStateDefault);
    uint32_t image[kVdencConstQptStateDwords];
    for (uint32_t i = 0; i < kVdencConstQptStateDwords; ++i) image[i] = c.get(i);
    static const uint8_t kLambda[42] = {1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 10, 12, 13, 15, 17, 19, 21, 23, 26, 30, 33, 37, 42, 47, 53, 59, 66, 74, 83};
    for (uint32_t i = 0; i < 40; ++i) put_bytes(image, 1, i, kLambda[i], 1);
    put_bytes(image, 11, 0, kLambda[40], 1);
    put_bytes(image, 11, 1, kLambda[41], 1);
    if (p.p_picture) {
      static const uint16_t kSkip[26] = {0, 0, 0, 0, 2, 4, 7, 11, 17, 25, 35, 50, 68, 91, 119, 153, 194, 241, 296, 360, 432, 513, 604, 706, 819, 944};
      for (uint32_t i = 0; i < 26; ++i) {
        uint32_t v = kSkip[i];
        if (!kBlockBasedSkip) v *= 3;
        else v /= 2;   // (no 8x8 transform in the stream)
        put_bytes(image, 12, i, v, 2);
      }
      if (kForwardTransformSkip) {
        static const uint16_t kM0[26] = {0x02, 0x02, 0x03, 0x04, 0x04, 0x05, 0x07, 0x09, 0x0b, 0x0e, 0x12, 0x14, 0x18, 0x1d, 0x20, 0x25, 0x2a, 0x34, 0x39, 0x3f, 0x4e, 0x51, 0x5b, 0x63, 0x6f, 0x7f};
        static const uint8_t kM135[26] = {0x03, 0x04, 0x05, 0x05, 0x07, 0x09, 0x0b, 0x0e, 0x12, 0x17, 0x1c, 0x21, 0x27, 0x2c, 0x33, 0x3b, 0x41, 0x51, 0x5c, 0x1a, 0x1e, 0x21, 0x22, 0x26, 0x2c, 0x30};
        static const uint8_t kM2[26] = {0x02, 0x02, 0x03, 0x04, 0x04, 0x05, 0x07, 0x09, 0x0b, 0x0e, 0x12, 0x14, 0x18, 0x1d, 0x20, 0x25, 0x2a, 0x34, 0x39, 0x0f, 0x13, 0x14, 0x16, 0x18, 0x1b, 0x1f};
        static const uint8_t kM46[26] = {0x04, 0x05, 0x06, 0x09, 0x0b, 0x0d, 0x12, 0x16, 0x1b, 0x23, 0x2c, 0x33, 0x3d, 0x45, 0x4f, 0x5b, 0x66, 0x7f, 0x8e, 0x2a, 0x2f, 0x32, 0x37, 0x3c, 0x45, 0x4c};
        for (uint32_t i = 0; i < 26; ++i) put_bytes(image, 26, i, kM0[i], 2);
        for (uint32_t i = 0; i < 24; ++i) put_bytes(image, 40, i, kM135[i], 1);
        put_bytes(image, 46, 0, kM135[24], 1);
        put_bytes(image, 46, 1, kM135[25], 1);
        for (uint32_t i = 0; i < 24; ++i) put_bytes(image, 47, i, kM2[i], 1);
        put_bytes(image, 53, 0, kM2[24], 1);
        put_bytes(image, 53, 1, kM2[25], 1);
        for (uint32_t i = 0; i < 24; ++i) put_bytes(image, 54, i, kM46[i], 1);
        put_bytes(image, 60, 0, kM46[24], 1);
        put_bytes(image, 60, 1, kM46[25], 1);
      }
    }
    for (uint32_t i = 0; i < kVdencConstQptStateDwords; ++i) w.emit(image[i]);
  }

  // --- MFX_AVC_IMG_STATE (the encoder's use of it).
  {
    Command<kMfxAvcImgStateDwords> c(kMfxAvcImgStateDefault);
    c.set(kMfxAvcImgState_FrameSize, mbs > 0xFFFF ? 0xFFFF : mbs);
    c.set(kMfxAvcImgState_FrameHeight, p.height_mbs - 1);
    c.set(kMfxAvcImgState_FrameWidth, p.width_mbs - 1);
    c.set(kMfxAvcImgState_FirstChromaQpOffset, static_cast<uint32_t>(p.chroma_qp_offset) & 0x1F);
    c.set(kMfxAvcImgState_SecondChromaQpOffset, static_cast<uint32_t>(p.chroma_qp_offset) & 0x1F);
    c.set(kMfxAvcImgState_RhodomainRateControlEnable, 1);
    c.set(kMfxAvcImgState_Framembonlyflag, 1);
    c.set(kMfxAvcImgState_Direct8X8Infflag, 1);
    c.set(kMfxAvcImgState_Chromaformatidc, 1);
    c.set(kMfxAvcImgState_Mbmvformatflag, 1);
    c.set(kMfxAvcImgState_Mvunpackedflag, 1);
    c.set(kMfxAvcImgState_IntrambmaxbitflagIntrambmaxsizereportmask, 1);
    c.set(kMfxAvcImgState_IntermbmaxbitflagIntermbmaxsizereportmask, 1);
    c.set(kMfxAvcImgState_FrameszoverflagFramebitratemaxreportmask, 1);
    c.set(kMfxAvcImgState_FrameszunderflagFramebitrateminreportmask, 1);
    c.set(kMfxAvcImgState_IntraIntermbipcmflagForceipcmcontrolmask, 1);
    c.set(kMfxAvcImgState_TrellisQuantizationChromaDisableTqchromadisable, 1);
    c.set(kMfxAvcImgState_Intrambmaxsz, 2700);
    c.set(kMfxAvcImgState_Intermbmaxsz, 4095);
    c.set(kMfxAvcImgState_Framebitratemin, 0);
    c.set(kMfxAvcImgState_Framebitrateminunit, 1);
    c.set(kMfxAvcImgState_Framebitrateminunitmode, 1);
    c.set(kMfxAvcImgState_Framebitratemax, (1u << 14) - 1);
    c.set(kMfxAvcImgState_Framebitratemaxunit, 1);
    c.set(kMfxAvcImgState_Framebitratemaxunitmode, 1);
    c.emit(w);
  }

  if (p.xe_lpg || p.xe_hpg) {
    if (p.xe_hpg) xe_hpg_avc_image_state(w, p, qp); else xe_lpg_avc_image_state(w, p, qp);
    flat_matrices(w);
    if (p.b_picture) {
      // MFX_AVC_DIRECTMODE_STATE for the encoder: no direct motion vector buffers (the VDEnc engine keeps the colocated motion vectors), only the picture order counts of the
      // reference frame stores (top and bottom field of a frame are equal) and of this picture, in dwords 37 .. 68 and 69, 70.
      w.emit(intel_mfx::kMfxAvcDirectmodeState | (71 - 2));
      for (uint32_t i = 0; i < 36; ++i) w.emit(0);
      uint32_t pocs[32] = {};
      pocs[2 * p.reference_frame_store] = pocs[2 * p.reference_frame_store + 1] = p.reference_poc;
      pocs[2 * p.backward_reference_frame_store] = pocs[2 * p.backward_reference_frame_store + 1] = p.backward_poc;
      for (uint32_t i = 0; i < 32; ++i) w.emit(pocs[i]);
      w.emit(p.poc);
      w.emit(p.poc);
    }
    return;
  }

  // --- VDENC_IMG_STATE.
  {
    Command<kVdencImgStateDwords> c(kVdencImgStateDefault);
    c.set(kVdencImgState_VdencExtendedPakObjCmdEnable, 1);
    c.set(kVdencImgState_UnidirectionalMixDisable, 0);
    c.set(kVdencImgState_IntraSadMeasureAdjustment, 2);
    c.set(kVdencImgState_SubMacroblockSubPartitionMask, 0x70);
    c.set(kVdencImgState_BilinearFilterEnable, 0);
    c.set(kVdencImgState_Mode0Cost, 10);
    c.set(kVdencImgState_Mode1Cost, 0);
    c.set(kVdencImgState_Mode2Cost, 3);
    c.set(kVdencImgState_Mode3Cost, 30);
    c.set(kVdencImgState_PenaltyForIntra16X16NondcPrediction, 36);
    c.set(kVdencImgState_PenaltyForIntra8X8NondcPrediction, 12);
    c.set(kVdencImgState_PenaltyForIntra4X4NondcPrediction, 4);
    c.set(kVdencImgState_Smallmbsizeinword, 0xFF);
    c.set(kVdencImgState_Largembsizeinword, 0xFF);
    c.set(kVdencImgState_MaxHmvR, 0x2000);
    c.set(kVdencImgState_MaxVmvR, 0x200);
    c.set(kVdencImgState_Maxdeltaqp, 0x0F);
    if (p.p_picture) {
      c.set(kVdencImgState_BidirectionalWeight, 0x20);
      c.set(kVdencImgState_SubPelMode, 3);
      c.set(kVdencImgState_BmeDisableForFbrMessage, 1);
      c.set(kVdencImgState_InterSadMeasureAdjustment, 2);
      c.set(kVdencImgState_CrePrefetchEnable, 1);
      c.set(kVdencImgState_NonSkipZeroMvCostAdded, 1);
      c.set(kVdencImgState_NonSkipMbModeCostAdded, 1);
      c.set(kVdencImgState_Mode0Cost, 7);
      c.set(kVdencImgState_Mode1Cost, 26);
      c.set(kVdencImgState_Mode2Cost, 30);
      c.set(kVdencImgState_Mode3Cost, 57);
      c.set(kVdencImgState_Mode4Cost, 8);
      c.set(kVdencImgState_Mode5Cost, 2);
      c.set(kVdencImgState_Mode6Cost, 4);
      c.set(kVdencImgState_Mode7Cost, 6);
      c.set(kVdencImgState_Mode8Cost, 5);
      c.set(kVdencImgState_Mode9Cost, 0);
      c.set(kVdencImgState_RefIdCost, 4);
      c.set(kVdencImgState_MvCost0, 0);
      c.set(kVdencImgState_MvCost1, 6);
      c.set(kVdencImgState_MvCost2, 6);
      c.set(kVdencImgState_MvCost3, 9);
      c.set(kVdencImgState_MvCost4, 10);
      c.set(kVdencImgState_MvCost5, 13);
      c.set(kVdencImgState_MvCost6, 14);
      c.set(kVdencImgState_MvCost7, 24);
      c.set(kVdencImgState_SadHaarThreshold0, 800);
      c.set(kVdencImgState_SadHaarThreshold1, 1600);
      c.set(kVdencImgState_SadHaarThreshold2, 2400);
      c.set(kVdencImgState_MidpointSadHaar, 0x640);
    }
    c.set(kVdencImgState_VdencPerfmode, 0);
    c.set(kVdencImgState_Transform8X8Flag, 0);
    c.set(kVdencImgState_PictureWidth, p.width_mbs);
    c.set(kVdencImgState_ForwardTransformSkipCheckEnable, kForwardTransformSkip ? 1 : 0);
    c.set(kVdencImgState_BlockBasedSkipEnabled, kBlockBasedSkip ? 1 : 0);
    c.set(kVdencImgState_CrePrefetchEnable, 0);   // (the driver's CRE prefetch setting, off by default)
    c.set(kVdencImgState_PictureHeightMinusOne, p.height_mbs - 1);
    c.set(kVdencImgState_PictureType, p.p_picture ? kVdencImgState_PICTURE_TYPE_P : kVdencImgState_PICTURE_TYPE_I);
    c.set(kVdencImgState_ConstrainedIntraPredictionFlag, 0);
    if (p.p_picture) c.set(kVdencImgState_HmeRef1Disable, 1);   // one reference and no performance mode
    c.set(kVdencImgState_SliceMacroblockHeightMinusOne, p.height_mbs - 1);
    c.set(kVdencImgState_LumaIntraPartitionMask, kVdencImgState_LUMA_INTRA_PARTITION_MASK_UNNAMED2);   // (no 8x8 transform: 8x8 intra off)
    c.set(kVdencImgState_QpPrimeY, qp);

    // The costs for this picture type and quantiser (CodechalVdencAvcState::LoadCosts; the motion vector costs only exist for P pictures).
    const uint32_t type = p.p_picture ? 1 : 0;
    uint8_t mode[12] = {};
    uint8_t mv[8] = {}, hme[8] = {};
    mode[0] = map44(kAvcModeCost[type][0][qp], 0x6F);
    mode[1] = map44(kAvcModeCost[type][1][qp], 0x8F);
    mode[2] = map44(kAvcModeCost[type][2][qp], 0x8F);
    mode[3] = map44(kAvcModeCost[type][3][qp], 0x8F);
    if (p.p_picture) {
      mode[8] = map44(kAvcModeCost[type][8][qp], 0x8F);
      mode[4] = map44(kAvcModeCost[type][4][qp], 0x8F);
      mode[5] = map44(kAvcModeCost[type][5][qp], 0x6F);
      mode[6] = map44(kAvcModeCost[type][6][qp], 0x6F);
      mode[7] = map44(kAvcModeCost[type][7][qp], 0x6F);
      mode[10] = map44(kAvcModeCost[type][10][qp], 0x6F);
      for (uint32_t i = 0; i < 8; ++i) mv[i] = map44(kMvCostSkipBiasQPel[0][i], 0x6F);
      const uint32_t row = (qp == 47 || qp == 48 || qp == 49) ? 1 : (qp == 50 || qp == 51) ? 2 : 0;
      if (row) for (uint32_t i = 3; i < 8; ++i) mv[i] = map44(kMvCostSkipBiasQPel[row][i], 0x6F);
      for (uint32_t i = 0; i < 8; ++i) hme[i] = map44(kHmeMvCost[i][qp], 0x6F);
    }
    c.set(kVdencImgState_Mode0Cost, mode[0]);
    c.set(kVdencImgState_Mode1Cost, mode[1]);
    c.set(kVdencImgState_Mode2Cost, mode[2]);
    c.set(kVdencImgState_Mode3Cost, mode[3]);
    c.set(kVdencImgState_Mode4Cost, mode[4]);
    c.set(kVdencImgState_Mode5Cost, mode[5]);
    c.set(kVdencImgState_Mode6Cost, mode[6]);
    c.set(kVdencImgState_Mode7Cost, mode[7]);
    c.set(kVdencImgState_Mode8Cost, mode[8]);
    c.set(kVdencImgState_RefIdCost, mode[10]);
    c.set(kVdencImgState_MvCost0, mv[0]);
    c.set(kVdencImgState_MvCost1, mv[1]);
    c.set(kVdencImgState_MvCost2, mv[2]);
    c.set(kVdencImgState_MvCost3, mv[3]);
    c.set(kVdencImgState_MvCost4, mv[4]);
    c.set(kVdencImgState_MvCost5, mv[5]);
    c.set(kVdencImgState_MvCost6, mv[6]);
    c.set(kVdencImgState_MvCost7, mv[7]);
    c.set(kVdencImgState_MaxVmvR, max_vmv_r(p.level_idc));
    c.set(kVdencImgState_HmeMvCost0, hme[0]);
    c.set(kVdencImgState_HmeMvCost1, hme[1]);
    c.set(kVdencImgState_HmeMvCost2, hme[2]);
    c.set(kVdencImgState_HmeMvCost3, hme[3]);
    c.set(kVdencImgState_HmeMvCost4, hme[4]);
    c.set(kVdencImgState_HmeMvCost5, hme[5]);
    c.set(kVdencImgState_HmeMvCost6, hme[6]);
    c.set(kVdencImgState_HmeMvCost7, hme[7]);
    c.set(kVdencImgState_MaxQp, 0x33);
    c.set(kVdencImgState_MinQp, 0x0A);
    c.emit(w);
  }

  flat_matrices(w);
}

namespace {

void pak_insert(DwordWriter& w, const AvcHeaderInsert& h, bool last_header, bool slice_header) {
  const uint32_t bytes = (h.bits + 7) / 8;
  const uint32_t data_dwords = (bytes + 3) / 4;
  Command<kMfxPakInsertObjectDwords> c(kMfxPakInsertObjectDefault);
  c.raw(0, kMfxPakInsertObjectDefault[0] | (kMfxPakInsertObjectDwords + data_dwords - 2));
  uint32_t bits_in_last = h.bits % 32;
  if (bits_in_last == 0) bits_in_last = 32;
  c.set(kMfxPakInsertObject_EmulationflagEmulationbytebitsinsertenable, h.emulation_prevention ? 1 : 0);
  c.set(kMfxPakInsertObject_SkipemulbytecntSkipEmulationByteCount, h.skip_emulation_bytes);
  c.set(kMfxPakInsertObject_DatabitsinlastdwSrcdataendingbitinclusion50, bits_in_last);
  c.set(kMfxPakInsertObject_LastheaderflagLastsrcheaderdatainsertcommandflag, last_header ? 1 : 0);
  c.set(kMfxPakInsertObject_SliceHeaderIndicator, slice_header ? 1 : 0);
  c.set(kMfxPakInsertObject_Headerlengthexcludefrmsize, h.emulation_prevention ? 0 : 1);
  c.emit(w);
  for (uint32_t i = 0; i < data_dwords; ++i) {
    uint32_t dw = 0;
    for (uint32_t k = 0; k < 4; ++k) {
      const uint32_t at = i * 4 + k;
      uint8_t byte = 0;
      if (at < bytes) h.data.try_get(at, &byte);
      dw |= uint32_t{byte} << (8 * k);
    }
    w.emit(dw);
  }
}

}  // namespace

bool emit_avc_vdenc_slice(DwordWriter& w, const AvcEncodePicture& p, ArrayView<const AvcHeaderInsert> headers) {
  if (headers.size() == 0) return false;
  const uint32_t qp = static_cast<uint32_t>(p.qp < 0 ? 0 : p.qp > static_cast<int32_t>(kMaxQp) ? kMaxQp : p.qp);

  // --- The reference picture lists of a P slice (one entry: the previous picture, a short-term frame) or a B slice (list 0: the earlier anchor, list 1: the later one).
  for (uint32_t list = 0; list < (p.b_picture ? 2u : p.p_picture ? 1u : 0u); ++list) {
    Command<kMfxAvcRefIdxStateDwords> c(kMfxAvcRefIdxStateDefault);
    c.set(kMfxAvcRefIdxState_RefpiclistSelect, list);
    // 32 one-byte entries from dword 2: frame store id in bits 1..4, bit 5 = frame (not a field), bit 6 = long term, bit 7 = not present.
    uint32_t entries[8] = {0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u};
    entries[0] = (entries[0] & ~0xFFu) | intel_mfx::avc_ref_idx_entry(list == 0 ? p.reference_frame_store : p.backward_reference_frame_store, false);
    for (uint32_t i = 0; i < 8; ++i) c.raw(2 + i, entries[i]);
    c.emit(w);
  }

  // --- MFX_AVC_SLICE_STATE: the whole picture is one slice.
  {
    Command<kMfxAvcSliceStateDwords> c(kMfxAvcSliceStateDefault);
    c.set(kMfxAvcSliceState_SliceType, p.b_picture ? kMfxAvcSliceState_SLICE_TYPE_BSLICE : p.p_picture ? kMfxAvcSliceState_SLICE_TYPE_PSLICE : kMfxAvcSliceState_SLICE_TYPE_ISLICE);
    c.set(kMfxAvcSliceState_NumberOfReferencePicturesInInterPredictionList0, p.p_picture ? 1 : 0);
    c.set(kMfxAvcSliceState_NumberOfReferencePicturesInInterPredictionList1, p.b_picture ? 1 : 0);
    c.set(kMfxAvcSliceState_DirectPredictionType, p.b_picture ? kMfxAvcSliceState_DIRECT_PREDICTION_TYPE_SPATIAL : 0);
    c.set(kMfxAvcSliceState_SliceAlphaC0OffsetDiv2, static_cast<uint32_t>(p.alpha_c0_offset_div2) & 0xF);
    c.set(kMfxAvcSliceState_SliceBetaOffsetDiv2, static_cast<uint32_t>(p.beta_offset_div2) & 0xF);
    c.set(kMfxAvcSliceState_SliceQuantizationParameter, qp);
    c.set(kMfxAvcSliceState_NextSliceVerticalPosition, p.height_mbs);   // the slice ends at the bottom of the picture
    c.set(kMfxAvcSliceState_DisableDeblockingFilterIndicator, p.deblocking ? 0 : 1);
    c.set(kMfxAvcSliceState_Cabaczerowordinsertionenable, 1);
    c.set(kMfxAvcSliceState_Emulationbytesliceinsertenable, 1);
    c.set(kMfxAvcSliceState_IsLastSlice, 1);
    c.set(kMfxAvcSliceState_TailInsertionPresentInBitstream, p.last_in_stream ? 1 : 0);
    c.set(kMfxAvcSliceState_SlicedataInsertionPresentInBitstream, 1);
    c.set(kMfxAvcSliceState_HeaderInsertionPresentInBitstream, 1);
    const bool newer = p.xe_lpg || p.xe_hpg;   // (the newer drivers use the G0 P tables)
    c.set(kMfxAvcSliceState_Roundinter, p.p_picture ? (newer ? rounding_inter(p, qp) : kAdaptiveInterRoundingPWithoutB[qp]) : 2);
    c.set(kMfxAvcSliceState_Roundinterenable, newer && p.p_picture ? 1 : 0);   // (the newer drivers enable the inter rounding for P slices)
    c.set(kMfxAvcSliceState_Roundintra, newer && p.p_picture ? rounding_intra(p, qp) : 5);
    c.set(kMfxAvcSliceState_Roundintraenable, 1);
    c.emit(w);
  }

  // --- Xe-LPG: VDENC_AVC_SLICE_STATE carries the same rounding values.
  if (p.xe_lpg || p.xe_hpg) {
    Command<kXeLpgAvcVdencSliceStateDwords> c(kXeLpgAvcVdencSliceStateImage);
    c.set(kXeLpgAvcVdencSliceState_RoundIntra, p.p_picture ? rounding_intra(p, qp) : 5);
    c.set(kXeLpgAvcVdencSliceState_RoundIntraEnable, 1);
    c.set(kXeLpgAvcVdencSliceState_RoundInter, p.p_picture ? rounding_inter(p, qp) : 2);
    c.set(kXeLpgAvcVdencSliceState_RoundInterEnable, p.p_picture ? 1 : 0);
    c.set(kXeLpgAvcVdencSliceState_Log2WeightDenomLuma, 0);
    c.emit(w);
  }

  // --- The headers go in front of the slice data the engine writes.
  for (uint64_t i = 0; i < headers.size(); ++i) pak_insert(w, headers[i], i + 1 == headers.size(), headers[i].slice_header);

  // --- Xe-LPG: the weights (none) and a 3-dword walker (the start is the top of the picture, the next slice starts below it).
  if (p.xe_lpg || p.xe_hpg) {
    for (uint32_t i = 0; i < kXeLpgVdencWeightsOffsetsStateDwords; ++i) w.emit(kXeLpgVdencWeightsOffsetsStateImage[i]);
    Command<kXeLpgAvcVdencWalkerStateDwords> c(kXeLpgAvcVdencWalkerStateImage);
    c.set(kXeLpgAvcVdencWalkerState_FirstSuperSlice, 1);
    c.set(kXeLpgAvcVdencWalkerState_NextsliceMbStartYPosition, p.height_mbs);
    c.emit(w);
  }

  // --- VDENC_WALKER_STATE: start at the top, the next slice starts below the picture; then wait for the VDEnc and MFX pipes.
  // (From Ice Lake the walker also carries the picture width in samples.)
  if (!p.xe_lpg && !p.xe_hpg) switch (p.generation) {
    case EncodeGeneration::Gen9: {
      Command<kVdencWalkerStateDwords> c(kVdencWalkerStateDefault);
      c.set(kVdencWalkerState_MbLcuStartYPosition, 0);
      c.set(kVdencWalkerState_MbLcuStartXPosition, 0);
      c.set(kVdencWalkerState_NextsliceMbStartYPosition, p.height_mbs);
      c.emit(w);
      break;
    }
    case EncodeGeneration::Gen11: {
      Command<kGen11VdencWalkerStateDwords> c(kGen11VdencWalkerStateDefault);
      c.set(kGen11VdencWalkerState_MbLcuStartYPosition, 0);
      c.set(kGen11VdencWalkerState_MbLcuStartXPosition, 0);
      c.set(kGen11VdencWalkerState_NextsliceMbStartYPosition, p.height_mbs);
      c.set(kGen11VdencWalkerState_TileWidth, p.width_mbs * 16 - 1);
      c.emit(w);
      break;
    }
    case EncodeGeneration::Gen12: {
      Command<kGen12VdencWalkerStateDwords> c(kGen12VdencWalkerStateDefault);
      c.set(kGen12VdencWalkerState_MbLcuStartYPosition, 0);
      c.set(kGen12VdencWalkerState_MbLcuStartXPosition, 0);
      c.set(kGen12VdencWalkerState_NextsliceMbStartYPosition, p.height_mbs);
      c.set(kGen12VdencWalkerState_TileWidth, p.width_mbs * 16 - 1);
      c.emit(w);
      break;
    }
  }
  {
    Command<kVdPipelineFlushDwords> c(kVdPipelineFlushDefault);
    c.set(kVdPipelineFlush_MfxPipelineDone, p.last_in_stream ? 0 : 1);
    c.set(kVdPipelineFlush_VdencPipelineDone, 1);
    c.set(kVdPipelineFlush_VdencPipelineCommandFlush, 1);
    c.set(kVdPipelineFlush_VdCommandMessageParserDone, 1);
    c.emit(w);
  }
  return w.ok();
}

void emit_avc_vdenc_finish(DwordWriter& w, const AvcEncodeBuffers& b, EncodeGeneration generation) {
  intel_mfx::emit_mi_flush(w);
  if (b.status) {
    // MI_STORE_REGISTER_MEM (48-bit address): the number of bytes the PAK wrote for the frame.
    w.emit((0x24u << 23) | 2u);
    w.emit(bitstream_byte_count_register(generation));
    w.emit(b.status);
    w.emit(0);
  }
  intel_mfx::emit_batch_end(w);
}

}  // namespace intel_enc
}  // namespace hardware
