// DxvaDecoderTest: the AV1 cases (DxvaProfile::Av1VldProfile0, src/Hardware/VideoCodec/Dxva/DxvaAv1.cpp).
//
// The program: the kernel's own AV1 decoder (Av1Decoder, its OBU parser) parses each stream of tests/data/av1_*.ivf and keeps the eight reference slots, as ffmpeg's
// av1 decoder does for its hwaccel; its IOffload hook hands every frame over (sequence and frame header, tiles). The hook fills DXVA_PicParams_AV1 and DXVA_Tile_AV1
// as libavcodec/dxva2_av1.c does (the header fields in force, RefFrameMapTextureIndex from the slots, frame_refs[].Index the slot, the tile data alone, padded to 128
// bytes), decodes through create_dxva_decoder() and keeps the picture of every surface. The pictures the stream shows - shown frames and show_existing_frame - are
// compared with ffmpeg's decode (libdav1d, film grain applied; MD5 per picture, each at its own size). The surfaces are named three ways (DxvaNaming).
#include "DxvaDecoderTestRefs.inc"
#include "Hardware/VideoCodec/Dxva/DxvaTypesAv1.hpp"
#include "Hardware/VideoCodec/Software/Av1.hpp"
#include "Hardware/VideoCodec/Software/Av1Core.hpp"
#include "Hardware/VideoCodec/Software/Av1Warp.hpp"

