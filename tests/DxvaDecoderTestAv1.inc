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

// The decoded picture as ffmpeg's framemd5 hashes it: planar yuv420p / yuv420p10le, or gray / gray10le for monochrome.
std::vector<uint8_t> av1_ffmpeg_layout(const VideoFrame& f, bool mono) {
  std::vector<uint8_t> out;
  const uint32_t w = f.format.width, h = f.format.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
  const bool deep = f.format.pixel_format == PixelFormat::P010;
  auto sample = [&](int plane, uint32_t x, uint32_t y) -> uint32_t {
    const uint8_t* row = f.plane[plane] + size_t(y) * f.stride[plane];
    return deep ? static_cast<uint32_t>(row[2 * x] | (row[2 * x + 1] << 8)) >> 6 : row[x];
  };
  auto put = [&](uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    if (deep) out.push_back(static_cast<uint8_t>(v >> 8));
  };
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) put(sample(0, x, y));
  if (!mono)
    for (int c = 0; c < 2; ++c)
      for (uint32_t y = 0; y < ch; ++y)
        for (uint32_t x = 0; x < cw; ++x) put(sample(1, 2 * x + c, y));
  return out;
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
  DxvaResult first_result = DxvaResult::Ok;
  std::vector<DxvaRecordedCall>* record = nullptr;
  static constexpr uint32_t kSurfaces = 10;   // 8 slots, the current frame and one more

  bool accepts(const av1::SequenceHeader&, const av1::FrameHeader&) override { return true; }

  bool decode_frame(const av1::OffloadFrame& f) override {
    const av1::SequenceHeader& seq = *f.seq;
    const av1::FrameHeader& hdr = *f.hdr;
    if (!decoder) {
      DxvaDecoderConfig cfg;
      cfg.profile = DxvaProfile::Av1VldProfile0;
      cfg.width = static_cast<uint32_t>(seq.max_width);
      cfg.height = static_cast<uint32_t>(seq.max_height);
      cfg.surfaces = naming == DxvaNaming::D3d11 ? kSurfaces : 16;
      cfg.output = seq.hbd ? PixelFormat::P010 : PixelFormat::Nv12;
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
    if (r != DxvaResult::Ok) {
      if (first_result == DxvaResult::Ok) printf("  AV1 frame %d: result %d (%s)\n", frames, static_cast<int>(r), out.detail ? out.detail : "");
      ++failures;
      return false;
    }
    CHECK(out.surface == cur_index);
    CHECK(out.frame.format.width == static_cast<uint32_t>(hdr.width[1]) && out.frame.format.height == static_cast<uint32_t>(hdr.height));
    if (seq.monochrome) {
      // neutral chroma
      const uint32_t cw = (out.frame.format.width + 1) / 2, ch = (out.frame.format.height + 1) / 2;
      const bool deep = out.frame.format.pixel_format == PixelFormat::P010;
      for (uint32_t y = 0; y < ch; ++y)
        for (uint32_t x = 0; x < 2 * cw; ++x) {
          const uint8_t* row = out.frame.plane[1] + size_t(y) * out.frame.stride[1];
          const uint32_t v = deep ? static_cast<uint32_t>(row[2 * x] | (row[2 * x + 1] << 8)) : row[x];
          if (v != (deep ? 512u << 6 : 128u)) ++mono_chroma_bad;
        }
    }
    picture_of_key[cur_key] = av1_ffmpeg_layout(out.frame, seq.monochrome);
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

Av1Run dxva_av1_stream(const std::string& name, DxvaNaming naming, std::vector<DxvaRecordedCall>* record, bool compare = true) {
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

UniquePointer<IDxvaDecoder> av1_make_decoder(TestAllocator& alloc, const DxvaRecordedCall& first, PixelFormat format) {
  dxva::PicParamsAv1 pp;
  memcpy(&pp, first.params.data(), sizeof pp);
  DxvaDecoderConfig cfg;
  cfg.profile = DxvaProfile::Av1VldProfile0;
  cfg.width = pp.max_width;
  cfg.height = pp.max_height;
  cfg.surfaces = Av1DxvaApp::kSurfaces;
  cfg.output = format;
  return create_dxva_decoder(alloc, cfg);
}

void dxva_av1_errors() {
  // What profile 0 does not take: 4:4:4, 4:2:2 (profiles 1 and 2), 12-bit.
  for (const char* s : {"av1_444.ivf", "av1_422.ivf", "av1_12bit.ivf"}) {
    const Av1Run r = dxva_av1_stream(s, DxvaNaming::D3d11, nullptr, false);
    CHECK(r.frames > 0 && r.first == DxvaResult::Unsupported);
  }
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

static void dxva_test_av1() {
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
  dxva_av1_errors();
}
