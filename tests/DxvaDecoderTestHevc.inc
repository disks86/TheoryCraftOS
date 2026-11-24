// DxvaDecoderTest: the HEVC cases (DxvaProfile::HevcVldMain, src/Hardware/VideoCodec/Dxva/DxvaHevc.cpp).
//
// The program: the kernel's own H.265 decoder (H265Decoder) parses each stream of tests/data/hevc_*.h265 and keeps the decoded picture buffer, as ffmpeg's hevc
// decoder does for its hwaccel; its IOffload hook hands every picture over - parameter sets, slice headers, the reference picture sets as picture buffer slots, the
// slice NAL units. The hook fills DXVA_PicParams_HEVC, DXVA_Qmatrix_HEVC and DXVA_Slice_HEVC_Short exactly as libavcodec/dxva2_hevc.c does (RefPicList = every
// reference picture of the buffer, the three sets as indexes into it, the slices with three-byte start codes and the last one padded to 128 bytes), decodes through
// create_dxva_decoder() and writes the NV12 picture back into the H.265 decoder's picture, which outputs it in display order cropped: that is compared with ffmpeg's
// decode (MD5 per picture). The streams' own picture hashes (SEI) are checked as well. The surfaces are named three ways (DxvaNaming); the output must not change.
#include "DxvaDecoderTestRefs.inc"
#include "Hardware/VideoCodec/Dxva/DxvaTypesHevc.hpp"
#include "Hardware/VideoCodec/Software/H265.hpp"
#include "Hardware/VideoCodec/Software/H265Core.hpp"