namespace {

// ffmpeg's ff_dxva2_av1_fill_picture_parameters, from the parsed headers (dav1d's form, which keeps what CBS keeps in its raw header, derived values included).
dxva::PicParamsAv1 av1_fill_params(const av1::SequenceHeader& seq, const av1::FrameHeader& hdr, const av1::FrameHeader* slot_hdr, const bool* slot_valid,
                                   const uint8_t* slot_index, uint8_t cur_index) {
  dxva::PicParamsAv1 pp;
  memset(&pp, 0, sizeof pp);
  pp.width = static_cast<uint32_t>(hdr.width[1]);
  pp.height = static_cast<uint32_t>(hdr.height);
  pp.max_width = static_cast<uint32_t>(seq.max_width);
  pp.max_height = static_cast<uint32_t>(seq.max_height);
  pp.CurrPicTextureIndex = cur_index;
  pp.superres_denom = hdr.super_res.enabled ? hdr.super_res.width_scale_denominator : 8;
  pp.bitdepth = static_cast<uint8_t>(8 + 2 * seq.hbd);
  pp.seq_profile = seq.profile;
  pp.tiles.cols = hdr.tiling.cols;
  pp.tiles.rows = hdr.tiling.rows;
  pp.tiles.context_update_id = hdr.tiling.update;
  for (int i = 0; i < hdr.tiling.cols; ++i) pp.tiles.widths[i] = static_cast<uint16_t>(hdr.tiling.col_start_sb[i + 1] - hdr.tiling.col_start_sb[i]);
  for (int i = 0; i < hdr.tiling.rows; ++i) pp.tiles.heights[i] = static_cast<uint16_t>(hdr.tiling.row_start_sb[i + 1] - hdr.tiling.row_start_sb[i]);
  namespace c = dxva::av1_coding;
  auto set = [&](int position, unsigned v) { pp.CodingParamToolFlags |= (v & 1u) << position; };
  set(c::kUse128x128Superblock, seq.sb128);
  set(c::kIntraEdgeFilter, seq.intra_edge_filter);
  set(c::kInterintraCompound, seq.inter_intra);
  set(c::kMaskedCompound, seq.masked_compound);
  set(c::kWarpedMotion, hdr.warp_motion);
  set(c::kDualFilter, seq.dual_filter);
  set(c::kJntComp, seq.jnt_comp);
  set(c::kScreenContentTools, hdr.allow_screen_content_tools);
  set(c::kIntegerMv, hdr.force_integer_mv || !(hdr.frame_type & 1));
  set(c::kCdef, seq.cdef);
  set(c::kRestoration, seq.restoration);
  set(c::kFilmGrain, seq.film_grain_present);
  set(c::kIntrabc, hdr.allow_intrabc);
  set(c::kHighPrecisionMv, hdr.hp);
  set(c::kSwitchableMotionMode, hdr.switchable_motion_mode);
  set(c::kFilterIntra, seq.filter_intra);
  set(c::kDisableFrameEndUpdateCdf, !hdr.refresh_context);
  set(c::kDisableCdfUpdate, hdr.disable_cdf_update);
  set(c::kReferenceMode, hdr.switchable_comp_refs);
  set(c::kSkipMode, hdr.skip_mode_enabled);
  set(c::kReducedTxSet, hdr.reduced_txtp_set);
  set(c::kSuperres, hdr.super_res.enabled);
  pp.CodingParamToolFlags |= static_cast<uint32_t>(hdr.txfm_mode) << c::kTxMode;
  set(c::kUseRefFrameMvs, hdr.use_ref_frame_mvs);
  set(c::kEnableRefFrameMvs, seq.ref_frame_mvs);
  set(c::kReferenceFrameUpdate, 1);
  pp.FormatAndPictureInfoFlags = static_cast<uint8_t>(hdr.frame_type | (hdr.show_frame << 2) | (hdr.showable_frame << 3) | (seq.ss_hor << 4) | (seq.ss_ver << 5) |
                                                      (seq.monochrome << 6));
  pp.primary_ref_frame = hdr.primary_ref_frame;
  pp.order_hint = hdr.frame_offset;
  pp.order_hint_bits = seq.order_hint ? seq.order_hint_n_bits : 0;
  const bool inter = av1::is_inter_or_switch(hdr);
  for (int i = 0; i < av1::REFS_PER_FRAME; ++i) {
    dxva::PicEntryAv1& r = pp.frame_refs[i];
    const int idx = inter ? hdr.refidx[i] : 0;
    if (slot_valid[idx]) {
      r.width = static_cast<uint32_t>(slot_hdr[idx].width[1]);
      r.height = static_cast<uint32_t>(slot_hdr[idx].height);
      r.Index = static_cast<uint8_t>(idx);
    } else {
      r.Index = 0xFF;
    }
    av1::WarpedMotionParams g = hdr.gmv[i];
    const int invalid = g.type > av1::WM_TYPE_TRANSLATION ? av1::get_shear_params(&g) : 0;
    r.GlobalMotionFlags = static_cast<uint8_t>((invalid ? 1 : 0) | (hdr.gmv[i].type << 1));
    for (int k = 0; k < 6; ++k) r.wmmat[k] = hdr.gmv[i].matrix[k];
  }
  for (int i = 0; i < av1::NUM_REF_FRAMES; ++i) pp.RefFrameMapTextureIndex[i] = slot_valid[i] ? slot_index[i] : 0xFF;

  auto& lf = pp.loop_filter;
  lf.filter_level[0] = hdr.loopfilter.level_y[0];
  lf.filter_level[1] = hdr.loopfilter.level_y[1];
  lf.filter_level_u = hdr.loopfilter.level_u;
  lf.filter_level_v = hdr.loopfilter.level_v;
  lf.sharpness_level = hdr.loopfilter.sharpness;
  lf.ControlFlags = static_cast<uint8_t>(hdr.loopfilter.mode_ref_delta_enabled | (hdr.loopfilter.mode_ref_delta_update << 1) | (hdr.delta.lf.multi << 2) |
                                         (hdr.delta.lf.present << 3));
  lf.delta_lf_res = hdr.delta.lf.res_log2;
  for (int i = 0; i < 8; ++i) lf.ref_deltas[i] = hdr.loopfilter.mode_ref_deltas.ref_delta[i];
  for (int i = 0; i < 2; ++i) lf.mode_deltas[i] = hdr.loopfilter.mode_ref_deltas.mode_delta[i];
  static const uint8_t kToDxva[4] = {0, 3, 1, 2};   // dav1d's NONE, SWITCHABLE, WIENER, SGRPROJ -> DXVA's 0 none, 1 Wiener, 2 self-guided, 3 switchable
  bool uses_lr = false;
  for (int p = 0; p < 3; ++p) {
    lf.frame_restoration_type[p] = kToDxva[hdr.restoration.type[p]];
    uses_lr = uses_lr || hdr.restoration.type[p] != av1::RESTORATION_NONE;
  }
  lf.log2_restoration_unit_size[0] = uses_lr ? hdr.restoration.unit_size[0] : 8;
  lf.log2_restoration_unit_size[1] = lf.log2_restoration_unit_size[2] = uses_lr ? hdr.restoration.unit_size[1] : 8;

  auto& q = pp.quantization;
  q.ControlFlags = static_cast<uint8_t>(hdr.delta.q.present | (hdr.delta.q.res_log2 << 1));
  q.base_qindex = hdr.quant.yac;
  q.y_dc_delta_q = hdr.quant.ydc_delta;
  q.u_dc_delta_q = hdr.quant.udc_delta;
  q.v_dc_delta_q = hdr.quant.vdc_delta;
  q.u_ac_delta_q = hdr.quant.uac_delta;
  q.v_ac_delta_q = hdr.quant.vac_delta;
  q.qm_y = hdr.quant.qm ? hdr.quant.qm_y : 0xFF;
  q.qm_u = hdr.quant.qm ? hdr.quant.qm_u : 0xFF;
  q.qm_v = hdr.quant.qm ? hdr.quant.qm_v : 0xFF;

  pp.cdef.ControlFlags = static_cast<uint8_t>((hdr.cdef.damping ? hdr.cdef.damping - 3 : 0) | (hdr.cdef.n_bits << 2));
  for (int i = 0; i < 8; ++i) {
    pp.cdef.y_strengths[i] = static_cast<uint8_t>((hdr.cdef.y_strength[i] >> 2) | ((hdr.cdef.y_strength[i] & 3) << 6));
    pp.cdef.uv_strengths[i] = static_cast<uint8_t>((hdr.cdef.uv_strength[i] >> 2) | ((hdr.cdef.uv_strength[i] & 3) << 6));
  }
  pp.interp_filter = static_cast<uint8_t>(hdr.subpel_filter_mode);

  auto& sg = pp.segmentation;
  sg.ControlFlags = static_cast<uint8_t>(hdr.segmentation.enabled | (hdr.segmentation.update_map << 1) | (hdr.segmentation.update_data << 2) |
                                         (hdr.segmentation.temporal << 3));
  if (hdr.segmentation.enabled)
    for (int i = 0; i < 8; ++i) {
      const av1::SegmentationData& d = hdr.segmentation.seg_data.d[i];
      sg.feature_mask[i] = d.feature_mask;
      const int values[8] = {d.delta_q, d.delta_lf_y_v, d.delta_lf_y_h, d.delta_lf_u, d.delta_lf_v, d.ref < 0 ? 0 : d.ref, 0, 0};
      for (int j = 0; j < 8; ++j) sg.feature_data[i][j] = static_cast<int16_t>((d.feature_mask >> j) & 1 ? values[j] : 0);
    }

  if (hdr.film_grain.present) {
    const av1::FilmGrainData& d = hdr.film_grain.data;
    auto& fg = pp.film_grain;
    fg.ControlFlags = static_cast<uint16_t>(1 | ((d.scaling_shift - 8) << 1) | (d.chroma_scaling_from_luma << 3) | (d.ar_coeff_lag << 4) |
                                            ((d.ar_coeff_shift - 6) << 6) | (d.grain_scale_shift << 8) | (d.overlap_flag << 10) |
                                            (d.clip_to_restricted_range << 11) | ((seq.mtrx == av1::MC_IDENTITY ? 1 : 0) << 12));
    fg.grain_seed = static_cast<uint16_t>(d.seed);
    fg.num_y_points = static_cast<uint8_t>(d.num_y_points);
    for (int i = 0; i < d.num_y_points; ++i) {
      fg.scaling_points_y[i][0] = d.y_points[i][0];
      fg.scaling_points_y[i][1] = d.y_points[i][1];
    }
    fg.num_cb_points = static_cast<uint8_t>(d.num_uv_points[0]);
    fg.num_cr_points = static_cast<uint8_t>(d.num_uv_points[1]);
    for (int i = 0; i < d.num_uv_points[0]; ++i) {
      fg.scaling_points_cb[i][0] = d.uv_points[0][i][0];
      fg.scaling_points_cb[i][1] = d.uv_points[0][i][1];
    }
    for (int i = 0; i < d.num_uv_points[1]; ++i) {
      fg.scaling_points_cr[i][0] = d.uv_points[1][i][0];
      fg.scaling_points_cr[i][1] = d.uv_points[1][i][1];
    }
    for (int i = 0; i < 24; ++i) fg.ar_coeffs_y[i] = static_cast<uint8_t>(d.ar_coeffs_y[i] + 128);
    for (int i = 0; i < 25; ++i) {
      fg.ar_coeffs_cb[i] = static_cast<uint8_t>(d.ar_coeffs_uv[0][i] + 128);
      fg.ar_coeffs_cr[i] = static_cast<uint8_t>(d.ar_coeffs_uv[1][i] + 128);
    }
    fg.cb_mult = static_cast<uint8_t>(d.uv_mult[0] + 128);
    fg.cb_luma_mult = static_cast<uint8_t>(d.uv_luma_mult[0] + 128);
    fg.cr_mult = static_cast<uint8_t>(d.uv_mult[1] + 128);
    fg.cr_luma_mult = static_cast<uint8_t>(d.uv_luma_mult[1] + 128);
    fg.cb_offset = static_cast<int16_t>(d.uv_offset[0] + 256);
    fg.cr_offset = static_cast<int16_t>(d.uv_offset[1] + 256);
  }
  return pp;
}

template <class T>
std::vector<uint8_t> av1_bytes(const T& v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  return std::vector<uint8_t>(p, p + sizeof v);
}

// The decoded picture as ffmpeg's framemd5 hashes it: planar in the stream's own layout and depth (yuv420p / yuv420p10le / yuv420p12le, yuv422p..., yuv444p..., gray /
// gray10le / gray12le for monochrome), unpacked from the surface format the decoder wrote (NV12 / P010 / P016, AYUV, Y410, Y416, YUY2, Y210, Y216) by the layouts DXGI
// gives them - so the packing is checked along with the pixels.
std::vector<uint8_t> av1_ffmpeg_layout(const DxvaDecodedPicture& d, const av1::SequenceHeader& seq) {
  const VideoFrame& f = d.frame;
  std::vector<uint8_t> out;
  const uint32_t w = f.format.width, h = f.format.height;
  const int bpc = 8 + 2 * seq.hbd;
  const bool mono = seq.monochrome, deep = bpc > 8;
  const uint32_t shift = deep ? 16 - bpc : 0;
  const uint32_t cw = seq.ss_hor ? (w + 1) / 2 : w, ch = seq.ss_ver ? (h + 1) / 2 : h;
  auto le16 = [](const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); };
  auto put = [&](uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    if (deep) out.push_back(static_cast<uint8_t>(v >> 8));
  };
  // sample(plane, x, y): plane 0 Y, 1 U, 2 V, in their own resolution
  auto sample = [&](int plane, uint32_t x, uint32_t y) -> uint32_t {
    switch (d.surface_format) {
      case DxvaSurfaceFormat::Nv12:
      case DxvaSurfaceFormat::P010:
      case DxvaSurfaceFormat::P016: {
        const uint8_t* row = f.plane[plane ? 1 : 0] + size_t(y) * f.stride[plane ? 1 : 0];
        const uint32_t i = plane ? 2 * x + (plane - 1) : x;
        return deep ? le16(row + 2 * i) >> shift : row[i];
      }
      case DxvaSurfaceFormat::Ayuv: return f.plane[0][size_t(y) * f.stride[0] + 4 * x + (plane == 0 ? 2 : plane == 1 ? 1 : 0)];
      case DxvaSurfaceFormat::Y410: {
        const uint8_t* p = f.plane[0] + size_t(y) * f.stride[0] + 4 * x;
        const uint32_t word = le16(p) | le16(p + 2) << 16;
        return (word >> (plane == 0 ? 10 : plane == 1 ? 0 : 20)) & 0x3FF;
      }
      case DxvaSurfaceFormat::Y416: return le16(f.plane[0] + size_t(y) * f.stride[0] + 8 * x + 2 * (plane == 0 ? 1 : plane == 1 ? 0 : 2)) >> shift;
      case DxvaSurfaceFormat::Yuy2: return f.plane[0][size_t(y) * f.stride[0] + (plane == 0 ? 2 * x : 4 * x + (plane == 1 ? 1 : 3))];
      case DxvaSurfaceFormat::Y210:
      case DxvaSurfaceFormat::Y216: return le16(f.plane[0] + size_t(y) * f.stride[0] + (plane == 0 ? 4 * x : 8 * x + (plane == 1 ? 2 : 6))) >> shift;
    }
    return 0;
  };
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) put(sample(0, x, y));
  if (!mono)
    for (int c = 1; c <= 2; ++c)
      for (uint32_t y = 0; y < ch; ++y)
        for (uint32_t x = 0; x < cw; ++x) put(sample(c, x, y));
  return out;
}

