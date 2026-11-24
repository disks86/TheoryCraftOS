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
// DXVA AV1 decoding (D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0: 8- and 10-bit 4:2:0 and monochrome) on the CPU AV1 decoder (Software/Av1*.cpp).
//
// DXVA_PicParams_AV1 carries the sequence and frame header fields in force for the frame (the program's parser has already loaded what a header inherits from its
// primary reference: segmentation features, loop filter deltas, film grain); the bitstream holds tile data only. So the decoder's header structures (dav1d's
// Dav1dSequenceHeader / Dav1dFrameHeader, as Av1Obu.cpp leaves them) are made from the parameters, with what the parser derives derived here too (the coded width
// of super-resolution, tile starts, lossless flags, segment qindex, the skip mode references from the order hints of the references), and the core decodes the
// tiles with them (av1::Core::decode_frame_external). An AV1 reference carries more than its pixels - CDFs, the segmentation map, motion vectors, the frame
// header (order hints, sizes) - so all of it is kept per surface key; the eight slots of RefFrameMapTextureIndex name the surfaces, frame_refs[].Index the slots.
//
// The frame comes back at once, in decoding order, whether it is shown or not (show_frame 0: the program shows the surface itself later), at its size (the
// upscaled width), as NV12 (8-bit) or P010 (10-bit, the value in the top bits). With apply_grain the film grain is applied to the output, as DXVA has the
// accelerator do; the surface's reference picture stays without grain. Large-scale tile decoding (anchor frames) is not offered: Unsupported.
#include "Hardware/VideoCodec/Dxva/DxvaCodecs.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaSurfaces.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaTypesAv1.hpp"
#include "Hardware/VideoCodec/Software/Av1Core.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {
namespace {

using av1::FrameHeader;
using av1::Picture;
using av1::RefSlot;
using av1::SequenceHeader;
using av1::TileData;

constexpr uint8_t kNoSlot = 0xFF;

inline bool bit(uint32_t word, int position) { return ((word >> position) & 1) != 0; }
inline uint32_t bits(uint32_t word, int position, int count) { return (word >> position) & ((1u << count) - 1); }
inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int iabs(int v) { return v < 0 ? -v : v; }

// What a surface key holds: the frame's state as a reference (picture without grain, segmentation map, motion vectors, CDFs; the picture carries the header).
struct Av1Surface {
  RefSlot state;
};

class Av1DxvaDecoder final : public IDxvaDecoder {
 public:
  Av1DxvaDecoder(allocators::IAllocator& allocator, const DxvaDecoderConfig& config)
      : allocator_(allocator), config_(config), surfaces_(config.surfaces + 1), tiles_(allocator), out_(allocator) {}
  ~Av1DxvaDecoder() = default;

  [[nodiscard]] bool init() {
    core_ = UniquePointer<av1::Core>::make(allocator_, allocator_);
    return static_cast<bool>(core_);
  }

  DxvaProfile profile() const override { return DxvaProfile::Av1VldProfile0; }

  void reset() override {
    surfaces_.clear();
    surfaces_.for_each_state([](Av1Surface& s) { s.state = RefSlot(); });
  }

  [[nodiscard]] DxvaResult decode(const DxvaPicture& picture, DxvaDecodedPicture* out) override;

 private:
  DxvaResult refuse(DxvaDecodedPicture* out, DxvaResult result, const char* why) {
    out->detail = why;
    return result;
  }
  // The headers of the frame from the parameters; Ok or what to refuse it with (*why says why).
  DxvaResult make_headers(const dxva::PicParamsAv1& pp, SequenceHeader* seq, FrameHeader* hdr, const char** why);
  // The tiles of the frame by tile number; Ok or what to refuse.
  DxvaResult collect_tiles(const DxvaPicture& picture, const FrameHeader& hdr, const char** why);
  // A mid-grey picture standing in for a missing reference of `width` x `height` with the order hint `order_hint`, and the default CDFs.
  [[nodiscard]] bool grey_reference(const SequenceHeader& seq, const FrameHeader& hdr, int width, int height, RefSlot* slot);
  [[nodiscard]] bool output(const Picture& p, DxvaDecodedPicture* out);