namespace {

struct DxvaMd5PlaneHasher final : IPlaneHasher {
  void md5(const uint8_t* plane, uint32_t width, uint32_t height, uint32_t stride, uint8_t out[16]) override {
    crypto::Hasher h(crypto::HashKind::Md5);
    for (uint32_t y = 0; y < height; ++y) h.update(ConstByteView(plane + size_t(y) * stride, width));
    h.finish(out);
  }
};

void hevc_diag_scan(int n, uint8_t* raster) {
  int i = 0, x = 0, y = 0;
  while (i < n * n) {
    while (y >= 0) {
      if (x < n && y < n) raster[i++] = static_cast<uint8_t>(y * n + x);
      --y;
      ++x;
    }
    y = x;
    x = 0;
  }
}

// ffmpeg's ff_dxva2_hevc_fill_picture_parameters, from the parsed structures.
dxva::PicParamsHevc hevc_fill_params(const h265::OffloadPicture& o, const uint8_t* ref_index, uint8_t cur_index) {
  const h265::Sps& sps = *o.sps;
  const h265::Pps& pps = *o.pps;
  const h265::SliceHeader& sh = *o.first;
  dxva::PicParamsHevc pp;
  memset(&pp, 0, sizeof pp);
  pp.PicWidthInMinCbsY = static_cast<uint16_t>(sps.width >> sps.log2_min_cb);
  pp.PicHeightInMinCbsY = static_cast<uint16_t>(sps.height >> sps.log2_min_cb);
  pp.wFormatAndSequenceInfoFlags = static_cast<uint16_t>((sps.chroma_format_idc << 0) | ((sps.bit_depth_luma - 8) << 3) | ((sps.bit_depth_chroma - 8) << 6) |
                                                         ((sps.log2_max_poc_lsb - 4) << 9));
  pp.sps_max_dec_pic_buffering_minus1 = static_cast<uint8_t>(sps.max_dec_pic_buffering[sps.max_sub_layers - 1] - 1);
  pp.log2_min_luma_coding_block_size_minus3 = static_cast<uint8_t>(sps.log2_min_cb - 3);
  pp.log2_diff_max_min_luma_coding_block_size = static_cast<uint8_t>(sps.log2_ctb - sps.log2_min_cb);
  pp.log2_min_transform_block_size_minus2 = static_cast<uint8_t>(sps.log2_min_tb - 2);
  pp.log2_diff_max_min_transform_block_size = static_cast<uint8_t>(sps.log2_max_tb - sps.log2_min_tb);
  pp.max_transform_hierarchy_depth_inter = static_cast<uint8_t>(sps.max_th_depth_inter);
  pp.max_transform_hierarchy_depth_intra = static_cast<uint8_t>(sps.max_th_depth_intra);
  pp.num_short_term_ref_pic_sets = static_cast<uint8_t>(sps.num_st_rps);
  pp.num_long_term_ref_pics_sps = static_cast<uint8_t>(sps.num_lt_sps);
  pp.num_ref_idx_l0_default_active_minus1 = static_cast<uint8_t>(pps.num_ref_idx_default[0] - 1);
  pp.num_ref_idx_l1_default_active_minus1 = static_cast<uint8_t>(pps.num_ref_idx_default[1] - 1);
  pp.init_qp_minus26 = static_cast<int8_t>(pps.init_qp - 26);
  if (!sh.st_rps_sps_flag && !h265::is_idr(sh.nal_type)) {
    pp.ucNumDeltaPocsOfRefRpsIdx = static_cast<uint8_t>(sh.num_delta_pocs_ref_rps_idx);
    pp.wNumBitsForShortTermRPSInSlice = static_cast<uint16_t>(sh.st_rps_bits);
  }
  pp.dwCodingParamToolFlags = (sps.scaling_list_enabled << 0) | (sps.amp << 1) | (sps.sao << 2) | (sps.pcm << 3) |
                              ((sps.pcm ? sps.pcm_bit_depth_luma - 1 : 0) << 4) | ((sps.pcm ? sps.pcm_bit_depth_chroma - 1 : 0) << 8) |
                              ((sps.pcm ? sps.log2_min_pcm - 3 : 0) << 12) | ((sps.pcm ? sps.log2_max_pcm - sps.log2_min_pcm : 0) << 14) |
                              (sps.pcm_loop_filter_disabled << 16) | (sps.long_term_ref_pics_present << 17) | (sps.temporal_mvp << 18) |
                              (sps.strong_intra_smoothing << 19) | (pps.dependent_slice_segments << 20) | (pps.output_flag_present << 21) |
                              (pps.num_extra_slice_header_bits << 22) | (pps.sign_data_hiding << 25) | (pps.cabac_init_present << 26);
  const bool irap = h265::is_irap(sh.nal_type), idr = h265::is_idr(sh.nal_type);
  pp.dwCodingSettingPicturePropertyFlags =
      (pps.constrained_intra_pred << 0) | (pps.transform_skip << 1) | (pps.cu_qp_delta_enabled << 2) | (pps.slice_chroma_qp_offsets_present << 3) |
      (pps.weighted_pred << 4) | (pps.weighted_bipred << 5) | (pps.transquant_bypass << 6) | (pps.tiles_enabled << 7) | (pps.entropy_coding_sync << 8) |
      (pps.uniform_spacing << 9) | ((pps.tiles_enabled ? pps.loop_filter_across_tiles : 0) << 10) | (pps.loop_filter_across_slices << 11) |
      (pps.deblocking_override_enabled << 12) | (pps.deblocking_disabled << 13) | (pps.lists_modification_present << 14) |
      (pps.slice_header_extension_present << 15) | (irap << 16) | (idr << 17) | (irap << 18);
  pp.pps_cb_qp_offset = static_cast<int8_t>(pps.cb_qp_offset);
  pp.pps_cr_qp_offset = static_cast<int8_t>(pps.cr_qp_offset);
  if (pps.tiles_enabled) {
    pp.num_tile_columns_minus1 = static_cast<uint8_t>(pps.num_tile_cols - 1);
    pp.num_tile_rows_minus1 = static_cast<uint8_t>(pps.num_tile_rows - 1);
    if (!pps.uniform_spacing) {
      for (int i = 0; i < pps.num_tile_cols - 1; ++i) pp.column_width_minus1[i] = static_cast<uint16_t>(pps.col_width_minus1[i]);
      for (int i = 0; i < pps.num_tile_rows - 1; ++i) pp.row_height_minus1[i] = static_cast<uint16_t>(pps.row_height_minus1[i]);
    }
  }
  pp.diff_cu_qp_delta_depth = static_cast<uint8_t>(pps.diff_cu_qp_delta_depth);
  pp.pps_beta_offset_div2 = static_cast<int8_t>(pps.beta_offset_div2);
  pp.pps_tc_offset_div2 = static_cast<int8_t>(pps.tc_offset_div2);
  pp.log2_parallel_merge_level_minus2 = static_cast<uint8_t>(pps.log2_parallel_merge_level - 2);
  pp.CurrPicOrderCntVal = o.poc;
  for (int i = 0; i < 15; ++i) {
    if (static_cast<uint32_t>(i) < o.ref_count) {
      pp.RefPicList[i] = static_cast<uint8_t>(ref_index[i] | (o.refs[i].is_long ? 0x80 : 0));
      pp.PicOrderCntValList[i] = o.refs[i].poc;
    } else {
      pp.RefPicList[i] = 0xFF;
    }
  }
  pp.CurrPic = cur_index;
  memset(pp.RefPicSetStCurrBefore, 0xFF, 8);
  memset(pp.RefPicSetStCurrAfter, 0xFF, 8);
  memset(pp.RefPicSetLtCurr, 0xFF, 8);
  for (uint32_t i = 0; i < o.st_before_count && i < 8; ++i) pp.RefPicSetStCurrBefore[i] = o.st_before[i];
  for (uint32_t i = 0; i < o.st_after_count && i < 8; ++i) pp.RefPicSetStCurrAfter[i] = o.st_after[i];
  for (uint32_t i = 0; i < o.lt_count && i < 8; ++i) pp.RefPicSetLtCurr[i] = o.lt_curr[i];
  pp.StatusReportFeedbackNumber = 1;
  return pp;
}

// ffmpeg's ff_dxva2_hevc_fill_scaling_lists.
dxva::QmatrixHevc hevc_fill_qm(const h265::Sps& sps, const h265::Pps& pps) {
  const h265::ScalingList& sl = pps.scaling_list_present ? pps.scaling : sps.scaling;
  uint8_t s4[16], s8[64];
  hevc_diag_scan(4, s4);
  hevc_diag_scan(8, s8);
  dxva::QmatrixHevc q;
  memset(&q, 0, sizeof q);
  for (int i = 0; i < 6; ++i) {
    for (int j = 0; j < 16; ++j) q.ucScalingLists0[i][j] = sl.list[0][i][s4[j]];
    for (int j = 0; j < 64; ++j) {
      q.ucScalingLists1[i][j] = sl.list[1][i][s8[j]];
      q.ucScalingLists2[i][j] = sl.list[2][i][s8[j]];
      if (i < 2) q.ucScalingLists3[i][j] = sl.list[3][i * 3][s8[j]];
    }
    q.ucScalingListDCCoefSizeID2[i] = sl.dc[2][i];
    if (i < 2) q.ucScalingListDCCoefSizeID3[i] = sl.dc[3][i * 3];
  }
  return q;
}

template <class T>
std::vector<uint8_t> hevc_bytes(const T& v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  return std::vector<uint8_t>(p, p + sizeof v);
}

struct HevcDxvaApp final : h265::IOffload {
  TestAllocator* alloc = nullptr;
  DxvaNaming naming = DxvaNaming::D3d11;
  UniquePointer<IDxvaDecoder> decoder;
  uint32_t slot_key[h265::kMaxPics] = {};
  uint32_t next_key = 1000;
  std::mt19937 rng{7};
  int pictures = 0, failures = 0;
  std::vector<DxvaRecordedCall>* record = nullptr;
  uint32_t width = 0, height = 0;