// What a packed surface holds besides the samples: AYUV / Y410 / Y416 an opaque alpha, the 16-bit words of 10- and 12-bit samples zeros below the value; and an NV12 /
// P010 / P016 picture of a monochrome stream neutral chroma. The count of samples that are not so.
int av1_surface_extras_bad(const DxvaDecodedPicture& d, const av1::SequenceHeader& seq) {
  const VideoFrame& f = d.frame;
  const uint32_t w = f.format.width, h = f.format.height;
  const int bpc = 8 + 2 * seq.hbd;
  const uint32_t low = bpc > 8 ? (1u << (16 - bpc)) - 1 : 0;
  auto le16 = [](const uint8_t* p) { return static_cast<uint32_t>(p[0] | (p[1] << 8)); };
  int bad = 0;
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* row = f.plane[0] + size_t(y) * f.stride[0];
    switch (d.surface_format) {
      case DxvaSurfaceFormat::Ayuv: for (uint32_t x = 0; x < w; ++x) bad += row[4 * x + 3] != 0xFF; break;
      case DxvaSurfaceFormat::Y410: for (uint32_t x = 0; x < w; ++x) bad += (row[4 * x + 3] >> 6) != 3; break;
      case DxvaSurfaceFormat::Y416:
        for (uint32_t x = 0; x < w; ++x) {
          bad += le16(row + 8 * x + 6) != 0xFFFF;
          for (int c = 0; c < 3; ++c) bad += (le16(row + 8 * x + 2 * c) & low) != 0;
        }
        break;
      case DxvaSurfaceFormat::Y210:
      case DxvaSurfaceFormat::Y216:
        for (uint32_t x = 0; x < (w + 1) / 2 * 4; ++x) bad += (le16(row + 2 * x) & low) != 0;
        break;
      default: break;
    }
  }
  if (seq.monochrome && !dxva_surface_packed(d.surface_format)) {
    const uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    const bool deep = d.surface_format != DxvaSurfaceFormat::Nv12;
    for (uint32_t y = 0; y < ch; ++y)
      for (uint32_t x = 0; x < 2 * cw; ++x) {
        const uint8_t* row = f.plane[1] + size_t(y) * f.stride[1];
        const uint32_t v = deep ? le16(row + 2 * x) : row[x];
        if (v != (deep ? (1u << (bpc - 1)) << (16 - bpc) : 128u)) ++bad;
      }
  }
  return bad;
}