  allocators::IAllocator& allocator_;
  DxvaDecoderConfig config_;
  UniquePointer<av1::Core> core_;
  dxva::SurfaceTable<Av1Surface> surfaces_;
  av1::OwnedArray<TileData> tiles_;
  ByteBuffer out_;
};

DxvaResult Av1DxvaDecoder::make_headers(const dxva::PicParamsAv1& pp, SequenceHeader* seq_out, FrameHeader* hdr_out, const char** why) {
  namespace c = dxva::av1_coding;
  const uint32_t coding = pp.CodingParamToolFlags;
  const uint32_t format = pp.FormatAndPictureInfoFlags;
  const bool mono = bit(format, 6), ss_x = bit(format, 4), ss_y = bit(format, 5);
  if (pp.seq_profile != 0) {
    *why = "AV1: only the Main profile (seq_profile 0) is decoded";
    return DxvaResult::Unsupported;
  }
  if (pp.bitdepth != 8 && pp.bitdepth != 10) {
    *why = pp.bitdepth == 12 ? "AV1: 12-bit samples are not in profile 0" : "AV1: bitdepth out of range";
    return pp.bitdepth == 12 ? DxvaResult::Unsupported : DxvaResult::Invalid;
  }
  if (!ss_x || !ss_y) {
    *why = "AV1: only 4:2:0 and monochrome are decoded (profile 0)";
    return DxvaResult::Unsupported;
  }
  if ((pp.bitdepth == 8) != (config_.output == PixelFormat::Nv12)) {
    *why = "AV1: the frame's bit depth does not match the decoder's output format (8-bit: NV12, 10-bit: P010)";
    return DxvaResult::Invalid;
  }
  *why = "AV1: picture parameters out of range";
  SequenceHeader seq{};
  seq.profile = 0;
  seq.max_width = static_cast<int>(pp.max_width);
  seq.max_height = static_cast<int>(pp.max_height);
  seq.layout = mono ? av1::PIXEL_LAYOUT_I400 : av1::PIXEL_LAYOUT_I420;
  seq.hbd = pp.bitdepth == 10 ? 1 : 0;
  seq.pri = av1::COLOR_PRI_UNKNOWN;
  seq.trc = av1::TRC_UNKNOWN;
  seq.mtrx = bit(pp.film_grain.ControlFlags, 12) ? av1::MC_IDENTITY : av1::MC_UNKNOWN;
  seq.num_operating_points = 1;
  seq.sb128 = bit(coding, c::kUse128x128Superblock);
  seq.filter_intra = bit(coding, c::kFilterIntra);
  seq.intra_edge_filter = bit(coding, c::kIntraEdgeFilter);
  seq.inter_intra = bit(coding, c::kInterintraCompound);
  seq.masked_compound = bit(coding, c::kMaskedCompound);
  seq.warped_motion = bit(coding, c::kWarpedMotion);
  seq.dual_filter = bit(coding, c::kDualFilter);
  seq.jnt_comp = bit(coding, c::kJntComp);
  seq.ref_frame_mvs = bit(coding, c::kEnableRefFrameMvs);
  seq.order_hint_n_bits = pp.order_hint_bits;
  seq.order_hint = pp.order_hint_bits != 0;
  if (pp.order_hint_bits > 8) return DxvaResult::Invalid;
  seq.super_res = bit(coding, c::kSuperres);
  seq.cdef = bit(coding, c::kCdef);
  seq.restoration = bit(coding, c::kRestoration);
  seq.ss_hor = seq.ss_ver = 1;
  seq.monochrome = mono;
  seq.film_grain_present = bit(coding, c::kFilmGrain);
  seq.screen_content_tools = bit(coding, c::kScreenContentTools) ? av1::ADAPTIVE : av1::OFF;
  seq.force_integer_mv = av1::ADAPTIVE;

  FrameHeader hdr{};
  hdr.frame_type = static_cast<av1::FrameType>(bits(format, 0, 2));
  const bool intra = av1::is_key_or_intra(hdr);
  hdr.show_frame = bit(format, 2);
  hdr.showable_frame = bit(format, 3);
  if (pp.width == 0 || pp.height == 0 || pp.width > 65536 || pp.height > 65536 || pp.max_width > 65536 || pp.max_height > 65536 || pp.width > pp.max_width ||
      pp.height > pp.max_height)
    return DxvaResult::Invalid;
  if (pp.width > config_.width || pp.height > config_.height) {
    *why = "AV1: the frame is larger than the decoder's surfaces";
    return DxvaResult::Invalid;
  }
  hdr.width[1] = static_cast<int>(pp.width);
  hdr.height = static_cast<int>(pp.height);
  hdr.render_width = hdr.width[1];
  hdr.render_height = hdr.height;
  hdr.super_res.enabled = bit(coding, c::kSuperres);
  if (hdr.super_res.enabled) {
    if (pp.superres_denom < 9 || pp.superres_denom > 16) return DxvaResult::Invalid;
    const int d = hdr.super_res.width_scale_denominator = pp.superres_denom;
    hdr.width[0] = imax((hdr.width[1] * 8 + (d >> 1)) / d, imin(16, hdr.width[1]));
  } else {
    hdr.super_res.width_scale_denominator = 8;
    hdr.width[0] = hdr.width[1];
  }
  hdr.frame_offset = pp.order_hint;
  hdr.disable_cdf_update = bit(coding, c::kDisableCdfUpdate);
  hdr.allow_screen_content_tools = bit(coding, c::kScreenContentTools);
  hdr.force_integer_mv = intra ? 1 : bit(coding, c::kIntegerMv);
  if (pp.primary_ref_frame > av1::PRIMARY_REF_NONE || (intra && pp.primary_ref_frame != av1::PRIMARY_REF_NONE)) return DxvaResult::Invalid;
  hdr.primary_ref_frame = pp.primary_ref_frame;
  hdr.allow_intrabc = bit(coding, c::kIntrabc);
  if (hdr.allow_intrabc && (!intra || !hdr.allow_screen_content_tools || hdr.super_res.enabled)) return DxvaResult::Invalid;
  for (int i = 0; i < av1::REFS_PER_FRAME; ++i) {
    const uint8_t slot = pp.frame_refs[i].Index;
    if (!intra && slot >= av1::NUM_REF_FRAMES) {
      *why = "AV1: frame_refs[].Index names no reference slot";
      return DxvaResult::Invalid;
    }
    hdr.refidx[i] = static_cast<int8_t>(intra ? 0 : slot);
  }
  if (!intra) {
    hdr.hp = hdr.force_integer_mv ? 0 : bit(coding, c::kHighPrecisionMv);
    if (pp.interp_filter > av1::FILTER_SWITCHABLE) return DxvaResult::Invalid;
    hdr.subpel_filter_mode = static_cast<av1::FilterMode>(pp.interp_filter);
    hdr.switchable_motion_mode = bit(coding, c::kSwitchableMotionMode);
    hdr.use_ref_frame_mvs = bit(coding, c::kUseRefFrameMvs) && seq.ref_frame_mvs && seq.order_hint;
  }
  hdr.refresh_context = !hdr.disable_cdf_update && !bit(coding, c::kDisableFrameEndUpdateCdf);

  // tiles (in superblocks)
  const int sb_log2 = 6 + seq.sb128;
  const int sbw = (hdr.width[0] + (1 << sb_log2) - 1) >> sb_log2, sbh = (hdr.height + (1 << sb_log2) - 1) >> sb_log2;
  const int max_tile_width_sb = 4096 >> sb_log2;
  if (pp.tiles.cols == 0 || pp.tiles.rows == 0 || pp.tiles.cols > av1::MAX_TILE_COLS || pp.tiles.rows > av1::MAX_TILE_ROWS) return DxvaResult::Invalid;
  hdr.tiling.cols = pp.tiles.cols;
  hdr.tiling.rows = pp.tiles.rows;
  int start = 0;
  for (int i = 0; i < hdr.tiling.cols; ++i) {
    const int w = pp.tiles.widths[i];
    if (w < 1 || w > max_tile_width_sb || w > sbw - start) return DxvaResult::Invalid;
    hdr.tiling.col_start_sb[i] = static_cast<uint16_t>(start);
    start += w;
  }
  if (start != sbw) return DxvaResult::Invalid;
  hdr.tiling.col_start_sb[hdr.tiling.cols] = static_cast<uint16_t>(sbw);
  start = 0;
  for (int i = 0; i < hdr.tiling.rows; ++i) {
    const int h = pp.tiles.heights[i];
    if (h < 1 || h > sbh - start) return DxvaResult::Invalid;
    hdr.tiling.row_start_sb[i] = static_cast<uint16_t>(start);
    start += h;
  }
  if (start != sbh) return DxvaResult::Invalid;
  hdr.tiling.row_start_sb[hdr.tiling.rows] = static_cast<uint16_t>(sbh);
  if (pp.tiles.context_update_id >= hdr.tiling.cols * hdr.tiling.rows) return DxvaResult::Invalid;
  hdr.tiling.update = pp.tiles.context_update_id;
  int log2 = 0;
  while ((1 << log2) < hdr.tiling.cols) ++log2;
  hdr.tiling.log2_cols = static_cast<uint8_t>(log2);
  log2 = 0;
  while ((1 << log2) < hdr.tiling.rows) ++log2;
  hdr.tiling.log2_rows = static_cast<uint8_t>(log2);
  hdr.tiling.uniform = 0;
  hdr.tiling.n_bytes = 4;

  // quantization
  auto delta_ok = [](int v) { return v >= -64 && v <= 63; };
  const auto& q = pp.quantization;
  if (!delta_ok(q.y_dc_delta_q) || !delta_ok(q.u_dc_delta_q) || !delta_ok(q.v_dc_delta_q) || !delta_ok(q.u_ac_delta_q) || !delta_ok(q.v_ac_delta_q))
    return DxvaResult::Invalid;
  hdr.quant.yac = q.base_qindex;
  hdr.quant.ydc_delta = q.y_dc_delta_q;
  if (!mono) {
    hdr.quant.udc_delta = q.u_dc_delta_q;
    hdr.quant.uac_delta = q.u_ac_delta_q;
    hdr.quant.vdc_delta = q.v_dc_delta_q;
    hdr.quant.vac_delta = q.v_ac_delta_q;
  }
  hdr.quant.qm = q.qm_y != 0xFF;
  if (hdr.quant.qm) {
    if (q.qm_y > 15 || q.qm_u > 15 || q.qm_v > 15) return DxvaResult::Invalid;
    hdr.quant.qm_y = q.qm_y;
    hdr.quant.qm_u = q.qm_u;
    hdr.quant.qm_v = q.qm_v;
  }

  // segmentation
  const auto& sg = pp.segmentation;
  hdr.segmentation.enabled = bit(sg.ControlFlags, 0);
  for (av1::SegmentationData& d : hdr.segmentation.seg_data.d) d.ref = -1;
  hdr.segmentation.seg_data.last_active_segid = -1;
  if (hdr.segmentation.enabled) {
    hdr.segmentation.update_map = bit(sg.ControlFlags, 1);
    hdr.segmentation.update_data = bit(sg.ControlFlags, 2);
    hdr.segmentation.temporal = hdr.segmentation.update_map && bit(sg.ControlFlags, 3);
    if (hdr.primary_ref_frame == av1::PRIMARY_REF_NONE && !hdr.segmentation.update_map) return DxvaResult::Invalid;   // (5.9.14: inferred 1)
    static const int kMax[6] = {255, 63, 63, 63, 63, 7};
    for (int i = 0; i < av1::MAX_SEGMENTS; ++i) {
      av1::SegmentationData& d = hdr.segmentation.seg_data.d[i];
      const uint8_t mask = sg.feature_mask[i];
      for (int j = 0; j < 6; ++j) {
        if (!bit(mask, j)) continue;
        const int v = sg.feature_data[i][j];
        if (j == 5 ? (v < 0 || v > kMax[j]) : (v < -kMax[j] || v > kMax[j])) return DxvaResult::Invalid;
      }
      if (bit(mask, 0)) d.delta_q = sg.feature_data[i][0];
      if (bit(mask, 1)) d.delta_lf_y_v = static_cast<int8_t>(sg.feature_data[i][1]);
      if (bit(mask, 2)) d.delta_lf_y_h = static_cast<int8_t>(sg.feature_data[i][2]);
      if (bit(mask, 3)) d.delta_lf_u = static_cast<int8_t>(sg.feature_data[i][3]);
      if (bit(mask, 4)) d.delta_lf_v = static_cast<int8_t>(sg.feature_data[i][4]);
      if (bit(mask, 5)) d.ref = static_cast<int8_t>(sg.feature_data[i][5]);
      d.skip = bit(mask, 6);
      d.globalmv = bit(mask, 7);
      d.feature_mask = mask;
      if (mask) hdr.segmentation.seg_data.last_active_segid = static_cast<int8_t>(i);
      if (bit(mask, 5) || bit(mask, 6) || bit(mask, 7)) hdr.segmentation.seg_data.preskip = 1;
    }
  }

  // delta q / lf
  const auto& lf = pp.loop_filter;
  hdr.delta.q.present = bit(q.ControlFlags, 0);
  if (hdr.delta.q.present) {
    if (!hdr.quant.yac) return DxvaResult::Invalid;
    hdr.delta.q.res_log2 = static_cast<uint8_t>(bits(q.ControlFlags, 1, 2));
    hdr.delta.lf.present = bit(lf.ControlFlags, 3);
    if (hdr.delta.lf.present) {
      if (hdr.allow_intrabc || lf.delta_lf_res > 3) return DxvaResult::Invalid;
      hdr.delta.lf.res_log2 = lf.delta_lf_res;
      hdr.delta.lf.multi = bit(lf.ControlFlags, 2);
    }
  }

  // lossless (as the parser derives it)
  const bool delta_lossless = !hdr.quant.ydc_delta && !hdr.quant.udc_delta && !hdr.quant.uac_delta && !hdr.quant.vdc_delta && !hdr.quant.vac_delta;
  hdr.all_lossless = 1;
  for (int i = 0; i < av1::MAX_SEGMENTS; i++) {
    const int qidx = hdr.quant.yac + hdr.segmentation.seg_data.d[i].delta_q;
    hdr.segmentation.qidx[i] = hdr.segmentation.enabled ? static_cast<uint8_t>(qidx < 0 ? 0 : qidx > 255 ? 255 : qidx) : hdr.quant.yac;
    hdr.segmentation.lossless[i] = !hdr.segmentation.qidx[i] && delta_lossless;
    hdr.all_lossless &= hdr.segmentation.lossless[i];
  }

  // loop filter
  if (hdr.all_lossless || hdr.allow_intrabc) {
    hdr.loopfilter.mode_ref_delta_enabled = 1;
    hdr.loopfilter.mode_ref_delta_update = 1;
    hdr.loopfilter.mode_ref_deltas = {{0, 0}, {1, 0, 0, 0, -1, 0, -1, -1}};
  } else {
    if (lf.filter_level[0] > 63 || lf.filter_level[1] > 63 || lf.filter_level_u > 63 || lf.filter_level_v > 63 || lf.sharpness_level > 7) return DxvaResult::Invalid;
    hdr.loopfilter.level_y[0] = lf.filter_level[0];
    hdr.loopfilter.level_y[1] = lf.filter_level[1];
    if (!mono) {
      hdr.loopfilter.level_u = lf.filter_level_u;
      hdr.loopfilter.level_v = lf.filter_level_v;
    }
    hdr.loopfilter.sharpness = lf.sharpness_level;
    hdr.loopfilter.mode_ref_delta_enabled = bit(lf.ControlFlags, 0);
    hdr.loopfilter.mode_ref_delta_update = bit(lf.ControlFlags, 1);
    for (int i = 0; i < 8; ++i) {
      if (!delta_ok(lf.ref_deltas[i])) return DxvaResult::Invalid;
      hdr.loopfilter.mode_ref_deltas.ref_delta[i] = lf.ref_deltas[i];
    }
    for (int i = 0; i < 2; ++i) {
      if (!delta_ok(lf.mode_deltas[i])) return DxvaResult::Invalid;
      hdr.loopfilter.mode_ref_deltas.mode_delta[i] = lf.mode_deltas[i];
    }
  }

  // cdef
  if (!hdr.all_lossless && seq.cdef && !hdr.allow_intrabc) {
    hdr.cdef.damping = static_cast<uint8_t>(bits(pp.cdef.ControlFlags, 0, 2) + 3);
    hdr.cdef.n_bits = static_cast<uint8_t>(bits(pp.cdef.ControlFlags, 2, 2));
    for (int i = 0; i < (1 << hdr.cdef.n_bits); ++i) {
      const uint8_t y = pp.cdef.y_strengths[i], uv = pp.cdef.uv_strengths[i];
      if ((y & 0x3F) > 15 || (uv & 0x3F) > 15) return DxvaResult::Invalid;
      hdr.cdef.y_strength[i] = static_cast<uint8_t>(((y & 0x3F) << 2) | (y >> 6));
      if (!mono) hdr.cdef.uv_strength[i] = static_cast<uint8_t>(((uv & 0x3F) << 2) | (uv >> 6));
    }
  }

  // loop restoration (DXVA: 0 none, 1 Wiener, 2 self-guided, 3 switchable)
  if ((!hdr.all_lossless || hdr.super_res.enabled) && seq.restoration && !hdr.allow_intrabc) {
    static const av1::RestorationType kType[4] = {av1::RESTORATION_NONE, av1::RESTORATION_WIENER, av1::RESTORATION_SGRPROJ, av1::RESTORATION_SWITCHABLE};
    bool any = false;
    for (int p = 0; p < (mono ? 1 : 3); ++p) {
      if (lf.frame_restoration_type[p] > 3) return DxvaResult::Invalid;
      hdr.restoration.type[p] = kType[lf.frame_restoration_type[p]];
      any = any || hdr.restoration.type[p] != av1::RESTORATION_NONE;
    }
    if (any) {
      const int y = lf.log2_restoration_unit_size[0], uv = lf.log2_restoration_unit_size[1];
      if (y < 6 + seq.sb128 || y > 8) return DxvaResult::Invalid;
      hdr.restoration.unit_size[0] = static_cast<uint8_t>(y);
      hdr.restoration.unit_size[1] = static_cast<uint8_t>(y);
      if (!mono && (hdr.restoration.type[1] || hdr.restoration.type[2])) {
        if (uv != y && uv != y - 1) return DxvaResult::Invalid;
        hdr.restoration.unit_size[1] = static_cast<uint8_t>(uv);
      }
    } else {
      hdr.restoration.unit_size[0] = 8;
    }
  }

  const uint32_t tx_mode = bits(coding, c::kTxMode, 2);
  // (5.9.21: TX_MODE_ONLY_4X4 when every segment is lossless, else TX_MODE_LARGEST or TX_MODE_SELECT)
  if (tx_mode > av1::TX_SWITCHABLE || (!hdr.all_lossless && tx_mode == av1::TX_4X4_ONLY)) return DxvaResult::Invalid;
  hdr.txfm_mode = hdr.all_lossless ? av1::TX_4X4_ONLY : static_cast<av1::TxfmMode>(tx_mode);
  if (!intra) {
    hdr.switchable_comp_refs = bit(coding, c::kReferenceMode);
    hdr.warp_motion = bit(coding, c::kWarpedMotion);
  }
  hdr.reduced_txtp_set = bit(coding, c::kReducedTxSet);

  // global motion
  for (av1::WarpedMotionParams& g : hdr.gmv) g = av1::default_wm_params();
  if (!intra) {
    for (int i = 0; i < av1::REFS_PER_FRAME; ++i) {
      const dxva::PicEntryAv1& r = pp.frame_refs[i];
      const uint32_t type = bits(r.GlobalMotionFlags, 1, 2);
      if (type == av1::WM_TYPE_IDENTITY) continue;
      av1::WarpedMotionParams& g = hdr.gmv[i];
      g.type = static_cast<av1::WarpedMotionType>(type);
      for (int k = 0; k < 6; ++k) g.matrix[k] = r.wmmat[k];
      if (type == av1::WM_TYPE_TRANSLATION) {
        g.matrix[2] = g.matrix[5] = 1 << 16;
        g.matrix[3] = g.matrix[4] = 0;
      } else if (type == av1::WM_TYPE_ROT_ZOOM) {
        g.matrix[4] = -g.matrix[3];
        g.matrix[5] = g.matrix[2];
      }
      // (the range the syntax can give: 7.10.2.12, decode_signed_subexp_with_ref)
      if (iabs(g.matrix[0]) > (1 << 22) || iabs(g.matrix[1]) > (1 << 22) || iabs(g.matrix[2] - (1 << 16)) > (1 << 13) || iabs(g.matrix[3]) > (1 << 13) ||
          iabs(g.matrix[4]) > (1 << 13) || iabs(g.matrix[5] - (1 << 16)) > (1 << 13))
        return DxvaResult::Invalid;
    }
  }

  // film grain (the parameters of the frame, as the program's parser loaded them)
  const auto& fg = pp.film_grain;
  hdr.film_grain.present = bit(fg.ControlFlags, 0);
  if (hdr.film_grain.present) {
    hdr.film_grain.update = 1;
    av1::FilmGrainData& d = hdr.film_grain.data;
    d.seed = fg.grain_seed;
    if (fg.num_y_points > 14 || fg.num_cb_points > 10 || fg.num_cr_points > 10) return DxvaResult::Invalid;
    d.num_y_points = fg.num_y_points;
    for (int i = 0; i < d.num_y_points; ++i) {
      d.y_points[i][0] = fg.scaling_points_y[i][0];
      d.y_points[i][1] = fg.scaling_points_y[i][1];
      if (i && d.y_points[i - 1][0] >= d.y_points[i][0]) return DxvaResult::Invalid;
    }
    d.chroma_scaling_from_luma = mono ? 0 : bit(fg.ControlFlags, 3);
    if (mono || d.chroma_scaling_from_luma || !d.num_y_points) {
      d.num_uv_points[0] = d.num_uv_points[1] = 0;
    } else {
      d.num_uv_points[0] = fg.num_cb_points;
      d.num_uv_points[1] = fg.num_cr_points;
      for (int pl = 0; pl < 2; ++pl)
        for (int i = 0; i < d.num_uv_points[pl]; ++i) {
          const uint8_t(*points)[2] = pl ? fg.scaling_points_cr : fg.scaling_points_cb;
          d.uv_points[pl][i][0] = points[i][0];
          d.uv_points[pl][i][1] = points[i][1];
          if (i && d.uv_points[pl][i - 1][0] >= d.uv_points[pl][i][0]) return DxvaResult::Invalid;
        }
      if (!!d.num_uv_points[0] != !!d.num_uv_points[1]) return DxvaResult::Invalid;
    }
    d.scaling_shift = static_cast<int>(bits(fg.ControlFlags, 1, 2)) + 8;
    d.ar_coeff_lag = static_cast<int>(bits(fg.ControlFlags, 4, 2));
    const int num_y_pos = 2 * d.ar_coeff_lag * (d.ar_coeff_lag + 1);
    if (d.num_y_points)
      for (int i = 0; i < num_y_pos; ++i) d.ar_coeffs_y[i] = static_cast<int8_t>(static_cast<int>(fg.ar_coeffs_y[i]) - 128);
    for (int pl = 0; pl < 2; ++pl)
      if (d.num_uv_points[pl] || d.chroma_scaling_from_luma) {
        const int num_uv_pos = num_y_pos + (d.num_y_points ? 1 : 0);
        const uint8_t* coeffs = pl ? fg.ar_coeffs_cr : fg.ar_coeffs_cb;
        for (int i = 0; i < num_uv_pos; ++i) d.ar_coeffs_uv[pl][i] = static_cast<int8_t>(static_cast<int>(coeffs[i]) - 128);
        if (!d.num_y_points) d.ar_coeffs_uv[pl][num_uv_pos] = 0;
      }
    d.ar_coeff_shift = bits(fg.ControlFlags, 6, 2) + 6;
    d.grain_scale_shift = static_cast<int>(bits(fg.ControlFlags, 8, 2));
    for (int pl = 0; pl < 2; ++pl)
      if (d.num_uv_points[pl]) {
        d.uv_mult[pl] = static_cast<int>(pl ? fg.cr_mult : fg.cb_mult) - 128;
        d.uv_luma_mult[pl] = static_cast<int>(pl ? fg.cr_luma_mult : fg.cb_luma_mult) - 128;
        const int offset = pl ? fg.cr_offset : fg.cb_offset;
        if (offset < 0 || offset > 511) return DxvaResult::Invalid;
        d.uv_offset[pl] = offset - 256;
      }
    d.overlap_flag = bit(fg.ControlFlags, 10);
    d.clip_to_restricted_range = bit(fg.ControlFlags, 11);
  }
  *seq_out = seq;
  *hdr_out = hdr;
  *why = nullptr;
  return DxvaResult::Ok;
}

DxvaResult Av1DxvaDecoder::collect_tiles(const DxvaPicture& picture, const FrameHeader& hdr, const char** why) {
  constexpr uint64_t kControl = sizeof(dxva::TileAv1);
  const uint64_t count = static_cast<uint64_t>(hdr.tiling.cols) * hdr.tiling.rows;
  *why = "AV1: the tile controls do not describe every tile of the frame once, inside the bitstream";
  if (picture.slice_control.size() != count * kControl || picture.bitstream.empty()) return DxvaResult::Invalid;
  if (!tiles_.reset(count)) {
    *why = "AV1: out of memory";
    return DxvaResult::Invalid;
  }
  for (uint64_t i = 0; i < count; ++i) {
    ByteReader in(picture.slice_control.subview(i * kControl, kControl));
    const uint32_t offset = in.le32(), size = in.le32();
    const uint16_t row = in.le16(), column = in.le16();
    in.skip(2);
    const uint8_t anchor = in.u8();
    if (!in.ok()) return DxvaResult::Invalid;
    if (anchor != 0xFF) {
      *why = "AV1: large-scale tile decoding (anchor frames) is not supported";
      return DxvaResult::Unsupported;
    }
    ConstByteView data;
    if (row >= hdr.tiling.rows || column >= hdr.tiling.cols || size == 0 || !picture.bitstream.try_subview(offset, size, &data)) return DxvaResult::Invalid;
    TileData& t = tiles_[static_cast<uint64_t>(row) * hdr.tiling.cols + column];
    if (!t.data.empty()) return DxvaResult::Invalid;   // the same tile twice
    t.data = data;
    t.tile_row = row;
    t.tile_col = column;
  }
  *why = nullptr;
  return DxvaResult::Ok;
}

bool Av1DxvaDecoder::grey_reference(const SequenceHeader& seq, const FrameHeader& hdr, int width, int height, RefSlot* slot) {
  const int bpc = 8 + 2 * seq.hbd;
  SharedPointer<Picture> pic = SharedPointer<Picture>::make(allocator_, allocator_);
  if (!pic || !pic->allocate(seq.layout, bpc, width, height)) return false;
  for (int p = 0; p < (seq.layout == av1::PIXEL_LAYOUT_I400 ? 1 : 3); ++p) pic->plane(p).plane.fill(static_cast<av1::Pixel>(1 << (bpc - 1)));
  pic->hdr = FrameHeader{};
  pic->hdr.width[0] = pic->hdr.width[1] = width;
  pic->hdr.height = height;
  pic->hdr.frame_offset = hdr.frame_offset;
  for (av1::WarpedMotionParams& g : pic->hdr.gmv) g = av1::default_wm_params();
  SharedPointer<av1::CdfContext> cdf = SharedPointer<av1::CdfContext>::make(allocator_);
  if (!cdf) return false;
  av1::cdf_load_default(*cdf, av1::cdf_qcat(hdr.quant.yac));
  slot->pic = pic;
  slot->cdf = cdf;
  slot->segmap.reset();
  slot->mvs.reset();
  return true;
}

bool Av1DxvaDecoder::output(const Picture& p, DxvaDecodedPicture* out) {
  const uint64_t w = static_cast<uint64_t>(p.width()), h = static_cast<uint64_t>(p.height());
  const uint64_t cw = (w + 1) / 2, ch = (h + 1) / 2;
  const bool deep = config_.output == PixelFormat::P010;
  const uint64_t bytes = deep ? 2 : 1;
  out_.clear();
  if (!out_.put_zeros((w * h + cw * ch * 2) * bytes)) return false;
  ByteWriter dst(out_.writable_view());
  const int shift = deep ? 16 - p.bpc() : 0;
  auto put = [&](uint32_t v) {
    if (deep) dst.le16(static_cast<uint16_t>(v << shift));
    else dst.u8(static_cast<uint8_t>(v));
  };
  const av1::PixelView y = p.plane(0);
  for (uint64_t row = 0; row < h; ++row)
    for (uint64_t x = 0; x < w; ++x) put(y.at(static_cast<int>(x), static_cast<int>(row)));
  const bool mono = p.layout() == av1::PIXEL_LAYOUT_I400;
  const uint32_t neutral = 1u << (p.bpc() - 1);
  const av1::PixelView u = p.plane(1), v = p.plane(2);
  for (uint64_t row = 0; row < ch; ++row)
    for (uint64_t x = 0; x < cw; ++x) {
      put(mono ? neutral : u.at(static_cast<int>(x), static_cast<int>(row)));
      put(mono ? neutral : v.at(static_cast<int>(x), static_cast<int>(row)));
    }
  if (!dst.ok()) return false;
  VideoFrame f;
  f.format = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), deep ? PixelFormat::P010 : PixelFormat::Nv12};
  const ConstByteView all = out_.view();
  f.plane[0] = all.data();
  f.plane[1] = all.subview(w * h * bytes).data();
  f.stride[0] = static_cast<uint32_t>(w * bytes);
  f.stride[1] = static_cast<uint32_t>(cw * 2 * bytes);
  f.keyframe = p.hdr.frame_type == av1::FRAME_TYPE_KEY;
  f.limited_range = true;
  out->frame = f;
  return true;
}