  bool accepts(const h265::Sps&, const h265::Pps&) override { return true; }

  bool decode_picture(const h265::OffloadPicture& o) override {
    if (!decoder) {
      DxvaDecoderConfig cfg;
      cfg.profile = DxvaProfile::HevcVldMain;
      cfg.width = width = static_cast<uint32_t>(o.sps->width);
      cfg.height = height = static_cast<uint32_t>(o.sps->height);
      cfg.surfaces = h265::kMaxPics;
      cfg.output = PixelFormat::Nv12;
      decoder = create_dxva_decoder(*alloc, cfg);
      CHECK(decoder);
      if (!decoder) return false;
    }
    // The names of the surfaces.
    DxvaRecordedCall call;
    uint8_t ref_index[h265::kMaxRefs] = {};
    uint8_t cur_index = 0;
    if (naming == DxvaNaming::D3d11) {
      for (uint32_t i = 0; i < o.ref_count; ++i) ref_index[i] = static_cast<uint8_t>(o.refs[i].slot);
      cur_index = static_cast<uint8_t>(o.slot);
    } else {
      slot_key[o.slot] = next_key++;   // a new surface for every picture: the old keys age out of the decoder's table
      std::vector<uint32_t> keys;
      for (uint32_t i = 0; i < o.ref_count; ++i) keys.push_back(slot_key[o.refs[i].slot]);
      if (naming == DxvaNaming::D3d12Shuffled) keys.push_back(slot_key[o.slot]);
      std::shuffle(keys.begin(), keys.end(), rng);
      auto position = [&](uint32_t key) { return static_cast<uint8_t>(std::find(keys.begin(), keys.end(), key) - keys.begin()); };
      for (uint32_t i = 0; i < o.ref_count; ++i) ref_index[i] = position(slot_key[o.refs[i].slot]);
      if (naming == DxvaNaming::D3d12Shuffled) {
        cur_index = position(slot_key[o.slot]);
      } else {
        cur_index = static_cast<uint8_t>(keys.size() + 5);   // outside the list: the output is named by output_key
        call.output_key = slot_key[o.slot];
      }
      call.keys = keys;
    }
    const dxva::PicParamsHevc pp = hevc_fill_params(o, ref_index, cur_index);
    call.params = hevc_bytes(pp);
    if (o.sps->scaling_list_enabled) call.qm = hevc_bytes(hevc_fill_qm(*o.sps, *o.pps));
    for (const ConstByteView& nal : o.slices) {
      dxva::SliceHevcShort s;
      memset(&s, 0, sizeof s);
      s.BSNALunitDataLocation = static_cast<uint32_t>(call.bitstream.size());
      s.SliceBytesInBuffer = static_cast<uint32_t>(nal.size() + 3);
      call.bitstream.push_back(0);
      call.bitstream.push_back(0);
      call.bitstream.push_back(1);
      call.bitstream.insert(call.bitstream.end(), nal.data(), nal.data() + nal.size());
      const std::vector<uint8_t> b = hevc_bytes(s);
      call.slices.insert(call.slices.end(), b.begin(), b.end());
    }
    // ffmpeg pads the bitstream to 128 bytes and counts the padding to the last slice.
    const size_t padding = 128 - (call.bitstream.size() & 127);
    call.bitstream.resize(call.bitstream.size() + padding, 0);
    if (!call.slices.empty()) {
      dxva::SliceHevcShort last;
      memcpy(&last, call.slices.data() + call.slices.size() - sizeof last, sizeof last);
      last.SliceBytesInBuffer += static_cast<uint32_t>(padding);
      memcpy(call.slices.data() + call.slices.size() - sizeof last, &last, sizeof last);
    }
    DxvaDecodedPicture out;
    const DxvaResult r = decoder->decode(call.picture(), &out);
    ++pictures;
    if (record) record->push_back(call);
    if (r != DxvaResult::Ok) {
      printf("  HEVC picture %d (poc %d): result %d (%s)\n", pictures, o.poc, static_cast<int>(r), out.detail ? out.detail : "");
      ++failures;
      return false;
    }
    CHECK(out.surface == cur_index);
    CHECK(out.frame.format.pixel_format == PixelFormat::Nv12 && out.frame.format.width == width && out.frame.format.height == height);
    // NV12 back into the decoder's I420 picture (the coded size).
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) o.plane[0][uint64_t(y) * o.stride[0] + x] = out.frame.plane[0][size_t(y) * out.frame.stride[0] + x];
    for (uint32_t y = 0; y < height / 2; ++y)
      for (uint32_t x = 0; x < width / 2; ++x) {
        o.plane[1][uint64_t(y) * o.stride[1] + x] = out.frame.plane[1][size_t(y) * out.frame.stride[1] + 2 * x];
        o.plane[2][uint64_t(y) * o.stride[2] + x] = out.frame.plane[1][size_t(y) * out.frame.stride[1] + 2 * x + 1];
      }
    return true;
  }
};

