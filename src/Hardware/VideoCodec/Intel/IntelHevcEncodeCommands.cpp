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


#include "Hardware/VideoCodec/Intel/IntelHevcEncodeCommands.hpp"
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandBuilder.hpp"
#include "Hardware/VideoCodec/Intel/IntelHevcHeaders.hpp"

namespace hardware {
namespace intel_enc {

namespace {

using namespace build;

#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImages.inc"
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImagesGen11.inc"
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImagesGen12.inc"
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImagesGen12Hevc.inc"
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImagesXeLpgHevc.inc"
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImagesXe2LpmHevc.inc"   // (Lunar Lake: CMD1 and CMD2 differ from Xe-LPG's)
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImagesXe2HpmHevc.inc"   // (Battlemage: also the pipe mode select)
#include "Hardware/VideoCodec/Intel/IntelEncodeCommandImagesXeHpgHevc.inc"   // (only its CMD2 differs from Xe-LPG's; tools/gen_intel_hevc_xelpg.py xehpg)

constexpr uint32_t kPage = 4096;
constexpr uint32_t kCacheLine = 64;
constexpr uint32_t kMaxPakInsertDwords = 250;   // (the validator reads a command's length from eight bits)

// The VDEnc surface state commands share their body (dwords 2..5 of the command; the second surface of the DS reference command has its own at dwords 6..9). Xe-LP and later
// have a five-bit surface format from bit 27 of the second dword (NV12 is 4), tiled with the Y-major walk; `chroma_filter` is the chroma downsample filter control (bits 20 .. 22)
// the Xe-LPG driver sets to 7 on the source surface.
void vdenc_surface_body(uint32_t (&d)[10], uint32_t at, uint32_t width, uint32_t height, const Nv12TiledLayout& layout, uint32_t chroma_filter) {
  d[at] = ((height - 1) << 18) | ((width - 1) << 4);
  d[at + 1] = 1u | (1u << 1) | ((layout.pitch - 1) << 3) | (chroma_filter << 20) | (intel_mfx::kSurfacePlanar420 << 27);
  d[at + 2] = layout.chroma_offset_rows;
  d[at + 3] = layout.chroma_offset_rows;
}

template <uint32_t N>
void vdenc_surface(DwordWriter& w, const uint32_t (&defaults)[N], uint32_t width, uint32_t height, const Nv12TiledLayout& layout, uint32_t chroma_filter) {
  uint32_t d[10] = {};
  for (uint32_t i = 0; i < N; ++i) d[i] = defaults[i];
  vdenc_surface_body(d, 2, width, height, layout, chroma_filter);
  for (uint32_t i = 0; i < N; ++i) w.emit(d[i]);
}

// VDENC_DS_REF_SURFACE_STATE of Xe-LP: two surfaces, the 8x down-scaled picture and the 4x one.
void vdenc_ds_surfaces(DwordWriter& w, const HevcEncodeLayout& l) {
  uint32_t d[10];
  for (uint32_t i = 0; i < 10; ++i) d[i] = kGen11VdencDsRefSurfaceStateDefault[i];
  vdenc_surface_body(d, 2, l.ds8_width, l.ds8_height, l.ds8, 0);
  vdenc_surface_body(d, 6, l.ds4_width, l.ds4_height, l.ds4, 0);
  for (uint32_t i = 0; i < 10; ++i) w.emit(d[i]);
}

// VDENC_PIPE_BUF_ADDR_STATE. The 89-dword command of Xe-LPG keeps the first 71 dwords where Xe-LP's has them (the rest is for AVC, VP9 and AV1 and stays as the image has it).
template <uint32_t N>
void vdenc_pipe_buf(DwordWriter& w, const uint32_t (&image)[N], const HevcEncodePicture& p, const HevcEncodeBuffers& b) {
  Command<N> c(image);
  if (p.p_picture) {
    c.buffer(1, b.reference_ds8, kMocs);
    c.buffer(37, b.reference_ds4, kMocs);
    c.buffer(22, b.reference, kMocs);
  }
  c.buffer(10, b.source, kMocs);
  c.buffer(16, b.vdenc_row_store, kMocs);
  c.buffer(34, b.vdenc_statistics, kMocs);
  c.buffer(46, b.pak_objects, kMocs);
  c.buffer(49, b.recon_ds8, kMocs);
  c.buffer(52, b.recon_ds4, kMocs);
  c.raw(61, 3 * kCacheLine);   // the weights histogram follows the first three cache lines of the statistics
  c.buffer(62, b.vdenc_tile_row_store, kMocs);
  c.buffer(65, b.cu_count, kMocs);
  c.emit(w);
}

void hcp_surface(DwordWriter& w, uint32_t id, const HevcEncodeLayout& l) {
  Command<kGen12HcpSurfaceStateDwords> c(kGen12HcpSurfaceStateDefault);
  c.set(kGen12HcpSurfaceState_SurfaceId, id);
  c.set(kGen12HcpSurfaceState_SurfacePitchMinus1, l.full.pitch - 1);
  c.set(kGen12HcpSurfaceState_YOffsetForUCbInPixel, l.full.chroma_offset_rows);
  c.set(kGen12HcpSurfaceState_YOffsetForVCr, l.full.chroma_offset_rows);
  c.set(kGen12HcpSurfaceState_SurfaceFormat, kGen12HcpSurfaceState_SURFACE_FORMAT_PLANAR4208);
  c.emit(w);
}

// HCP_PIPE_BUF_ADDR_STATE. Dword layout: three dwords (low address, high address, attributes) per buffer for the single ones, two for the eight reference pictures (37 .. 52, one
// attributes dword at 53) and the eight co-located motion vector buffers (66 .. 81, attributes at 82).
void hcp_pipe_buf(DwordWriter& w, const HevcEncodePicture& p, const HevcEncodeBuffers& b) {
  Command<kGen12HcpPipeBufAddrStateDwords> c(kGen12HcpPipeBufAddrStateDefault);
  c.buffer(1, b.recon, kMocs);
  c.buffer(4, b.deblocking_line, kMocs);
  c.buffer(7, b.deblocking_tile_line, kMocs);
  c.buffer(10, b.deblocking_tile_column, kMocs);
  c.buffer(13, b.metadata_line, kMocs);
  c.buffer(16, b.metadata_tile_line, kMocs);
  c.buffer(19, b.metadata_tile_column, kMocs);
  c.buffer(22, b.sao_line, kMocs);
  c.buffer(25, b.sao_tile_line, kMocs);
  c.buffer(28, b.sao_tile_column, kMocs);
  c.buffer(31, b.mv_temporal, kMocs);
  if (p.p_picture) {
    c.raw(37, b.reference);   // frame store 0
    c.raw(38, 0);
    c.raw(66, b.reference_mv_temporal);
    c.raw(67, 0);
  }
  c.raw(53, (c.get(53) & ~0x7Fu) | kMocs);
  c.raw(82, (c.get(82) & ~0x7Fu) | kMocs);
  c.buffer(54, b.source, kMocs);
  c.buffer(57, b.cu_streamout, kMocs);
  c.buffer(60, b.lcu_base, kMocs);
  c.buffer(63, b.lcu_ildb, kMocs);
  c.buffer(95, b.sao_row_store, kMocs);
  c.buffer(98, b.frame_statistics, kMocs);
  c.buffer(101, b.sse_row_store, kMocs);
  c.emit(w);
}

// HCP_IND_OBJ_BASE_ADDR_STATE: the coding unit records (behind the LCU commands in the PAK object buffer) and the bitstream the PAK writes with its upper bound.
void hcp_ind_obj(DwordWriter& w, const HevcEncodeBuffers& b, const HevcScratchPlan& plan) {
  Command<kGen12HcpIndObjBaseAddrStateDwords> c(kGen12HcpIndObjBaseAddrStateDefault);
  c.buffer(6, b.pak_objects + plan.pak_object_cu_offset, kMocs);
  c.buffer(9, b.bitstream, kMocs);
  c.raw(12, b.bitstream + align_up(b.bitstream_bytes, kPage));
  c.raw(13, 0);
  c.emit(w);
}

// Flat quantiser matrices: the 16-bit reciprocal of every weight 16 (HCP_FQM_STATE, sent first) and the weights (HCP_QM_STATE).
void hcp_flat_matrices(DwordWriter& w) {
  constexpr uint32_t kUsedBytes[4] = {16, 64, 64, 64};
  for (uint32_t size = 0; size < 4; ++size) {
    for (uint32_t intra_inter = 0; intra_inter < 2; ++intra_inter) {
      Command<kGen12HcpFqmStateDwords> c(kGen12HcpFqmStateDefault);
      c.set(kGen12HcpFqmState_IntraInter, intra_inter);
      c.set(kGen12HcpFqmState_Sizeid, size);
      c.set(kGen12HcpFqmState_ColorComponent, 0);
      c.set(kGen12HcpFqmState_FqmDcValue1Dc, size >= 2 ? 0x1000 : 0);
      for (uint32_t i = 0; i < 32; ++i) c.raw(2 + i, i * 4 < kUsedBytes[size] ? 0x10001000u : 0u);
      c.emit(w);
    }
  }
  for (uint32_t size = 0; size < 4; ++size) {
    for (uint32_t pred = 0; pred < 2; ++pred) {
      for (uint32_t color = 0; color < (size == 3 ? 1u : 3u); ++color) {
        Command<kGen12HcpQmStateDwords> c(kGen12HcpQmStateDefault);
        c.set(kGen12HcpQmState_PredictionType, pred);
        c.set(kGen12HcpQmState_Sizeid, size);
        c.set(kGen12HcpQmState_ColorComponent, color);
        c.set(kGen12HcpQmState_DcCoefficient, size >= 2 ? 16 : 0);
        for (uint32_t i = 0; i < 16; ++i) c.raw(2 + i, i * 4 < kUsedBytes[size] ? 0x10101010u : 0u);
        c.emit(w);
      }
    }
  }
}

int32_t clip(int32_t lo, int32_t hi, int32_t v) { return v < lo ? lo : v > hi ? hi : v; }

// VDENC_CMD1: constant tables for the picture type (the media driver's AddVdencCmd1Cmd, HEVC branch, without visual quality improvement or ROI).
void vdenc_cmd1(DwordWriter& w, const HevcEncodePicture& p) {
  Command<kGen12VdencCmd1Dwords> c(kGen12VdencCmd1Default);
  for (uint32_t i = 22; i <= 29; ++i) c.raw(i, 0x10101010);
  c.raw(1, 0x5030200);
  c.raw(2, 0xb090806);
  c.raw(3, 0x1c140c04);
  c.raw(4, 0x3c342c24);
  c.raw(5, 0x5c544c44);
  c.raw(6, 0x1c140c04);
  c.raw(7, 0x3c342c24);
  c.raw(8, 0x5c544c44);
  c.raw(14, 0);
  c.raw(15, 0);
  c.raw(16, c.get(16) & 0xffff0000u);
  c.raw(19, (c.get(19) & 0xff0000ffu) | 0x140400);
  c.raw(20, 0x14141414);
  c.raw(21, 0x14141414);
  if (!p.p_picture) {
    c.raw(10, 0x23131f0f);
    c.raw(11, (c.get(11) & 0xffff0000u) | 0x2313);
    c.raw(12, 0x3e5c445c);
    c.raw(13, (c.get(13) & 0xff00u) | 0x1e040044);
    c.raw(16, (c.get(16) & 0xffffu) | 0x70000);
    c.raw(17, 0xd0e1007);
    c.raw(18, (c.get(18) & 0xffffff00u) | 0x32);
    c.raw(18, (c.get(18) & 0xffff00ffu) | 0x1e00);
    c.raw(30, (c.get(30) & 0xff000000u) | 0x101010);
  } else {   // a B picture
    c.raw(10, 0x23131f0f);
    c.raw(11, 0x331b2313);
    c.raw(12, 0x476e4d6e);
    c.raw(13, 0x3604004d);
    c.raw(16, (c.get(16) & 0xffffu) | 0x4150000);
    c.raw(17, 0x23231415);
    c.raw(18, (c.get(18) & 0xffffff00u) | 0x3f);
    c.raw(18, (c.get(18) & 0xffff00ffu) | 0x4400);
    c.raw(30, (c.get(30) & 0xff000000u) | 0x232323);
  }
  c.emit(w);
}

// VDENC_CMD1 and VDENC_CMD2 of Xe-LPG: the images the media driver's own parameter code produces for an I picture and a low-delay B picture (constant QP, target usage 7), with
// the one dword each that depends on the quantiser (lambda and its square root; QpPrimeYAc) and the picture's size and picture order counts.
uint32_t xe_lpg_qp_index(const HevcEncodePicture& p) { return static_cast<uint32_t>(clip(10, 51, p.qp)) - 10; }

template <uint32_t N>
void xe_cmd1_image(DwordWriter& w, const HevcEncodePicture& p, const uint32_t (&image_i)[N], const uint32_t (&image_b)[N]) {
  Command<N> c(p.p_picture ? image_b : image_i);
  c.raw(kXeLpgVdencCmd1QpDword, (p.p_picture ? kXeLpgVdencCmd1QpTableB : kXeLpgVdencCmd1QpTableI)[xe_lpg_qp_index(p)]);   // (dword 22 on every platform)
  c.emit(w);
}

void xe_lpg_cmd1(DwordWriter& w, const HevcEncodePicture& p) {
  switch (p.platform) {
    case HevcVdencPlatform::Xe2Lpm: xe_cmd1_image(w, p, kXe2LpmVdencCmd1ImageI, kXe2LpmVdencCmd1ImageB); break;
    case HevcVdencPlatform::Xe2Hpm: xe_cmd1_image(w, p, kXe2HpmVdencCmd1ImageI, kXe2HpmVdencCmd1ImageB); break;
    default: xe_cmd1_image(w, p, kXeLpgVdencCmd1ImageI, kXeLpgVdencCmd1ImageB); break;   // (Arc's is Xe-LPG's)
  }
}

void xe_lpg_cmd2(DwordWriter& w, const HevcEncodePicture& p, const HevcEncodeLayout& l) {
  const uint32_t* image;
  switch (p.platform) {
    case HevcVdencPlatform::XeHpg: image = p.p_picture ? kXeHpgVdencCmd2ImageB : kXeHpgVdencCmd2ImageI; break;
    case HevcVdencPlatform::Xe2Lpm: image = p.p_picture ? kXe2LpmVdencCmd2ImageB : kXe2LpmVdencCmd2ImageI; break;
    case HevcVdencPlatform::Xe2Hpm: image = p.p_picture ? kXe2HpmVdencCmd2ImageB : kXe2HpmVdencCmd2ImageI; break;
    default: image = p.p_picture ? kXeLpgVdencCmd2ImageB : kXeLpgVdencCmd2ImageI; break;
  }
  static_assert(kXeLpgVdencCmd2Dwords == kXe2LpmVdencCmd2Dwords && kXeLpgVdencCmd2Dwords == kXe2HpmVdencCmd2Dwords, "CMD2 is 62 dwords on every platform");
  uint32_t copy[kXeLpgVdencCmd2Dwords];
  for (uint32_t i = 0; i < kXeLpgVdencCmd2Dwords; ++i) copy[i] = image[i];
  Command<kXeLpgVdencCmd2Dwords> c(copy);
  c.raw(kXeLpgVdencCmd2QpDword, (p.p_picture ? kXeLpgVdencCmd2QpTableB : kXeLpgVdencCmd2QpTableI)[xe_lpg_qp_index(p)]);
  c.set(kXeLpgVdencCmd2_FrameWidthInPixelsMinusOne, l.width - 1);
  c.set(kXeLpgVdencCmd2_FrameHeightInPixelsMinusOne, l.height - 1);
  if (p.p_picture) {
    // As on Xe-LP: the distance to the picture the list entry names; the entries the lists do not have name "frame 0" of the POC list, so they get this picture's own count.
    const int32_t distance = clip(-16, 16, static_cast<int32_t>(p.poc) - static_cast<int32_t>(p.reference_poc));
    const int32_t unused = clip(-16, 16, static_cast<int32_t>(p.poc));
    c.set(kXeLpgVdencCmd2_FwdPocNumberForRefid0InL0, static_cast<uint32_t>(distance) & 0xFF);
    c.set(kXeLpgVdencCmd2_BwdPocNumberForRefid0InL1, static_cast<uint32_t>(distance) & 0xFF);
    c.set(kXeLpgVdencCmd2_PocNumberForRefid1InL0, static_cast<uint32_t>(unused) & 0xFF);
    c.set(kXeLpgVdencCmd2_PocNumberForRefid1InL1, static_cast<uint32_t>(unused) & 0xFF);
    c.set(kXeLpgVdencCmd2_PocNumberForRefid2InL0, static_cast<uint32_t>(unused) & 0xFF);
    c.set(kXeLpgVdencCmd2_PocNumberForRefid2InL1, static_cast<uint32_t>(unused) & 0xFF);
  }
  c.emit(w);
}

// The lambda table of CMD2 DW26 by picture type (I, P, B) and quantiser 10 .. 51: the driver's table, from which only the I and B rows are used here (its P and B rows are equal).
const uint32_t kLambdaI[42] = {0x30002, 0x30002, 0x30002, 0x30003, 0x40004, 0x40005, 0x50006, 0x60008, 0x6000a, 0x7000c, 0x8000f, 0x90013, 0xa0018, 0xb001e, 0xc0026, 0xe0030,
                               0x10003d, 0x12004d, 0x140061, 0x16007a, 0x19009a, 0x1c00c2, 0x1f00f4, 0x230133, 0x270183, 0x2c01e8, 0x320266, 0x380306, 0x3e03cf, 0x4604cd,
                               0x4f060c, 0x58079f, 0x63099a, 0x6f0c18, 0x7d0f3d, 0x8c1333, 0x9d1831, 0xb11e7a, 0xc62666, 0xdf3062, 0xfa3cf5, 0x1184ccd};
const uint32_t kLambdaB[42] = {0x30003, 0x30003, 0x30003, 0x40003, 0x40004, 0x50005, 0x50007, 0x60008, 0x6000a, 0x7000d, 0x80011, 0x90015, 0xa001a, 0xb0021, 0xd002a, 0xe0034,
                               0x100042, 0x120053, 0x140069, 0x170084, 0x1a00a6, 0x1d00d2, 0x210108, 0x24014d, 0x2901a3, 0x2e0210, 0x34029a, 0x3a0347, 0x410421, 0x490533,
                               0x52068d, 0x5c0841, 0x670a66, 0x740d1a, 0x821082, 0x9214cd, 0xa41a35, 0xb82105, 0xce299a, 0xe8346a, 0x1044209, 0x1245333};

// VDENC_CMD2: the picture-level VDEnc parameters (AddVdencCmd2Cmd, HEVC branch) for target usage 7 (speed), one reference, low-delay B, rounding offsets on, no ROI, no stream-in.
void vdenc_cmd2(DwordWriter& w, const HevcEncodePicture& p, const HevcEncodeLayout& l) {
  Command<kGen12VdencCmd2Dwords> c(kGen12VdencCmd2Default);
  const uint32_t qp = static_cast<uint32_t>(clip(10, 51, p.qp));
  c.raw(2, (c.get(2) & 0xdff00000u) | 0x2005aff3);
  c.raw(3, 0xfe02ff01);
  c.raw(4, 0xfc04fd03);
  c.raw(5, (c.get(5) & 0xff7e03ffu) | 0x80ac00);
  c.raw(7, (c.get(7) & 0xffd90ff0u) | 0x62003);
  c.raw(9, (c.get(9) & 0xffffu) | 0x43840000);
  c.raw(12, 0xffffffffu);
  c.raw(15, 0x4e201f40);
  c.raw(16, (c.get(16) & 0xf0ff0000u) | 0xf003300);
  c.raw(17, (c.get(17) & 0xfff00000u) | 0x2710);
  c.raw(19, (c.get(19) & 0x80ffffffu) | 0x18000000);
  c.raw(21, c.get(21) & 0xfffffffu);
  c.raw(22, 0x1f001102);
  c.raw(23, 0xaaaa1f00u);
  c.raw(27, (c.get(27) & 0xffff0000u) | 0x1a1a);

  c.set(kGen12VdencCmd2_FrameWidthInPixelsMinusOne, l.width - 1);
  c.set(kGen12VdencCmd2_FrameHeightInPixelsMinusOne, l.height - 1);
  c.set(kGen12VdencCmd2_PictureType, p.p_picture ? 3 : 0);   // low-delay B or I
  c.set(kGen12VdencCmd2_TemporalMvpEnableFlag, 0);
  c.set(kGen12VdencCmd2_TransformSkip, 0);
  c.set(kGen12VdencCmd2_NumRefIdxL0Minus1, 0);
  c.set(kGen12VdencCmd2_NumRefIdxL1Minus1, 0);
  c.raw(5, (c.get(5) & 0xff83ffffu) | 0x400000);
  c.raw(14, (c.get(14) & 0xffffu) | 0x7d00000);
  c.raw(15, 0x4e201f40);
  c.raw(17, (c.get(17) & 0xfff00000u) | 0x2710);
  c.raw(18, (c.get(18) & 0xffffu) | 0x600000);
  c.raw(19, (c.get(19) & 0xffff0000u) | 0xc0);
  c.raw(20, c.get(20) & 0xfffeffffu);
  c.set(kGen12VdencCmd2_TilingEnable, 0);
  c.set(kGen12VdencCmd2_TileReplayEnable, 0);

  if (p.p_picture) {
    // The driver writes the (clipped) distance to the picture each list entry names; entries the list does not have name "frame 0" of the reference POC list (POC 0 as far as
    // this call is concerned), which it also writes.
    const int32_t distance = clip(-16, 16, static_cast<int32_t>(p.poc) - static_cast<int32_t>(p.reference_poc));
    const int32_t unused = clip(-16, 16, static_cast<int32_t>(p.poc));
    c.set(kGen12VdencCmd2_PocNumberForRefid0InL0, static_cast<uint32_t>(distance) & 0xFF);
    c.set(kGen12VdencCmd2_PocNumberForRefid1InL0, static_cast<uint32_t>(unused) & 0xFF);
    c.set(kGen12VdencCmd2_PocNumberForRefid2InL0, static_cast<uint32_t>(unused) & 0xFF);
    c.set(kGen12VdencCmd2_PocNumberForRefid0InL1, static_cast<uint32_t>(distance) & 0xFF);
    c.set(kGen12VdencCmd2_PocNumberForRefid1InL1, static_cast<uint32_t>(unused) & 0xFF);
    c.set(kGen12VdencCmd2_PocNumberForRefid2InL1, static_cast<uint32_t>(unused) & 0xFF);
    c.set(kGen12VdencCmd2_SubPelMode, 3);
  } else {
    c.raw(3, 0);
    c.raw(4, 0);
  }

  // Target usage 7.
  c.raw(2, (c.get(2) & 0xdfffffffu) | 0x20000000);
  c.raw(7, (c.get(7) & 0xfff7feffu) | 0x80100);
  c.raw(9, (c.get(9) & 0xffffu) | 0x22420000);
  c.raw(12, 0x89800dc0u);
  c.raw(34, (c.get(34) & 0xffffffu) | 0x20000000);
  c.raw(7, (c.get(7) & 0xfff7ffffu) | 0x80000);   // one reference
  c.set(kGen12VdencCmd2_VdencStreamInEnable, 0);
  c.set(kGen12VdencCmd2_PakOnlyMultiPassEnable, 0);
  c.raw(8, 0);                                    // low delay: no backward-reference parameters
  c.raw(9, c.get(9) & 0xffff0000u);
  c.set(kGen12VdencCmd2_MinQp, 10);
  c.set(kGen12VdencCmd2_MaxQp, 51);
  c.set(kGen12VdencCmd2_TemporalMVEnableForIntegerSearch, 0);

  c.raw(26, (c.get(26) & 0xfe000000u) | (p.p_picture ? kLambdaB : kLambdaI)[qp - 10]);
  c.raw(27, ((c.get(27) & ~0xff00u) | (qp << 8)) & 0xffffff00u);   // QpPrimeYAc, with the DC quantiser cleared as the driver does
  c.raw(35, (c.get(35) & 0xfffff0ffu) | 0x700);

  // Rounding offsets for I and low-delay B pictures: inter 4, intra 10.
  c.raw(28, 0x7d00fa0);
  c.raw(29, 0x2bc0bb8);
  c.raw(30, 0x32003e8);
  c.raw(31, 0x1f4012c);
  c.raw(32, (c.get(32) & 0xffff0000u) | 0x190);
  c.raw(32, (c.get(32) & 0xffffu) | 0xaa440000u);
  c.raw(33, 0x44aa4444);
  c.raw(34, (c.get(34) & 0xff000000u) | 0x44aa44);

  // Reference frame store of list entry 0 (store 0), the unused entries (7), and the flag in the top bit.
  c.raw(11, 0x00070700u);
  c.raw(11, (c.get(11) & 0x7fffffffu) | 0x80000000u);
  c.emit(w);
}

void pak_insert(DwordWriter& w, const HevcHeaderInsert& h, bool last_header) {
  const uint32_t bytes = (h.bits + 7) / 8;
  const uint32_t data_dwords = (bytes + 3) / 4;
  Command<kGen12HcpPakInsertObjectDwords> c(kGen12HcpPakInsertObjectDefault);
  c.raw(0, kGen12HcpPakInsertObjectDefault[0] | (kGen12HcpPakInsertObjectDwords + data_dwords - 2));
  uint32_t bits_in_last = h.bits % 32;
  if (bits_in_last == 0) bits_in_last = 32;
  c.set(kGen12HcpPakInsertObject_EndofsliceflagLastdstdatainsertcommandflag, 0);
  c.set(kGen12HcpPakInsertObject_LastheaderflagLastsrcheaderdatainsertcommandflag, last_header ? 1 : 0);
  c.set(kGen12HcpPakInsertObject_EmulationflagEmulationbytebitsinsertenable, h.emulation_prevention ? 1 : 0);
  c.set(kGen12HcpPakInsertObject_SkipemulbytecntSkipEmulationByteCount, h.skip_emulation_bytes);
  c.set(kGen12HcpPakInsertObject_DatabitsinlastdwSrcdataendingbitinclusion50, bits_in_last);
  c.set(kGen12HcpPakInsertObject_Headerlengthexcludefrmsize, 0);
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

// HCP_REF_IDX_STATE for one list: one entry, the previous picture (frame store 0).
void hcp_ref_idx(DwordWriter& w, uint32_t list, int32_t poc_distance) {
  Command<kGen12HcpRefIdxStateDwords> c(kGen12HcpRefIdxStateDefault);
  c.set(kGen12HcpRefIdxState_Refpiclistnum, list);
  c.set(kGen12HcpRefIdxState_NumRefIdxLRefpiclistnumActiveMinus1, 0);
  // list entry: picture order count distance (8 bits), frame store (bits 8 .. 10), the long-term/field flags; the driver also sets the bottom-field flag of frame pictures.
  c.raw(2, (static_cast<uint32_t>(clip(-128, 127, poc_distance)) & 0xFF) | (0u << 8) | (1u << 15));
  c.emit(w);
}

}  // namespace

bool hevc_encode_layout(uint32_t coded_width, uint32_t coded_height, HevcEncodeLayout* out) {
  if (coded_width < 16 || coded_height < 16 || coded_width > 4096 || coded_height > 4096 || (coded_width & 7) || (coded_height & 7)) return false;
  out->width = coded_width;
  out->height = coded_height;
  out->aligned_width = align_up(coded_width, 64);
  out->aligned_height = align_up(coded_height, 64);
  out->ctbs_x = out->aligned_width / 64;
  out->ctbs_y = out->aligned_height / 64;
  out->ds4_width = out->aligned_width / 4;
  out->ds4_height = out->aligned_height / 4;
  out->ds8_width = out->aligned_width / 8;
  out->ds8_height = out->aligned_height / 8;
  return intel_mfx::nv12_tiled_layout(out->aligned_width, out->aligned_height, &out->full) && intel_mfx::nv12_tiled_layout(out->ds4_width, out->ds4_height, &out->ds4) &&
         intel_mfx::nv12_tiled_layout(out->ds8_width, out->ds8_height, &out->ds8);
}

HevcScratchPlan hevc_scratch_plan(const HevcEncodeLayout& l) {
  HevcScratchPlan p;
  const uint32_t w16 = l.aligned_width / 16, h16 = l.aligned_height / 16;
  const uint32_t tile_columns = (l.width + 127) / 128;
  uint32_t at = 0;
  auto take = [&at](uint32_t* field, uint64_t bytes) {
    *field = at;
    at += align_up(static_cast<uint32_t>(bytes), kPage);
  };
  take(&p.deblocking_line, uint64_t{8} * kCacheLine * w16);
  take(&p.deblocking_tile_line, uint64_t{16} * kCacheLine * w16);
  take(&p.deblocking_tile_column, uint64_t{18} * kCacheLine * h16);
  take(&p.metadata_line, uint64_t{2} * kCacheLine * w16);
  take(&p.metadata_tile_line, uint64_t{4} * kCacheLine * w16);
  take(&p.metadata_tile_column, uint64_t{4} * kCacheLine * h16);
  take(&p.sao_line, uint64_t{5} * kCacheLine * w16);
  take(&p.sao_tile_line, uint64_t{10} * kCacheLine * w16);
  take(&p.sao_tile_column, uint64_t{18} * kCacheLine * h16);
  take(&p.sao_row_store, uint64_t{align_up(w16 + 3 * tile_columns, 4)} * 16);
  take(&p.lcu_ildb, kCacheLine);
  take(&p.lcu_base, uint64_t{w16} * h16 * kCacheLine);
  take(&p.frame_statistics, 9 * kCacheLine);
  take(&p.sse_row_store, uint64_t{2048} * (l.aligned_width + 3 * tile_columns));
  take(&p.cu_streamout, uint64_t{align_up((l.width / 8) * (l.height / 8) * 16, kCacheLine)});
  take(&p.vdenc_row_store, uint64_t{l.ctbs_x} * kCacheLine * 4);
  take(&p.vdenc_statistics, 16384);
  take(&p.vdenc_tile_row_store, uint64_t{(l.aligned_width + 31) / 32} * kCacheLine * 2);
  take(&p.cu_count, uint64_t{w16} * h16 * 4);
  p.total = at;
  const uint32_t mvt = (((l.aligned_width + 63) >> 6) * ((l.aligned_height + 15) >> 4) + 1) & ~1u;
  const uint32_t mvtb = (((l.aligned_width + 31) >> 5) * ((l.aligned_height + 31) >> 5) + 1) & ~1u;
  p.mv_temporal_bytes = uint64_t{(mvt > mvtb ? mvt : mvtb)} * kCacheLine;
  p.pak_object_cu_offset = align_up(w16 * h16 * 8 * 4, kPage);
  p.pak_object_bytes = uint64_t{p.pak_object_cu_offset} + align_up(static_cast<uint32_t>(uint64_t{2} * 4 * l.ctbs_x * l.ctbs_y * (5 + 64 * 8)), kPage);
  return p;
}

void hevc_resolve_scratch(const HevcScratchPlan& p, uint32_t base, HevcEncodeBuffers* b) {
  b->deblocking_line = base + p.deblocking_line;
  b->deblocking_tile_line = base + p.deblocking_tile_line;
  b->deblocking_tile_column = base + p.deblocking_tile_column;
  b->metadata_line = base + p.metadata_line;
  b->metadata_tile_line = base + p.metadata_tile_line;
  b->metadata_tile_column = base + p.metadata_tile_column;
  b->sao_line = base + p.sao_line;
  b->sao_tile_line = base + p.sao_tile_line;
  b->sao_tile_column = base + p.sao_tile_column;
  b->sao_row_store = base + p.sao_row_store;
  b->lcu_ildb = base + p.lcu_ildb;
  b->lcu_base = base + p.lcu_base;
  b->frame_statistics = base + p.frame_statistics;
  b->sse_row_store = base + p.sse_row_store;
  b->cu_streamout = base + p.cu_streamout;
  b->vdenc_row_store = base + p.vdenc_row_store;
  b->vdenc_statistics = base + p.vdenc_statistics;
  b->vdenc_tile_row_store = base + p.vdenc_tile_row_store;
  b->cu_count = base + p.cu_count;
}

void emit_hevc_vdenc_picture(DwordWriter& w, const HevcEncodePicture& p, const HevcEncodeBuffers& b, const HevcEncodeLayout& layout) {
  const HevcScratchPlan plan = hevc_scratch_plan(layout);
  const uint32_t qp = static_cast<uint32_t>(clip(0, 51, p.qp));

  // --- Initialise the VDEnc pipe and the HCP pipe, then select the HCP mode (encode, VDEnc, legacy single pipe) between two MFX waits.
  {
    Command<kGen12VdencControlStateDwords> c(kGen12VdencControlStateDefault);
    c.set(kGen12VdencControlState_VdencInitialization, 1);
    c.emit(w);
  }
  {
    // MI_VD_CONTROL_STATE for the HCP: 3 dwords, command 10, opcode 7, pipeline initialisation.
    w.emit(1u | (10u << 16) | (7u << 23) | (2u << 27) | (3u << 29));
    w.emit(1);
    w.emit(0);
  }
  intel_mfx::emit_mfx_wait_sync(w);
  {
    Command<kGen12HcpPipeModeSelectDwords> c(kGen12HcpPipeModeSelectDefault);
    c.set(kGen12HcpPipeModeSelect_CodecSelect, kGen12HcpPipeModeSelect_CODEC_SELECT_ENCODE);
    c.set(kGen12HcpPipeModeSelect_CodecStandardSelect, 0);   // HEVC
    c.set(kGen12HcpPipeModeSelect_PakPipelineStreamoutEnable, 1);
    c.set(kGen12HcpPipeModeSelect_VdencMode, 1);
    c.set(kGen12HcpPipeModeSelect_RdoqEnabledFlag, 0);
    c.set(kGen12HcpPipeModeSelect_PakFrameLevelStreamoutEnable, 1);
    c.emit(w);
  }
  intel_mfx::emit_mfx_wait_sync(w);

  // --- The surfaces (source, reconstruction, reference), the buffers, the bitstream, the quantiser matrices.
  hcp_surface(w, kHcpSurfaceIdSource, layout);
  hcp_surface(w, kHcpSurfaceIdRecon, layout);
  hcp_surface(w, kHcpSurfaceIdReference, layout);
  hcp_pipe_buf(w, p, b);
  hcp_ind_obj(w, b, plan);
  hcp_flat_matrices(w);

  // --- The VDEnc pipeline: mode (TLB prefetch, random-access off), surfaces, buffers.
  const bool xe_lpg = p.platform != HevcVdencPlatform::XeLp;   // (Xe-LPG's commands; Arc has them too)
  if (xe_lpg) {
    if (p.platform == HevcVdencPlatform::Xe2Hpm) for (uint32_t i = 0; i < kXe2HpmVdencPipeModeSelectDwords; ++i) w.emit(kXe2HpmVdencPipeModeSelectImage[i]);
    else for (uint32_t i = 0; i < kXeLpgVdencPipeModeSelectDwords; ++i) w.emit(kXeLpgVdencPipeModeSelectImage[i]);
  } else {
    Command<kGen12VdencPipeModeSelectDwords> c(kGen12VdencPipeModeSelectDefault);
    c.set(kGen12VdencPipeModeSelect_StandardSelect, 0);   // HEVC
    c.set(kGen12VdencPipeModeSelect_FrameStatisticsStreamOutEnable, 0);
    c.set(kGen12VdencPipeModeSelect_VdencPakObjCmdStreamOutEnable, 0);
    c.set(kGen12VdencPipeModeSelect_TlbPrefetchEnable, 1);
    c.set(kGen12VdencPipeModeSelect_PakThresholdCheckEnable, 0);
    c.set(kGen12VdencPipeModeSelect_VdencStreamInEnable, 0);
    c.set(kGen12VdencPipeModeSelect_BitDepth, kGen12VdencPipeModeSelect_BIT_DEPTH_8BIT);
    c.set(kGen12VdencPipeModeSelect_PakChromaSubSamplingType, kGen12VdencPipeModeSelect_PAK_CHROMA_SUB_SAMPLING_TYPE_420);
    c.set(kGen12VdencPipeModeSelect_OutputRangeControlAfterColorSpaceConversion, 1);
    c.set(kGen12VdencPipeModeSelect_IsRandomAccess, 0);   // low delay
    c.set(kGen12VdencPipeModeSelect_HmeRegionPreFetchenable, 0);
    c.set(kGen12VdencPipeModeSelect_PreFetchoffsetforsource, 4);
    c.set(kGen12VdencPipeModeSelect_Numverticalreqminus1Src, 1);
    c.emit(w);
  }
  vdenc_surface(w, kVdencSrcSurfaceStateDefault, layout.width, layout.height, layout.full, 7);   // (chroma downsample filter 7 on the source from Xe-LP on)
  vdenc_surface(w, kVdencRefSurfaceStateDefault, layout.width, layout.height, layout.full, 0);
  vdenc_ds_surfaces(w, layout);
  if (xe_lpg) vdenc_pipe_buf<kXeLpgVdencPipeBufAddrStateDwords>(w, kXeLpgVdencPipeBufAddrStateImage, p, b);
  else vdenc_pipe_buf<kGen12VdencPipeBufAddrStateDwords>(w, kGen12VdencPipeBufAddrStateDefault, p, b);

  // --- CMD1, HCP_PIC_STATE, CMD2.
  if (xe_lpg) xe_lpg_cmd1(w, p);
  else vdenc_cmd1(w, p);
  {
    Command<kGen12HcpPicStateDwords> c(kGen12HcpPicStateDefault);
    c.set(kGen12HcpPicState_Framewidthinmincbminus1, layout.width / 8 - 1);
    c.set(kGen12HcpPicState_Frameheightinmincbminus1, layout.height / 8 - 1);
    c.set(kGen12HcpPicState_Mincusize, kHevcLog2MinCb - 3);
    c.set(kGen12HcpPicState_CtbsizeLcusize, kHevcLog2Ctb - 3);
    c.set(kGen12HcpPicState_Maxtusize, kHevcLog2MaxTb - 2);
    c.set(kGen12HcpPicState_Mintusize, kHevcLog2MinTb - 2);
    c.set(kGen12HcpPicState_Minpcmsize, 0);
    c.set(kGen12HcpPicState_Maxpcmsize, 0);
    c.set(kGen12HcpPicState_ChromaSubsampling, 1);
    c.set(kGen12HcpPicState_Colpicisi, 0);
    c.set(kGen12HcpPicState_Curpicisi, 0);
    c.set(kGen12HcpPicState_SampleAdaptiveOffsetEnabledFlag, 0);
    c.set(kGen12HcpPicState_PcmEnabledFlag, 0);
    c.set(kGen12HcpPicState_CuQpDeltaEnabledFlag, 1);
    c.set(kGen12HcpPicState_DiffCuQpDeltaDepthOrNamedAsMaxDqpDepth, kHevcDiffCuQpDeltaDepth);
    c.set(kGen12HcpPicState_Log2ParallelMergeLevelMinus2, kHevcLog2ParMrgLevel - 2);
    c.set(kGen12HcpPicState_StrongIntraSmoothingEnableFlag, 1);   // (the SPS says so)
    c.set(kGen12HcpPicState_PicCbQpOffset, static_cast<uint32_t>(p.cb_qp_offset) & 0x1F);
    c.set(kGen12HcpPicState_PicCrQpOffset, static_cast<uint32_t>(p.cr_qp_offset) & 0x1F);
    c.set(kGen12HcpPicState_MaxTransformHierarchyDepthIntraOrNamedAsTuMaxDepthIntra, kHevcMaxTransformDepthIntra);
    c.set(kGen12HcpPicState_MaxTransformHierarchyDepthInterOrNamedAsTuMaxDepthInter, kHevcMaxTransformDepthInter);
    c.set(kGen12HcpPicState_RhodomainRateControlEnable, 1);
    c.set(kGen12HcpPicState_Rhodomainframelevelqp, static_cast<uint32_t>(clip(0, 51, p.pps_qp)));
    c.set(kGen12HcpPicState_FractionalQpAdjustmentEnable, 1);
    c.set(kGen12HcpPicState_FirstSliceSegmentInPicFlag, 1);
    c.set(kGen12HcpPicState_SseEnable, 1);
    c.set(kGen12HcpPicState_RdoqEnable, 0);
    c.set(kGen12HcpPicState_TemporalMvPredDisable, 1);
    c.emit(w);
  }
  if (xe_lpg) xe_lpg_cmd2(w, p, layout);
  else vdenc_cmd2(w, p, layout);
  (void)qp;
}

bool emit_hevc_vdenc_slice(DwordWriter& w, const HevcEncodePicture& p, const HevcEncodeLayout& layout, ArrayView<const HevcHeaderInsert> headers) {
  if (headers.size() == 0) return false;
  const uint32_t qp = static_cast<uint32_t>(clip(0, 51, p.qp));

  // --- The reference picture list of a low-delay B slice: both lists hold the previous picture.
  if (p.p_picture) {
    const int32_t distance = static_cast<int32_t>(p.poc) - static_cast<int32_t>(p.reference_poc);
    hcp_ref_idx(w, 0, distance);
    hcp_ref_idx(w, 1, distance);
  }

  // --- HCP_SLICE_STATE: the whole picture is the one slice.
  {
    Command<kGen12HcpSliceStateDwords> c(kGen12HcpSliceStateDefault);
    c.set(kGen12HcpSliceState_SlicestartctbxOrSliceStartLcuXEncoder, 0);
    c.set(kGen12HcpSliceState_SlicestartctbyOrSliceStartLcuYEncoder, 0);
    c.set(kGen12HcpSliceState_NextslicestartctbxOrNextSliceStartLcuXEncoder, 0);
    c.set(kGen12HcpSliceState_NextslicestartctbyOrNextSliceStartLcuYEncoder, 0);
    c.set(kGen12HcpSliceState_SliceType, p.p_picture ? kGen12HcpSliceState_SLICE_TYPE_B_SLICE : kGen12HcpSliceState_SLICE_TYPE_I_SLICE);
    c.set(kGen12HcpSliceState_Lastsliceofpic, 1);
    c.set(kGen12HcpSliceState_SliceqpSignFlag, 0);
    c.set(kGen12HcpSliceState_DependentSliceFlag, 0);
    c.set(kGen12HcpSliceState_SliceTemporalMvpEnableFlag, 0);
    c.set(kGen12HcpSliceState_Sliceqp, qp);
    c.set(kGen12HcpSliceState_SliceCbQpOffset, 0);
    c.set(kGen12HcpSliceState_SliceCrQpOffset, 0);
    c.set(kGen12HcpSliceState_SliceHeaderDisableDeblockingFilterFlag, p.deblocking ? 0 : 1);
    c.set(kGen12HcpSliceState_SliceLoopFilterAcrossSlicesEnabledFlag, 1);
    c.set(kGen12HcpSliceState_SliceSaoChromaFlag, 0);
    c.set(kGen12HcpSliceState_SliceSaoLumaFlag, 0);
    c.set(kGen12HcpSliceState_Islowdelay, 1);
    c.set(kGen12HcpSliceState_CollocatedFromL0Flag, 1);
    c.set(kGen12HcpSliceState_Maxmergeidx, 4);
    c.set(kGen12HcpSliceState_Collocatedrefidx, 0);
    c.set(kGen12HcpSliceState_Roundinter, 4);
    c.set(kGen12HcpSliceState_Roundintra, 10);
    c.set(kGen12HcpSliceState_Cabaczerowordinsertionenable, 0);
    c.set(kGen12HcpSliceState_Emulationbytesliceinsertenable, 1);
    c.set(kGen12HcpSliceState_TailInsertionEnable, 0);
    c.set(kGen12HcpSliceState_SlicedataEnable, 1);
    c.set(kGen12HcpSliceState_HeaderInsertionEnable, 1);
    c.emit(w);
  }

  // --- The headers go in front of the slice data the engine writes.
  for (uint64_t i = 0; i < headers.size(); ++i) {
    if ((headers[i].bits + 7) / 32 + 1 > kMaxPakInsertDwords) return false;
    pak_insert(w, headers[i], i + 1 == headers.size());
  }

  if (p.platform != HevcVdencPlatform::XeLp) {
    // --- VDENC_WEIGHTSOFFSETS_STATE (7 dwords), the tile and slice state (the whole picture is one tile of the picture's size) and the 3-dword walker.
    for (uint32_t i = 0; i < kXeLpgVdencWeightsOffsetsStateDwords; ++i) w.emit(kXeLpgVdencWeightsOffsetsStateImage[i]);
    {
      Command<kXeLpgVdencTileSliceStateDwords> c(kXeLpgVdencTileSliceStateImage);
      c.set(kXeLpgVdencTileSliceState_TileWidth, (layout.width >= 256 ? align_up(layout.width, 8) : layout.width) - 1);
      c.set(kXeLpgVdencTileSliceState_TileHeight, (layout.height >= 128 ? align_up(layout.height, 8) : layout.height) - 1);
      c.emit(w);
    }
    {
      Command<kXeLpgVdencWalkerStateDwords> c(kXeLpgVdencWalkerStateImage);
      c.set(kXeLpgVdencWalkerState_FirstSuperSlice, 0);
      c.set(kXeLpgVdencWalkerState_MbLcuStartXPosition, 0);
      c.set(kXeLpgVdencWalkerState_MbLcuStartYPosition, 0);
      c.set(kXeLpgVdencWalkerState_NextsliceMbLcuStartXPosition, layout.ctbs_x);
      c.set(kXeLpgVdencWalkerState_NextsliceMbStartYPosition, layout.ctbs_y);
      c.emit(w);
    }
  } else {
    // --- VDENC_WEIGHTSOFFSETS_STATE: no weighted prediction.
    {
      Command<kGen12VdencWeightsOffsetsStateDwords> c(kGen12VdencWeightsOffsetsStateDefault);
      c.set(kGen12VdencWeightsOffsetsState_WeightsForwardReference0, 1);
      c.set(kGen12VdencWeightsOffsetsState_OffsetForwardReference0, 0);
      c.set(kGen12VdencWeightsOffsetsState_WeightsForwardReference1, 1);
      c.set(kGen12VdencWeightsOffsetsState_OffsetForwardReference1, 0);
      c.set(kGen12VdencWeightsOffsetsState_WeightsForwardReference2, 1);
      c.set(kGen12VdencWeightsOffsetsState_OffsetForwardReference2, 0);
      c.set(kGen12VdencWeightsOffsetsState_HevcVp9WeightsBackwardReference0, 1);
      c.set(kGen12VdencWeightsOffsetsState_HevcVp9OffsetBackwardReference0, 0);
      c.emit(w);
    }

    // --- VDENC_WALKER_STATE: the one slice covers the picture. The rows from DW11 on are the driver's tables by slice quantiser (AddVdencWalkerStateCmd, HEVC branch, target usage 7).
    {
      Command<kGen12VdencWalkerStateDwords> c(kGen12VdencWalkerStateDefault);
      c.set(kGen12VdencWalkerState_MbLcuStartYPosition, 0);
      c.set(kGen12VdencWalkerState_MbLcuStartXPosition, 0);
      c.set(kGen12VdencWalkerState_NextsliceMbLcuStartXPosition, layout.ctbs_x);
      c.set(kGen12VdencWalkerState_NextsliceMbStartYPosition, layout.ctbs_y);
      c.set(kGen12VdencWalkerState_Log2WeightDenomLuma, 0);
      c.set(kGen12VdencWalkerState_HevcLog2WeightDemonLuma, 0);
      c.set(kGen12VdencWalkerState_TileWidth, layout.width - 1);
      c.set(kGen12VdencWalkerState_TileHeight, layout.height - 1);
      c.raw(11, c.get(11) & 0xfffc00ffu);
      c.raw(12, (c.get(12) & 0xfcffffffu) | 0x1000000);
      c.set(kGen12VdencWalkerState_IbcControl, 0);
      c.set(kGen12VdencWalkerState_PaletteModeEnable, 0);
      uint32_t index;
      if (qp <= 12) index = 0;
      else if (qp <= 47) index = (qp - 8) / 5;
      else if (qp <= 49) index = 8;
      else index = 9;
      static const uint32_t kTable1[10] = {0x50001, 0x50001, 0x50001, 0x50002, 0x90002, 0x90002, 0x90002, 0xd0002, 0x190002, 0x210003};
      static const uint32_t kTable2[10] = {0x2000a, 0x2000a, 0x2000a, 0x4000a, 0x8000a, 0xc0010, 0xc0018, 0xc0018, 0x100020, 0x180030};
      static const uint32_t kTable3[10] = {0x101004, 0x101004, 0x101004, 0xc1008, 0x42004, 0x42006, 0x13f06, 0x13f06, 0x13f0c, 0x13006};
      static const uint32_t kTable4[10] = {0x100004, 0x100004, 0x100004, 0x100004, 0x200004, 0x300004, 0x400004, 0x600004, 0x800004, 0x1000004};
      c.raw(12, (c.get(12) & 0xff80fff8u) | kTable1[index]);
      c.raw(13, kTable2[index]);
      c.raw(14, (c.get(14) & 0xffe0c0c0u) | kTable3[index]);
      c.raw(15, (c.get(15) & 0xfc00u) | kTable4[index]);
      c.raw(12, c.get(12) & 0xffff00ffu);
      c.raw(14, (c.get(14) & 0x9fffffu) | 0xc8400000u);
      c.raw(16, (c.get(16) & 0xffffffu) | 0xa6000000u);
      c.raw(16, (c.get(16) & 0xffc0c0c0u) | 0x313131);   // target usage 7
      c.emit(w);
    }
  }
  (void)layout;

  // --- Wait for the VDEnc and MFX pipes and flush the VDEnc commands.
  {
    Command<kVdPipelineFlushDwords> c(kVdPipelineFlushDefault);
    c.set(kVdPipelineFlush_MfxPipelineDone, 1);
    c.set(kVdPipelineFlush_VdencPipelineDone, 1);
    c.set(kVdPipelineFlush_VdencPipelineCommandFlush, 1);
    c.set(kVdPipelineFlush_VdCommandMessageParserDone, 1);
    c.emit(w);
  }
  return w.ok();
}

void emit_hevc_vdenc_finish(DwordWriter& w, const HevcEncodeBuffers& b) {
  intel_mfx::emit_mi_flush(w);
  {
    Command<kVdPipelineFlushDwords> c(kVdPipelineFlushDefault);
    c.set(kVdPipelineFlush_HevcPipelineDone, 1);
    c.set(kVdPipelineFlush_HevcPipelineCommandFlush, 1);
    c.set(kVdPipelineFlush_VdCommandMessageParserDone, 1);
    c.emit(w);
  }
  intel_mfx::emit_mi_flush(w);
  if (b.status) {
    // MI_STORE_REGISTER_MEM (48-bit address): the number of bytes the PAK wrote for the frame.
    w.emit((0x24u << 23) | 2u);
    w.emit(kHcpBitstreamByteCountRegisterGen12);
    w.emit(b.status);
    w.emit(0);
  }
  intel_mfx::emit_batch_end(w);
}

}  // namespace intel_enc
}  // namespace hardware