DxvaResult Av1DxvaDecoder::decode(const DxvaPicture& picture, DxvaDecodedPicture* out) {
  *out = DxvaDecodedPicture();
  dxva::PicParamsAv1 pp;
  {
    ByteReader in(picture.picture_parameters);
    if (!in.read_into(bytes_of(pp))) return refuse(out, DxvaResult::Invalid, "AV1: the picture parameter buffer is shorter than DXVA_PicParams_AV1");
  }
  const char* why = nullptr;
  SequenceHeader seq;
  FrameHeader hdr;
  const DxvaResult made = make_headers(pp, &seq, &hdr, &why);
  if (made != DxvaResult::Ok) return refuse(out, made, why);
  const DxvaResult tiled = collect_tiles(picture, hdr, &why);
  if (tiled != DxvaResult::Ok) return refuse(out, tiled, why);

  // The surfaces: the current one and the eight reference slots.
  uint32_t cur_key = 0;
  if (!dxva::output_surface_key(picture, config_.surfaces, pp.CurrPicTextureIndex, &cur_key))
    return refuse(out, DxvaResult::Invalid, "AV1: CurrPicTextureIndex names no surface");
  uint32_t keep[1 + av1::NUM_REF_FRAMES];
  uint32_t keep_count = 0;
  keep[keep_count++] = cur_key;
  RefSlot slots[av1::NUM_REF_FRAMES];
  bool slot_named[av1::NUM_REF_FRAMES] = {};
  for (int i = 0; i < av1::NUM_REF_FRAMES; ++i) {
    const uint8_t index = pp.RefFrameMapTextureIndex[i];
    if (index == kNoSlot) continue;
    uint32_t key = 0;
    if (!dxva::surface_key(picture, config_.surfaces, index, &key)) return refuse(out, DxvaResult::Invalid, "AV1: RefFrameMapTextureIndex names no surface");
    slot_named[i] = true;
    keep[keep_count++] = key;
    if (const Av1Surface* s = surfaces_.find(key)) {
      slots[i] = s->state;
      surfaces_.touch(key);
    }
  }
  // An inter frame needs its seven references (and the primary one's CDFs); what is not there is a grey picture of the size frame_refs gives.
  bool missing = false;
  const bool inter = av1::is_inter_or_switch(hdr);
  const int bpc = 8 + 2 * seq.hbd;
  if (inter) {
    for (int i = 0; i < av1::REFS_PER_FRAME; ++i) {
      RefSlot& slot = slots[hdr.refidx[i]];
      const Picture* rp = slot.pic.get();
      const bool usable = rp && rp->layout() == seq.layout && rp->bpc() == bpc && hdr.width[0] * 2 >= rp->width() && hdr.height * 2 >= rp->height() &&
                          hdr.width[0] <= rp->width() * 16 && hdr.height <= rp->height() * 16 && slot.cdf;
      if (usable) continue;
      if (rp && slot_named[hdr.refidx[i]] && slot.cdf) {
        // There, but not a picture this frame can use (format or scale): the stream and the surfaces do not belong together.
        return refuse(out, DxvaResult::Invalid, "AV1: a reference frame has another format, or a size it cannot be scaled from");
      }
      missing = true;
      int w = static_cast<int>(pp.frame_refs[i].width), h = static_cast<int>(pp.frame_refs[i].height);
      if (w <= 0 || h <= 0 || w > 65536 || h > 65536 || hdr.width[0] * 2 < w || hdr.height * 2 < h || hdr.width[0] > w * 16 || hdr.height > h * 16) {
        w = hdr.width[1];
        h = hdr.height;
      }
      if (!grey_reference(seq, hdr, w, h, &slot)) return refuse(out, DxvaResult::Invalid, "AV1: out of memory");
    }
    // The skip mode references (5.9.22), from the order hints of the references - as the parser derives them.
    if (hdr.switchable_comp_refs && seq.order_hint) {
      const int poc = hdr.frame_offset;
      int off_before = -1, off_after = -1, off_before_idx = 0, off_after_idx = 0;
      for (int i = 0; i < 7; i++) {
        const int refpoc = slots[hdr.refidx[i]].pic->hdr.frame_offset;
        const int diff = av1::get_poc_diff(seq.order_hint_n_bits, refpoc, poc);
        if (diff > 0) {
          if (off_after < 0 || av1::get_poc_diff(seq.order_hint_n_bits, off_after, refpoc) > 0) {
            off_after = refpoc;
            off_after_idx = i;
          }
        } else if (diff < 0 && (off_before < 0 || av1::get_poc_diff(seq.order_hint_n_bits, refpoc, off_before) > 0)) {
          off_before = refpoc;
          off_before_idx = i;
        }
      }
      if ((off_before | off_after) >= 0) {
        hdr.skip_mode_refs[0] = static_cast<int8_t>(imin(off_before_idx, off_after_idx));
        hdr.skip_mode_refs[1] = static_cast<int8_t>(imax(off_before_idx, off_after_idx));
        hdr.skip_mode_allowed = 1;
      } else if (off_before >= 0) {
        int off_before2 = -1, off_before2_idx = 0;
        for (int i = 0; i < 7; i++) {
          const int refpoc = slots[hdr.refidx[i]].pic->hdr.frame_offset;
          if (av1::get_poc_diff(seq.order_hint_n_bits, refpoc, off_before) < 0 &&
              (off_before2 < 0 || av1::get_poc_diff(seq.order_hint_n_bits, refpoc, off_before2) > 0)) {
            off_before2 = refpoc;
            off_before2_idx = i;
          }
        }
        if (off_before2 >= 0) {
          hdr.skip_mode_refs[0] = static_cast<int8_t>(imin(off_before_idx, off_before2_idx));
          hdr.skip_mode_refs[1] = static_cast<int8_t>(imax(off_before_idx, off_before2_idx));
          hdr.skip_mode_allowed = 1;
        }
      }
    }
    if (bit(pp.CodingParamToolFlags, dxva::av1_coding::kSkipMode)) {
      if (!hdr.skip_mode_allowed && !missing)
        return refuse(out, DxvaResult::Invalid, "AV1: skip mode with references that do not allow it (5.9.22)");
      hdr.skip_mode_enabled = hdr.skip_mode_allowed;
    }
  }

  RefSlot state;
  if (!core_->decode_frame_external(seq, hdr, ArrayView<const TileData>(tiles_.view()), ArrayView<const RefSlot>(slots), &state) || !state.pic) {
    // The frame is lost: its surface holds nothing a later frame can use.
    if (Av1Surface* s = surfaces_.find(cur_key)) s->state = RefSlot();
    return refuse(out, DxvaResult::Invalid, "AV1: the tile data is damaged");
  }
  bool fresh = false;
  Av1Surface* target = surfaces_.claim(cur_key, ArrayView<const uint32_t>(keep, keep_count), &fresh);
  if (!target) return refuse(out, DxvaResult::Invalid, "AV1: no room for the surface");
  target->state = state;
  // The output: with film grain applied when the parameters say so (the reference keeps the frame without it).
  const Picture* shown = core_->grained(*state.pic);
  if (!shown) return refuse(out, DxvaResult::Invalid, "AV1: out of memory");
  out->surface = pp.CurrPicTextureIndex;
  if (!output(*shown, out)) return refuse(out, DxvaResult::Invalid, "AV1: out of memory");
  return missing ? DxvaResult::MissingReference : DxvaResult::Ok;
}

}  // namespace

UniquePointer<IDxvaDecoder> create_dxva_av1(allocators::IAllocator& allocator, const DxvaDecoderConfig& config) {
  if (config.profile != DxvaProfile::Av1VldProfile0 || (config.output != PixelFormat::Nv12 && config.output != PixelFormat::P010)) return {};
  UniquePointer<Av1DxvaDecoder> decoder = UniquePointer<Av1DxvaDecoder>::make(allocator, allocator, config);
  if (!decoder || !decoder->init()) return {};
  return UniquePointer<IDxvaDecoder>(util::move(decoder));
}

}  // namespace hardware