// The decoder profile and surface format Windows uses for a stream (what ffmpeg's dxva2 picks): profile 0 NV12 / P010; profile 1 AYUV / Y410; profile 2 YUY2 / Y210
// (4:2:2), NV12 / P010 (monochrome), and at 12 bits Y216 / Y416 (12BIT_PROFILE2) or P016 (12BIT_PROFILE2_420).
void av1_decoder_for(const av1::SequenceHeader& seq, bool force_profile0, DxvaProfile* profile, DxvaSurfaceFormat* surface) {
  const bool i444 = !seq.ss_hor && !seq.ss_ver, i422 = seq.ss_hor && !seq.ss_ver;
  using S = DxvaSurfaceFormat;
  if (force_profile0 || seq.profile == 0) {
    *profile = DxvaProfile::Av1VldProfile0;
    *surface = seq.hbd ? S::P010 : S::Nv12;
  } else if (seq.profile == 1) {
    *profile = DxvaProfile::Av1VldProfile1;
    *surface = seq.hbd ? S::Y410 : S::Ayuv;
  } else if (seq.hbd == 2) {
    *profile = i444 || i422 ? DxvaProfile::Av1Vld12BitProfile2 : DxvaProfile::Av1Vld12BitProfile2_420;
    *surface = i444 ? S::Y416 : i422 ? S::Y216 : S::P016;
  } else {
    *profile = DxvaProfile::Av1VldProfile2;
    *surface = i422 ? (seq.hbd ? S::Y210 : S::Yuy2) : (seq.hbd ? S::P010 : S::Nv12);
  }
}

struct Av1DxvaApp final : av1::IOffload {
  TestAllocator* alloc = nullptr;
  DxvaNaming naming = DxvaNaming::D3d11;
  UniquePointer<IDxvaDecoder> decoder;
  bool slot_valid[8] = {};
  uint32_t slot_key[8] = {};
  av1::FrameHeader slot_hdr[8];
  std::map<uint32_t, std::vector<uint8_t>> picture_of_key;   // the shown form of every surface's picture
  std::vector<std::vector<uint8_t>> shown;                   // what the stream shows, in order
  uint32_t next_key = 5000;
  std::mt19937 rng{11};
  int frames = 0, failures = 0, mono_chroma_bad = 0;
  bool force_profile0 = false;   // a profile 0 decoder whatever the stream (what it refuses)
  DxvaProfile used_profile = DxvaProfile::Av1VldProfile0;
  DxvaSurfaceFormat used_surface = DxvaSurfaceFormat::Nv12;
  DxvaResult first_result = DxvaResult::Ok;
  std::vector<DxvaRecordedCall>* record = nullptr;
  // What the stream does, in order, for a program to replay (av1_dump_stream): a decode (the call's number) shown at once or not, or a surface shown again.
  struct Event { int call; int shown; uint32_t surface; };
  std::vector<Event>* events = nullptr;
  static constexpr uint32_t kSurfaces = 10;   // 8 slots, the current frame and one more

  bool accepts(const av1::SequenceHeader&, const av1::FrameHeader&) override { return true; }

