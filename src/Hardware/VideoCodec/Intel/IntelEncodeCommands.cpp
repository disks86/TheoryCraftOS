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

namespace hardware {
namespace intel_enc {

namespace {

struct EncodeField {
  uint8_t dw, lo, hi;
};

#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImages.inc"
#include "Hardware/VideoCodec/Intel/IntelVdencTables.inc"

// A command being filled in: the media driver's defaults, then fields by name.
template <uint32_t N>
class Command {
 public:
  explicit Command(const uint32_t (&defaults)[N]) {
    for (uint32_t i = 0; i < N; ++i) d_[i] = defaults[i];
  }
  void set(EncodeField f, uint32_t value) {
    const uint32_t width = f.hi - f.lo + 1u;
    const uint32_t mask = (width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u)) << f.lo;
    d_[f.dw] = (d_[f.dw] & ~mask) | ((value << f.lo) & mask);
  }
  void raw(uint32_t dw, uint32_t value) { d_[dw] = value; }
  uint32_t get(uint32_t dw) const { return d_[dw]; }
  // A buffer's address (low dword; the high dword stays 0 below 4 GiB) and its memory attributes dword.
  void buffer(uint32_t dw, uint32_t address, uint32_t attributes) {
    d_[dw] = address;
    d_[dw + 1] = 0;
    d_[dw + 2] = (d_[dw + 2] & ~0x7Fu) | attributes;
  }
  void emit(DwordWriter& w) const {
    for (uint32_t i = 0; i < N; ++i) w.emit(d_[i]);
  }