// The access units of an Annex B stream: a new one at a parameter set, AUD or prefix SEI, or at a slice that starts a picture, after the slices of the one before.
std::vector<std::pair<size_t, size_t>> hevc_access_units(const std::vector<uint8_t>& d) {
  std::vector<size_t> starts;   // NAL start code positions
  for (size_t i = 0; i + 3 <= d.size(); ++i)
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) starts.push_back(i);
  std::vector<std::pair<size_t, size_t>> units;
  size_t unit_start = 0;
  bool have_slice = false;
  for (size_t k = 0; k < starts.size(); ++k) {
    const size_t nal = starts[k] + 3;
    if (nal + 2 >= d.size()) break;
    const int type = (d[nal] >> 1) & 0x3F;
    const bool slice = type <= 21;
    const bool first_slice = slice && (d[nal + 2] & 0x80);
    const bool starts_unit = (type >= 32 && type <= 35) || type == 39 || first_slice;
    size_t at = starts[k];
    if (at > 0 && d[at - 1] == 0) --at;   // a four-byte start code
    if (starts_unit && have_slice) {
      units.push_back({unit_start, at - unit_start});
      unit_start = at;
      have_slice = false;
    }
    if (slice) have_slice = true;
  }
  units.push_back({unit_start, d.size() - unit_start});
  return units;
}