  bool decode_frame(const av1::OffloadFrame& f) override {
    const av1::SequenceHeader& seq = *f.seq;
    const av1::FrameHeader& hdr = *f.hdr;
    if (!decoder) {
      DxvaDecoderConfig cfg;
      av1_decoder_for(seq, force_profile0, &cfg.profile, &cfg.surface);
      used_profile = cfg.profile;
      used_surface = cfg.surface;
      cfg.width = static_cast<uint32_t>(seq.max_width);
      cfg.height = static_cast<uint32_t>(seq.max_height);
      cfg.surfaces = naming == DxvaNaming::D3d11 ? kSurfaces : 16;
      decoder = create_dxva_decoder(*alloc, cfg);
      CHECK(decoder);
      if (!decoder) return false;
    }
    DxvaRecordedCall call;
    uint32_t cur_key = 0;
    uint8_t slot_index[8] = {};
    uint8_t cur_index = 0;
    if (naming == DxvaNaming::D3d11) {
      // A surface no slot holds (the program's pool).
      for (cur_key = 0; cur_key < kSurfaces; ++cur_key) {
        bool held = false;
        for (int i = 0; i < 8; ++i) held = held || (slot_valid[i] && slot_key[i] == cur_key);
        if (!held) break;
      }
      for (int i = 0; i < 8; ++i) slot_index[i] = static_cast<uint8_t>(slot_key[i]);
      cur_index = static_cast<uint8_t>(cur_key);
    } else {
      cur_key = next_key++;
      std::vector<uint32_t> keys;
      for (int i = 0; i < 8; ++i)
        if (slot_valid[i] && std::find(keys.begin(), keys.end(), slot_key[i]) == keys.end()) keys.push_back(slot_key[i]);
      if (naming == DxvaNaming::D3d12Shuffled) keys.push_back(cur_key);
      std::shuffle(keys.begin(), keys.end(), rng);
      auto position = [&](uint32_t key) { return static_cast<uint8_t>(std::find(keys.begin(), keys.end(), key) - keys.begin()); };
      for (int i = 0; i < 8; ++i) slot_index[i] = slot_valid[i] ? position(slot_key[i]) : 0xFF;
      if (naming == DxvaNaming::D3d12Shuffled) {
        cur_index = position(cur_key);
      } else {
        cur_index = static_cast<uint8_t>(keys.size() + 7);
        call.output_key = cur_key;
      }
      call.keys = keys;
    }
    call.params = av1_bytes(av1_fill_params(seq, hdr, slot_hdr, slot_valid, slot_index, cur_index));
    for (const av1::TileData& t : f.tiles) {
      dxva::TileAv1 c;
      memset(&c, 0, sizeof c);
      c.DataOffset = static_cast<uint32_t>(call.bitstream.size());
      c.DataSize = static_cast<uint32_t>(t.data.size());
      c.row = t.tile_row;
      c.column = t.tile_col;
      c.anchor_frame = 0xFF;
      call.bitstream.insert(call.bitstream.end(), t.data.data(), t.data.data() + t.data.size());
      const std::vector<uint8_t> b = av1_bytes(c);
      call.slices.insert(call.slices.end(), b.begin(), b.end());
    }
    call.bitstream.resize(call.bitstream.size() + (128 - (call.bitstream.size() & 127)), 0);
    DxvaDecodedPicture out;
    const DxvaResult r = decoder->decode(call.picture(), &out);
    if (frames == 0) first_result = r;
    ++frames;
    if (record) record->push_back(call);
    if (events) events->push_back({static_cast<int>(record ? record->size() - 1 : 0), hdr.show_frame ? 1 : 0, cur_index});
    if (r != DxvaResult::Ok) {
      if (first_result == DxvaResult::Ok) printf("  AV1 frame %d: result %d (%s)\n", frames, static_cast<int>(r), out.detail ? out.detail : "");
      ++failures;
      return false;
    }
    CHECK(out.surface == cur_index);
    CHECK(out.frame.format.width == static_cast<uint32_t>(hdr.width[1]) && out.frame.format.height == static_cast<uint32_t>(hdr.height));
    CHECK(out.surface_format == used_surface);
    mono_chroma_bad += av1_surface_extras_bad(out, seq);
    picture_of_key[cur_key] = av1_ffmpeg_layout(out, seq);
    if (hdr.show_frame) shown.push_back(picture_of_key[cur_key]);
    for (int i = 0; i < 8; ++i)
      if (hdr.refresh_frame_flags & (1 << i)) {
        slot_valid[i] = true;
        slot_key[i] = cur_key;
        slot_hdr[i] = hdr;
      }
    return true;
  }
  void show_existing(unsigned idx, bool refresh_all) override {
    if (idx >= 8 || !slot_valid[idx]) return;
    if (events) events->push_back({-1, 1, slot_key[idx]});
    shown.push_back(picture_of_key[slot_key[idx]]);
    if (refresh_all)
      for (int i = 0; i < 8; ++i) {
        slot_valid[i] = true;
        slot_key[i] = slot_key[idx];
        slot_hdr[i] = slot_hdr[idx];
      }
  }
  void new_sequence() override {
    for (bool& v : slot_valid) v = false;
  }
};

// The frames of an IVF file.
std::vector<std::pair<size_t, size_t>> av1_ivf_frames(const std::vector<uint8_t>& d) {
  std::vector<std::pair<size_t, size_t>> frames;
  if (d.size() < 32) return frames;
  const size_t header = d[6] | (d[7] << 8);
  size_t pos = header;
  while (pos + 12 <= d.size()) {
    const size_t size = d[pos] | (d[pos + 1] << 8) | (d[pos + 2] << 16) | (size_t(d[pos + 3]) << 24);
    pos += 12;
    if (size > d.size() - pos) break;
    frames.push_back({pos, size});
    pos += size;
  }
  return frames;
}

struct Av1Run {
  int matched = 0, total = 0, frames = 0, failures = 0;
  DxvaResult first = DxvaResult::Ok;
};

Av1Run dxva_av1_stream(const std::string& name, DxvaNaming naming, std::vector<DxvaRecordedCall>* record, bool compare = true, bool force_profile0 = false,
                       std::vector<Av1DxvaApp::Event>* events = nullptr) {
  TestAllocator alloc;
  Av1Run run;
  const std::vector<uint8_t> data = read_file("tests/data/" + name);
  const std::vector<std::string> refs = compare ? dxva_reference_md5(name) : std::vector<std::string>();
  CHECK(!data.empty() && (!compare || !refs.empty()));
  {
    Av1DxvaApp app;
    app.alloc = &alloc;
    app.naming = naming;
    app.record = record;
    app.force_profile0 = force_profile0;
    app.events = events;
    Av1Decoder dec(alloc);
    dec.set_offload(&app);
    for (const auto& fr : av1_ivf_frames(data)) (void)dec.decode(ConstByteView(data.data() + fr.first, fr.second), 0);
    run.frames = app.frames;
    run.failures = app.failures;
    run.first = app.first_result;
    CHECK(app.mono_chroma_bad == 0);
    if (compare) {
      run.total = static_cast<int>(refs.size());
      CHECK(app.shown.size() == refs.size());
      for (size_t i = 0; i < app.shown.size() && i < refs.size(); ++i) {
        if (dxva_md5_hex(app.shown[i]) == refs[i]) ++run.matched;
        else if (static_cast<int>(i) - run.matched < 3) printf("  %s %s: picture %zu differs from ffmpeg's\n", name.c_str(), dxva_naming_name(naming), i);
      }
      CHECK(run.failures == 0);
      CHECK(run.matched == run.total);
    }
  }
  CHECK(alloc.live == 0);
  return run;
}