 private:
  uint32_t d_[N];
};

constexpr uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

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

// The three VDEnc surface state commands share their body (VDENC_Surface_State_Fields: dwords 2..5 of the command).
template <uint32_t N>
void vdenc_surface(DwordWriter& w, const uint32_t (&defaults)[N], uint32_t width, uint32_t height, const Nv12TiledLayout& layout) {
  Command<N> c(defaults);
  c.raw(2, ((height - 1) << 18) | ((width - 1) << 4));                 // width, height (dword 0 of the body)
  c.raw(3, 1u | (1u << 1) | ((layout.pitch - 1) << 3) | (1u << 27) |   // Y-major tiled, pitch, interleaved chroma
               (intel_mfx::kSurfacePlanar420 << 28));
  c.raw(4, layout.chroma_offset_rows);                                 // Y offset of the Cb plane
  c.raw(5, layout.chroma_offset_rows);                                 // ... of the Cr plane (the same: interleaved)
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

void emit_avc_vdenc_picture(DwordWriter& w, const AvcEncodePicture& p, const AvcEncodeBuffers& b, const AvcEncodeLayout& layout) {
  const uint32_t mbs = p.width_mbs * p.height_mbs;
  const uint32_t qp = static_cast<uint32_t>(p.qp < 0 ? 0 : p.qp > static_cast<int32_t>(kMaxQp) ? kMaxQp : p.qp);

  // --- MFX_PIPE_MODE_SELECT: encode, VDEnc, the picture leaves through the post-deblocking path when the filter runs.
  {
    Command<kMfxPipeModeSelectDwords> c(kMfxPipeModeSelectDefault);
    c.set(kMfxPipeModeSelect_StandardSelect, kMfxPipeModeSelect_STANDARD_SELECT_AVC);
    c.set(kMfxPipeModeSelect_CodecSelect, kMfxPipeModeSelect_CODEC_SELECT_ENCODE);
    c.set(kMfxPipeModeSelect_DecoderShortFormatMode, 1);   // (the encoder's "long format": the engine does not move the next slice's position)
    c.set(kMfxPipeModeSelect_VdencMode, 1);
    c.set(kMfxPipeModeSelect_ScaledSurfaceEnable, 1);
    c.set(kMfxPipeModeSelect_StreamOutEnable, 0);
    c.set(kMfxPipeModeSelect_FrameStatisticsStreamoutEnable, 1);
    c.set(kMfxPipeModeSelect_PreDeblockingOutputEnablePredeblockoutenable, p.deblocking ? 0 : 1);
    c.set(kMfxPipeModeSelect_PostDeblockingOutputEnablePostdeblockoutenable, p.deblocking ? 1 : 0);
    c.emit(w);
  }
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
    c.raw(51, (c.get(51) & ~0x7Fu) | kMocs);
    if (b.recon_ds4) {
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
  {
    Command<kVdencPipeModeSelectDwords> c(kVdencPipeModeSelectDefault);
    c.set(kVdencPipeModeSelect_StandardSelect, kVdencPipeModeSelect_STANDARD_SELECT_AVC);
    c.set(kVdencPipeModeSelect_FrameStatisticsStreamOutEnable, 1);
    c.set(kVdencPipeModeSelect_TlbPrefetchEnable, 0);
    c.set(kVdencPipeModeSelect_PakThresholdCheckEnable, 0);
    c.set(kVdencPipeModeSelect_VdencStreamInEnable, 0);
    c.emit(w);
  }
  vdenc_surface(w, kVdencSrcSurfaceStateDefault, layout.width, layout.height, layout.full);
  vdenc_surface(w, kVdencRefSurfaceStateDefault, layout.width, layout.height, layout.full);
  vdenc_surface(w, kVdencDsRefSurfaceStateDefault, layout.quarter_width, layout.quarter_height, layout.quarter);
  {
    Command<kVdencPipeBufAddrStateDwords> c(kVdencPipeBufAddrStateDefault);
    // Dword layout: three dwords (low address, high address, attributes) per buffer. DS refs 1, 4, 7; original picture 10; stream-in 13; row store 16; co-located MV 19;
    // forward references 22, 25, 28; backward reference 31; statistics stream-out 34.
    if (p.p_picture) {
      c.buffer(1, b.reference_ds4, 0);
      c.buffer(22, b.reference, kMocs);
    }
    c.buffer(10, b.source, kMocs);
    if (b.vdenc_row_store) c.buffer(16, b.vdenc_row_store, kMocs);
    if (b.vdenc_statistics) c.buffer(34, b.vdenc_statistics, kMocs);
    c.emit(w);
  }
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

  // --- The reference picture list of a P slice (one entry: the previous picture, a short-term frame).
  if (p.p_picture) {
    Command<kMfxAvcRefIdxStateDwords> c(kMfxAvcRefIdxStateDefault);
    c.set(kMfxAvcRefIdxState_RefpiclistSelect, 0);
    // 32 one-byte entries from dword 2: frame store id in bits 1..4, bit 5 = frame (not a field), bit 6 = long term, bit 7 = not present.
    uint32_t entries[8] = {0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u};
    entries[0] = (entries[0] & ~0xFFu) | intel_mfx::avc_ref_idx_entry(p.reference_frame_store, false);
    for (uint32_t i = 0; i < 8; ++i) c.raw(2 + i, entries[i]);
    c.emit(w);
  }

  // --- MFX_AVC_SLICE_STATE: the whole picture is one slice.
  {
    Command<kMfxAvcSliceStateDwords> c(kMfxAvcSliceStateDefault);
    c.set(kMfxAvcSliceState_SliceType, p.p_picture ? kMfxAvcSliceState_SLICE_TYPE_PSLICE : kMfxAvcSliceState_SLICE_TYPE_ISLICE);
    c.set(kMfxAvcSliceState_NumberOfReferencePicturesInInterPredictionList0, p.p_picture ? 1 : 0);
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
    c.set(kMfxAvcSliceState_Roundinter, p.p_picture ? kAdaptiveInterRoundingPWithoutB[qp] : 2);
    c.set(kMfxAvcSliceState_Roundinterenable, 0);
    c.set(kMfxAvcSliceState_Roundintra, 5);
    c.set(kMfxAvcSliceState_Roundintraenable, 1);
    c.emit(w);
  }

  // --- The headers go in front of the slice data the engine writes.
  for (uint64_t i = 0; i < headers.size(); ++i) pak_insert(w, headers[i], i + 1 == headers.size(), headers[i].slice_header);

  // --- VDENC_WALKER_STATE: start at the top, the next slice starts below the picture; then wait for the VDEnc and MFX pipes.
  {
    Command<kVdencWalkerStateDwords> c(kVdencWalkerStateDefault);
    c.set(kVdencWalkerState_MbLcuStartYPosition, 0);
    c.set(kVdencWalkerState_MbLcuStartXPosition, 0);
    c.set(kVdencWalkerState_NextsliceMbStartYPosition, p.height_mbs);
    c.emit(w);
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

void emit_avc_vdenc_finish(DwordWriter& w, const AvcEncodeBuffers& b) {
  intel_mfx::emit_mi_flush(w);
  if (b.status) {
    // MI_STORE_REGISTER_MEM (48-bit address): the number of bytes the PAK wrote for the frame.
    w.emit((0x24u << 23) | 2u);
    w.emit(kBitstreamByteCountRegister);
    w.emit(b.status);
    w.emit(0);
  }
  intel_mfx::emit_batch_end(w);
}

}  // namespace intel_enc
}  // namespace hardware