// Decodes `name` with the surfaces named as `naming` says; returns how many pictures matched ffmpeg's.
int dxva_hevc_stream(const std::string& name, DxvaNaming naming, std::vector<DxvaRecordedCall>* record, int* total) {
  TestAllocator alloc;
  const std::vector<uint8_t> data = read_file("tests/data/" + name);
  const std::vector<std::string> refs = dxva_reference_md5(name);
  CHECK(!data.empty() && !refs.empty());
  int matched = 0, n = 0;
  // ffmpeg decodes the chroma of hevc_cip wrong (its own picture hash check fails there, see scripts/gen_hevc_test_streams.sh): that stream is checked against the
  // MD5s it carries for every picture (SEI decoded picture hash) instead.
  const bool ffmpeg_wrong = name == "hevc_cip.h265";
  {
    HevcDxvaApp app;
    app.alloc = &alloc;
    app.naming = naming;
    app.record = record;
    DxvaMd5PlaneHasher hasher;
    H265Decoder dec(alloc);
    dec.set_offload(&app);
    dec.set_plane_hasher(&hasher);
    auto drain = [&] {
      VideoFrame f;
      while (dec.receive(&f)) {
        std::vector<uint8_t> packed;
        const uint32_t w = f.format.width, h = f.format.height;
        for (uint32_t y = 0; y < h; ++y) packed.insert(packed.end(), f.plane[0] + size_t(y) * f.stride[0], f.plane[0] + size_t(y) * f.stride[0] + w);
        for (int c = 1; c < 3; ++c)
          for (uint32_t y = 0; y < h / 2; ++y) packed.insert(packed.end(), f.plane[c] + size_t(y) * f.stride[c], f.plane[c] + size_t(y) * f.stride[c] + w / 2);
        const std::string md5 = dxva_md5_hex(packed);
        if (n < static_cast<int>(refs.size()) && (md5 == refs[n] || ffmpeg_wrong)) ++matched;
        else if (n - matched < 3) printf("  %s %s: picture %d differs from ffmpeg's\n", name.c_str(), dxva_naming_name(naming), n);
        ++n;
      }
    };
    for (const auto& au : hevc_access_units(data)) {
      dec.decode(ConstByteView(data.data() + au.first, au.second), 0);
      drain();
    }
    dec.flush();
    drain();
    CHECK(app.failures == 0);
    CHECK(n == static_cast<int>(refs.size()));
    CHECK(matched == n);
    const H265Decoder::HashStatistics stats = dec.hash_statistics();
    CHECK(stats.mismatched == 0);
    CHECK(stats.checked == static_cast<uint64_t>(n));   // (every picture of the x265 streams carries its MD5)
  }
  CHECK(alloc.live == 0);
  *total = static_cast<int>(refs.size());
  return matched;
}