UniquePointer<IDxvaDecoder> av1_make_decoder(TestAllocator& alloc, const DxvaRecordedCall& first, PixelFormat format,
                                             DxvaProfile profile = DxvaProfile::Av1VldProfile0, DxvaSurfaceFormat surface = DxvaSurfaceFormat::Nv12) {
  dxva::PicParamsAv1 pp;
  memcpy(&pp, first.params.data(), sizeof pp);
  DxvaDecoderConfig cfg;
  cfg.profile = profile;
  cfg.surface = surface;
  cfg.width = pp.max_width;
  cfg.height = pp.max_height;
  cfg.surfaces = Av1DxvaApp::kSurfaces;
  cfg.output = format;
  return create_dxva_decoder(alloc, cfg);
}

const char* const kAv1Profile12Streams[] = {"av1_444.ivf", "av1_444_10bit.ivf", "av1_422.ivf", "av1_422_10bit.ivf", "av1_422_12bit.ivf", "av1_444_12bit.ivf",
                                             "av1_12bit.ivf", "av1_mono_12bit.ivf"};

// What the profile 1 and 2 decoders refuse: a stream of another profile (Unsupported), a surface format that does not hold the frame's layout or depth, parameters that
// break 5.5.2's rules (Invalid); which formats each profile takes; damaged buffers.
void dxva_av1_profile12_errors() {
  using S = DxvaSurfaceFormat;
  // The formats of each profile (CheckVideoDecoderFormat answers these).
  CHECK(dxva_profile_supports(DxvaProfile::Av1VldProfile1, S::Ayuv, 64, 64) && dxva_profile_supports(DxvaProfile::Av1VldProfile1, S::Y410, 64, 64));
  CHECK(!dxva_profile_supports(DxvaProfile::Av1VldProfile1, S::Nv12, 64, 64) && !dxva_profile_supports(DxvaProfile::Av1VldProfile1, S::Y416, 64, 64));
  CHECK(dxva_profile_supports(DxvaProfile::Av1VldProfile2, S::Yuy2, 64, 64) && dxva_profile_supports(DxvaProfile::Av1VldProfile2, S::Y210, 64, 64));
  CHECK(dxva_profile_supports(DxvaProfile::Av1VldProfile2, S::Nv12, 64, 64) && !dxva_profile_supports(DxvaProfile::Av1VldProfile2, S::Y216, 64, 64));
  CHECK(dxva_profile_supports(DxvaProfile::Av1Vld12BitProfile2, S::Y216, 64, 64) && dxva_profile_supports(DxvaProfile::Av1Vld12BitProfile2, S::Y416, 64, 64));
  CHECK(!dxva_profile_supports(DxvaProfile::Av1Vld12BitProfile2, S::P016, 64, 64) && dxva_profile_supports(DxvaProfile::Av1Vld12BitProfile2_420, S::P016, 64, 64));
  CHECK(!dxva_profile_supports(DxvaProfile::Av1Vld12BitProfile2_420, S::P010, 64, 64) && !dxva_profile_supports(DxvaProfile::Av1VldProfile0, S::P016, 64, 64));
  CHECK(!dxva_profile_supports(DxvaProfile::Av1VldProfile0, S::Ayuv, 64, 64) && !dxva_profile_supports(DxvaProfile::HevcVldMain, S::Yuy2, 64, 64));
  CHECK(!dxva_profile_supports(DxvaProfile::Av1VldProfile1, S::Ayuv, 0, 64) && !dxva_profile_supports(DxvaProfile::Av1VldProfile1, S::Ayuv, 64, 20000));
  TestAllocator alloc;
  {
    DxvaDecoderConfig cfg;
    cfg.profile = DxvaProfile::Av1VldProfile1;
    cfg.width = cfg.height = 64;
    cfg.surfaces = 8;
    cfg.surface = S::Nv12;   // (not one of profile 1's)
    CHECK(!create_dxva_decoder(alloc, cfg));
    cfg.surface = S::Ayuv;
    CHECK(create_dxva_decoder(alloc, cfg) && create_dxva_decoder(alloc, cfg)->profile() == DxvaProfile::Av1VldProfile1);
  }
  int runs = 0;
  for (const char* name : {"av1_444_10bit.ivf", "av1_422_12bit.ivf", "av1_mono_12bit.ivf"}) {
    std::vector<DxvaRecordedCall> calls;
    const Av1Run r = dxva_av1_stream(name, DxvaNaming::D3d11, &calls);
    CHECK(r.matched == r.total && calls.size() >= 2);
    if (calls.size() < 2) continue;
    dxva::PicParamsAv1 pp;
    memcpy(&pp, calls[0].params.data(), sizeof pp);
    av1::SequenceHeader seq{};
    seq.profile = pp.seq_profile;
    seq.hbd = (pp.bitdepth - 8) / 2;
    seq.ss_hor = (pp.FormatAndPictureInfoFlags >> 4) & 1;
    seq.ss_ver = (pp.FormatAndPictureInfoFlags >> 5) & 1;
    seq.monochrome = (pp.FormatAndPictureInfoFlags >> 6) & 1;
    DxvaProfile profile;
    S surface;
    av1_decoder_for(seq, false, &profile, &surface);
    DxvaDecodedPicture o;
    {
      UniquePointer<IDxvaDecoder> d = av1_make_decoder(alloc, calls[0], PixelFormat::Nv12, profile, surface);
      CHECK(d && d->decode(calls[0].picture(), &o) == DxvaResult::Ok && o.surface_format == surface);
      // A surface of the profile that does not hold this frame (another depth or layout): Invalid.
      const S other = surface == S::Y410 ? S::Ayuv : surface == S::Y216 ? S::Y416 : S::P016;
      UniquePointer<IDxvaDecoder> wrong = av1_make_decoder(alloc, calls[0], PixelFormat::Nv12, profile, other);
      if (other != surface && wrong) CHECK(wrong->decode(calls[0].picture(), &o) == DxvaResult::Invalid && o.detail != nullptr);
      // Another profile's decoder: Unsupported.
      UniquePointer<IDxvaDecoder> p0 = av1_make_decoder(alloc, calls[0], PixelFormat::P010);
      CHECK(p0 && p0->decode(calls[0].picture(), &o) == DxvaResult::Unsupported);
      // Parameters against 5.5.2: the layout or depth a profile does not have, seq_profile 3, bitdepth 9, 4:4:0, monochrome 4:4:4.
      auto refused = [&](void (*damage)(dxva::PicParamsAv1&)) {
        dxva::PicParamsAv1 bad = pp;
        damage(bad);
        DxvaRecordedCall c = calls[0];
        c.params = av1_bytes(bad);
        d->reset();
        const DxvaResult res = d->decode(c.picture(), &o);
        return res == DxvaResult::Invalid || res == DxvaResult::Unsupported;
      };
      CHECK(refused([](dxva::PicParamsAv1& b) { b.seq_profile = 3; }));
      CHECK(refused([](dxva::PicParamsAv1& b) { b.bitdepth = 9; }));
      CHECK(refused([](dxva::PicParamsAv1& b) { b.FormatAndPictureInfoFlags = static_cast<uint8_t>((b.FormatAndPictureInfoFlags & ~0x70) | 0x20); }));   // 4:4:0
      CHECK(refused([](dxva::PicParamsAv1& b) { b.FormatAndPictureInfoFlags = static_cast<uint8_t>((b.FormatAndPictureInfoFlags & ~0x70) | 0x40); }));   // mono 4:4:4
      CHECK(refused([](dxva::PicParamsAv1& b) { b.bitdepth = b.bitdepth == 12 ? 10 : 12; }));
      // still decodes afterwards
      d->reset();
      CHECK(d->decode(calls[0].picture(), &o) == DxvaResult::Ok);
    }
    auto make = [&]() { return av1_make_decoder(alloc, calls[0], PixelFormat::Nv12, profile, surface); };
    runs += dxva_damage(make, {}, calls[0], 31);
    runs += dxva_damage(make, {calls[0]}, calls[1], 32);
  }
  CHECK(alloc.live == 0);
  printf("  AV1 profiles 1 and 2 damaged buffers: %d decodes\n", runs);
}

void dxva_av1_errors() {
  // What profile 0 does not take: 4:4:4, 4:2:2 (profiles 1 and 2), 12-bit.
  for (const char* s : {"av1_444.ivf", "av1_422.ivf", "av1_12bit.ivf"}) {
    const Av1Run r = dxva_av1_stream(s, DxvaNaming::D3d11, nullptr, false, true);
    CHECK(r.frames > 0 && r.first == DxvaResult::Unsupported);
  }
  dxva_av1_profile12_errors();
  std::vector<DxvaRecordedCall> calls;
  dxva_av1_stream("av1_inter.ivf", DxvaNaming::D3d11, &calls);
  CHECK(calls.size() >= 4);
  if (calls.size() < 4) return;
  TestAllocator alloc;
  {
    UniquePointer<IDxvaDecoder> d = av1_make_decoder(alloc, calls[0], PixelFormat::Nv12);
    DxvaDecodedPicture o;
    // An inter frame without its references: grey stand-ins, MissingReference, a picture all the same.
    CHECK(d->decode(calls[2].picture(), &o) == DxvaResult::MissingReference && o.frame.plane[0] != nullptr);
    d->reset();
    for (size_t i = 0; i < 3; ++i) CHECK(d->decode(calls[i].picture(), &o) == DxvaResult::Ok);
    d->reset();
    CHECK(d->decode(calls[2].picture(), &o) == DxvaResult::MissingReference);
    // The wrong output format for the bit depth.
    UniquePointer<IDxvaDecoder> d10 = av1_make_decoder(alloc, calls[0], PixelFormat::P010);
    CHECK(d10->decode(calls[0].picture(), &o) == DxvaResult::Invalid);

    d->reset();
    CHECK(d->decode(calls[0].picture(), &o) == DxvaResult::Ok);
    dxva::PicParamsAv1 pp;
    memcpy(&pp, calls[1].params.data(), sizeof pp);
    DxvaRecordedCall c = calls[1];
    dxva::PicParamsAv1 bad = pp;
    bad.CurrPicTextureIndex = 40;   // out of range
    c.params = av1_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    bad = pp;
    bad.RefFrameMapTextureIndex[3] = 200;
    c.params = av1_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    bad = pp;
    bad.frame_refs[2].Index = 9;
    c.params = av1_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    bad = pp;
    bad.tiles.widths[0] = static_cast<uint16_t>(bad.tiles.widths[0] + 1);   // the tiles do not cover the frame
    c.params = av1_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    bad = pp;
    bad.width = pp.max_width + 1;
    c.params = av1_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    // an index past the key list
    c = calls[1];
    c.keys = {0};
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    // Buffers missing or short.
    c = calls[1];
    c.params.resize(c.params.size() - 1);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    c = calls[1];
    c.slices.clear();
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    c = calls[1];
    c.bitstream.clear();
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    c = calls[1];
    c.bitstream.resize(4);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    // Large-scale tile decoding (anchor frames) is not offered.
    c = calls[1];
    dxva::TileAv1 t;
    memcpy(&t, c.slices.data(), sizeof t);
    t.anchor_frame = 0;
    memcpy(c.slices.data(), &t, sizeof t);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Unsupported && o.detail != nullptr);
  }
  CHECK(alloc.live == 0);
  int runs = 0;
  auto make = [&]() { return av1_make_decoder(alloc, calls[0], PixelFormat::Nv12); };
  runs += dxva_damage(make, {}, calls[0], 21);
  runs += dxva_damage(make, std::vector<DxvaRecordedCall>(calls.begin(), calls.begin() + 3), calls[3], 22);
  for (const char* s : {"av1_tiles.ivf", "av1_segments.ivf", "av1_grain.ivf", "av1_superres.ivf", "av1_screen.ivf"}) {
    std::vector<DxvaRecordedCall> more;
    dxva_av1_stream(s, DxvaNaming::D3d11, &more);
    if (more.size() < 2) continue;
    auto make_more = [&]() { return av1_make_decoder(alloc, more[0], PixelFormat::Nv12); };
    runs += dxva_damage(make_more, {more[0]}, more[1], 23);
  }
  CHECK(alloc.live == 0);
  printf("  AV1 damaged buffers: %d decodes\n", runs);
}

}  // namespace