// The decoder of the recorded stream's size.
UniquePointer<IDxvaDecoder> hevc_make_decoder(TestAllocator& alloc, const DxvaRecordedCall& first, uint32_t surfaces = h265::kMaxPics) {
  dxva::PicParamsHevc pp;
  memcpy(&pp, first.params.data(), sizeof pp);
  DxvaDecoderConfig cfg;
  cfg.profile = DxvaProfile::HevcVldMain;
  cfg.width = static_cast<uint32_t>(pp.PicWidthInMinCbsY) << (pp.log2_min_luma_coding_block_size_minus3 + 3);
  cfg.height = static_cast<uint32_t>(pp.PicHeightInMinCbsY) << (pp.log2_min_luma_coding_block_size_minus3 + 3);
  cfg.surfaces = surfaces;
  cfg.output = PixelFormat::Nv12;
  return create_dxva_decoder(alloc, cfg);
}

void dxva_hevc_errors() {
  std::vector<DxvaRecordedCall> calls;
  int total = 0;
  dxva_hevc_stream("hevc_scaling.h265", DxvaNaming::D3d11, &calls, &total);
  CHECK(calls.size() >= 4);
  if (calls.size() < 4) return;
  TestAllocator alloc;
  {
    // An inter picture with its references in place decodes; on a fresh decoder its references are missing: grey pictures, MissingReference, a picture all the same.
    size_t inter = 0;
    for (size_t i = 1; i < calls.size() && !inter; ++i) {
      dxva::PicParamsHevc pp;
      memcpy(&pp, calls[i].params.data(), sizeof pp);
      if (pp.RefPicSetStCurrBefore[0] != 0xFF) inter = i;
    }
    CHECK(inter > 0);
    UniquePointer<IDxvaDecoder> d = hevc_make_decoder(alloc, calls[0]);
    DxvaDecodedPicture o;
    CHECK(d->decode(calls[inter].picture(), &o) == DxvaResult::MissingReference && o.frame.plane[0] != nullptr);
    d->reset();
    for (size_t i = 0; i <= inter; ++i) CHECK(d->decode(calls[i].picture(), &o) == DxvaResult::Ok);
    // reset() forgets every picture
    d->reset();
    CHECK(d->decode(calls[inter].picture(), &o) == DxvaResult::MissingReference);

    // Surface indices out of range: CurrPic, a RefPicList entry; a set naming an empty RefPicList entry; an index past the key list.
    DxvaRecordedCall c = calls[inter];
    dxva::PicParamsHevc pp;
    memcpy(&pp, c.params.data(), sizeof pp);
    dxva::PicParamsHevc bad = pp;
    bad.CurrPic = 40;
    c.params = hevc_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    bad = pp;
    bad.RefPicList[0] = 100;
    c.params = hevc_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    bad = pp;
    bad.RefPicSetStCurrBefore[0] = 14;
    bad.RefPicList[14] = 0xFF;
    c.params = hevc_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    c = calls[inter];
    bad = pp;
    bad.CurrPic = 3;
    c.params = hevc_bytes(bad);
    c.keys = {1, 2};   // index 3 is past the key list
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    // Missing and short buffers.
    c = calls[inter];
    c.params.resize(sizeof(dxva::PicParamsHevc) - 1);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    c = calls[inter];
    c.slices.clear();
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    c = calls[inter];
    c.slices.resize(c.slices.size() - 1);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    c = calls[inter];
    c.bitstream.clear();
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    c = calls[inter];
    c.qm.clear();   // scaling lists on, no matrices
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    c = calls[inter];
    c.bitstream.resize(c.bitstream.size() - 200);   // the slice runs past the bitstream
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    // What Main does not take: 10-bit, 4:2:2, a picture larger than the surfaces.
    bad = pp;
    bad.wFormatAndSequenceInfoFlags = static_cast<uint16_t>(bad.wFormatAndSequenceInfoFlags | (2 << 3) | (2 << 6));
    c = calls[inter];
    c.params = hevc_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Unsupported && o.detail != nullptr);
    bad = pp;
    bad.wFormatAndSequenceInfoFlags = static_cast<uint16_t>((bad.wFormatAndSequenceInfoFlags & ~3u) | 2);
    c.params = hevc_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Unsupported);
    bad = pp;
    bad.PicWidthInMinCbsY = static_cast<uint16_t>(bad.PicWidthInMinCbsY + 8);
    c.params = hevc_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    // A chopped slice (wBadSliceChopping 1 + 3) decodes as the whole one did.
    d->reset();
    for (size_t i = 0; i < inter; ++i) CHECK(d->decode(calls[i].picture(), &o) == DxvaResult::Ok);
    c = calls[inter];
    if (c.slices.size() == sizeof(dxva::SliceHevcShort)) {
      dxva::SliceHevcShort s;
      memcpy(&s, c.slices.data(), sizeof s);
      dxva::SliceHevcShort a = s, b = s;
      a.SliceBytesInBuffer = s.SliceBytesInBuffer / 2;
      a.wBadSliceChopping = 1;
      b.BSNALunitDataLocation = s.BSNALunitDataLocation + a.SliceBytesInBuffer;
      b.SliceBytesInBuffer = s.SliceBytesInBuffer - a.SliceBytesInBuffer;
      b.wBadSliceChopping = 3;
      c.slices = hevc_bytes(a);
      const std::vector<uint8_t> tail = hevc_bytes(b);
      c.slices.insert(c.slices.end(), tail.begin(), tail.end());
      DxvaDecodedPicture whole;
      UniquePointer<IDxvaDecoder> d2 = hevc_make_decoder(alloc, calls[0]);
      for (size_t i = 0; i < inter; ++i) CHECK(d2->decode(calls[i].picture(), &whole) == DxvaResult::Ok);
      CHECK(d2->decode(calls[inter].picture(), &whole) == DxvaResult::Ok);
      std::vector<uint8_t> expect(whole.frame.plane[0], whole.frame.plane[0] + whole.frame.stride[0] * whole.frame.format.height);
      CHECK(d->decode(c.picture(), &o) == DxvaResult::Ok);
      CHECK(memcmp(expect.data(), o.frame.plane[0], expect.size()) == 0);
    }
  }
  {
    // Direct3D 12 may name as the output a surface the picture refers to: the reference is the picture that was there before, the new one takes its place.
    size_t inter = 0;
    for (size_t i = 1; i < calls.size() && !inter; ++i) {
      dxva::PicParamsHevc pp;
      memcpy(&pp, calls[i].params.data(), sizeof pp);
      if (pp.RefPicSetStCurrBefore[0] != 0xFF) inter = i;
    }
    if (inter) {
      UniquePointer<IDxvaDecoder> a = hevc_make_decoder(alloc, calls[0]), b = hevc_make_decoder(alloc, calls[0]);
      DxvaDecodedPicture oa, ob;
      for (size_t i = 0; i < inter; ++i) {
        CHECK(a->decode(calls[i].picture(), &oa) == DxvaResult::Ok);
        CHECK(b->decode(calls[i].picture(), &ob) == DxvaResult::Ok);
      }
      CHECK(a->decode(calls[inter].picture(), &oa) == DxvaResult::Ok);
      dxva::PicParamsHevc pp;
      memcpy(&pp, calls[inter].params.data(), sizeof pp);
      DxvaRecordedCall c = calls[inter];
      c.output_key = pp.RefPicList[pp.RefPicSetStCurrBefore[0]] & 0x7F;   // (no key list: the key of index i is i)
      CHECK(b->decode(c.picture(), &ob) == DxvaResult::Ok);
      const size_t bytes = size_t(oa.frame.stride[0]) * oa.frame.format.height * 3 / 2;
      CHECK(ob.frame.plane[0] && memcmp(oa.frame.plane[0], ob.frame.plane[0], bytes) == 0);
    }
  }
  CHECK(alloc.live == 0);
  // Damaged buffers: no fault, no sanitizer report.
  int runs = 0;
  auto make = [&]() { return hevc_make_decoder(alloc, calls[0]); };
  runs += dxva_damage(make, {}, calls[0], 1);
  runs += dxva_damage(make, std::vector<DxvaRecordedCall>(calls.begin(), calls.begin() + 3), calls[3], 2);
  uint32_t seed = 3;
  for (const char* s : {"hevc_tools.h265", "hevc_wpp.h265", "hevc_slices.h265", "hevc_weighted.h265", "hevc_lossless.h265"}) {
    std::vector<DxvaRecordedCall> more;
    dxva_hevc_stream(s, DxvaNaming::D3d11, &more, &total);
    if (more.size() < 3) continue;
    auto make_more = [&]() { return hevc_make_decoder(alloc, more[0]); };
    runs += dxva_damage(make_more, std::vector<DxvaRecordedCall>(more.begin(), more.begin() + 2), more[2], seed++);
  }
  CHECK(alloc.live == 0);
  printf("  HEVC damaged buffers: %d decodes\n", runs);
}

}  // namespace