// DXVA_AV1_DUMP=<folder>: writes, for the streams the Windows tests replay (tests/data/make_av1_dxva_data.py makes tests/windows/av1_dxva_data.h of them), the DXVA buffers
// of every picture as this test fills them - ffmpeg's dxva2_av1.c's way - with Direct3D 11's surface naming (index = array slice of 10), and the order the stream
// decodes and shows them in. A file of little-endian words: "AV1D", surfaces, width, height, seq_profile, bitdepth, layout (0 4:2:0, 1 4:2:2, 2 4:4:4, 3 monochrome),
// event count; per event: kind (0 decode, 1 show again), shown, surface, and for a decode the sizes of the picture parameters, tile controls and bitstream, then them.
static void av1_dump_stream(const char* folder, const char* name) {
  std::vector<DxvaRecordedCall> calls;
  std::vector<Av1DxvaApp::Event> events;
  dxva_av1_stream(name, DxvaNaming::D3d11, &calls, true, false, &events);
  if (calls.empty()) return;
  dxva::PicParamsAv1 pp;
  memcpy(&pp, calls[0].params.data(), sizeof pp);
  const uint32_t f = pp.FormatAndPictureInfoFlags;
  const uint32_t layout = (f >> 6) & 1 ? 3 : !((f >> 4) & 1) ? 2 : !((f >> 5) & 1) ? 1 : 0;
  std::vector<uint8_t> out;
  auto word = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i))); };
  auto bytes = [&](const std::vector<uint8_t>& b) { out.insert(out.end(), b.begin(), b.end()); };
  out.insert(out.end(), {'A', 'V', '1', 'D'});
  word(Av1DxvaApp::kSurfaces);
  word(pp.max_width);
  word(pp.max_height);
  word(pp.seq_profile);
  word(pp.bitdepth);
  word(layout);
  word(static_cast<uint32_t>(events.size()));
  for (const Av1DxvaApp::Event& e : events) {
    word(e.call < 0 ? 1 : 0);
    word(static_cast<uint32_t>(e.shown));
    word(e.surface);
    if (e.call < 0) continue;
    const DxvaRecordedCall& c = calls[static_cast<size_t>(e.call)];
    word(static_cast<uint32_t>(c.params.size()));
    word(static_cast<uint32_t>(c.slices.size()));
    word(static_cast<uint32_t>(c.bitstream.size()));
    bytes(c.params);
    bytes(c.slices);
    bytes(c.bitstream);
  }
  const std::string path = std::string(folder) + "/" + name + ".dxva";
  FILE* file = fopen(path.c_str(), "wb");
  CHECK(file != nullptr);
  if (!file) return;
  CHECK(fwrite(out.data(), 1, out.size(), file) == out.size());
  fclose(file);
  printf("  wrote %s (%zu events, %zu bytes)\n", path.c_str(), events.size(), out.size());
}

static void dxva_test_av1() {
  if (const char* dump = getenv("DXVA_AV1_DUMP")) {
    for (const char* s : {"av1_422_10bit.ivf", "av1_444.ivf", "av1_444_12bit.ivf"}) av1_dump_stream(dump, s);
    return;
  }
  static const char* kStreams[] = {"av1_intra.ivf",   "av1_inter.ivf",    "av1_tiles.ivf",  "av1_sb128.ivf", "av1_lossless.ivf", "av1_qm.ivf",
                                   "av1_segments.ivf", "av1_screen.ivf",   "av1_superres.ivf", "av1_resize.ivf", "av1_grain.ivf",  "av1_10bit.ivf",
                                   "av1_mono.ivf",     "av1_svt.ivf",      "av1_rav1e.ivf"};
  int matched = 0, total = 0, frames = 0;
  for (DxvaNaming naming : {DxvaNaming::D3d11, DxvaNaming::D3d12Shuffled, DxvaNaming::D3d12OutputKey})
    for (const char* s : kStreams) {
      const Av1Run r = dxva_av1_stream(s, naming, nullptr);
      matched += r.matched;
      total += r.total;
      frames += r.frames;
    }
  printf("DXVA AV1: %d of %d shown pictures bit-exact with ffmpeg/libdav1d (%d frames decoded, 15 streams, 3 ways of naming surfaces)\n", matched, total, frames);
  // Profiles 1 and 2: 4:4:4 (AYUV, Y410, Y416), 4:2:2 (YUY2, Y210 with film grain, Y216), 12-bit 4:2:0 and monochrome (P016).
  matched = total = frames = 0;
  for (DxvaNaming naming : {DxvaNaming::D3d11, DxvaNaming::D3d12Shuffled, DxvaNaming::D3d12OutputKey})
    for (const char* s : kAv1Profile12Streams) {
      const Av1Run r = dxva_av1_stream(s, naming, nullptr);
      matched += r.matched;
      total += r.total;
      frames += r.frames;
    }
  printf("DXVA AV1 profiles 1 and 2: %d of %d shown pictures bit-exact with ffmpeg/libdav1d (%d frames decoded, 8 streams, 3 ways of naming surfaces)\n", matched, total,
         frames);
  dxva_av1_errors();
}