static void dxva_test_hevc() {
  static const char* kStreams[] = {"hevc_bframes.h265", "hevc_cip.h265",      "hevc_cra.h265",     "hevc_ctu64.h265",   "hevc_cutree_off.h265",
                                   "hevc_highrate.h265", "hevc_intra_filt.h265", "hevc_intra.h265",   "hevc_ipp_filt.h265", "hevc_ipp.h265",
                                   "hevc_lossless.h265", "hevc_lowrate.h265",  "hevc_maxref.h265",  "hevc_oddsize.h265", "hevc_qcif.h265",
                                   "hevc_scaling.h265",  "hevc_slices.h265",   "hevc_tools.h265",   "hevc_weighted.h265", "hevc_wpp.h265"};
  int matched = 0, total = 0;
  for (DxvaNaming naming : {DxvaNaming::D3d11, DxvaNaming::D3d12Shuffled, DxvaNaming::D3d12OutputKey})
    for (const char* s : kStreams) {
      int t = 0;
      matched += dxva_hevc_stream(s, naming, nullptr, &t);
      total += t;
    }
  printf("DXVA HEVC: %d of %d pictures bit-exact with ffmpeg (hevc_cip: with its SEI MD5s; 20 streams, 3 ways of naming surfaces)\n", matched, total);
  dxva_hevc_errors();
}
