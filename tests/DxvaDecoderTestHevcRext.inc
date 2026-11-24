// DxvaDecoderTest: the HEVC range extension profiles (DxvaProfile::HevcVldMain12 .. HevcVldMonochrome10, src/Hardware/VideoCodec/Dxva/DxvaHevc.cpp). Included by
// DxvaDecoderTestHevc.inc, whose program it shares: the kernel's H.265 decoder parses tests/data/hevcrext_*.h265 (x265's 4:0:0 / 4:2:0 / 4:2:2 / 4:4:4 streams of 8 to
// 12 bits, scripts/gen_hevc_rext_test_streams.sh) and hevcrexttool_*.h265 (the range extension tools, scripts/gen_hevc_rext_tool_streams.py) and hands every picture
// over; this fills DXVA_PicParams_HEVC_RangeExt as libavcodec/dxva2_hevc.c does for its range extension profiles, decodes it in the profile and surface format the
// stream's chroma format and bit depth call for (or one given), and writes the surface back into the H.265 decoder's planes - the samples of every packed and planar
// format taken apart, checked to hold nothing in the bits below the sample. The streams carry an MD5 of every plane of every picture (x265's, and in the tool streams
// the reference decoder's), which the H.265 decoder checks over those planes: every picture must have one and agree with it. The x265 streams' pictures are compared
// with FFmpeg's frame MD5s (hevcrext_*.md5) as well.

namespace {

struct HevcRoute {
  DxvaProfile profile;
  DxvaSurfaceFormat surface;
};

// The profile and surface format a program picks for a stream (the smallest profile that takes it; Main and Main 10 for 4:2:0 streams without range extension tools).
HevcRoute hevc_route(const h265::Sps& sps, const h265::Pps& pps) {
  const int depth = sps.bit_depth_luma > sps.bit_depth_chroma || sps.chroma_format_idc == 0 ? sps.bit_depth_luma : sps.bit_depth_chroma;
  const bool tools = sps.range_extension_tools() || pps.range_extension_tools();
  switch (sps.chroma_format_idc) {
    case 0:
      if (depth <= 8) return {DxvaProfile::HevcVldMonochrome, DxvaSurfaceFormat::Nv12};
      if (depth <= 10) return {DxvaProfile::HevcVldMonochrome10, DxvaSurfaceFormat::P010};
      return {DxvaProfile::HevcVldMain12, DxvaSurfaceFormat::P016};
    case 1:
      if (!tools && depth <= 8) return {DxvaProfile::HevcVldMain, DxvaSurfaceFormat::Nv12};
      if (!tools && depth <= 10) return {DxvaProfile::HevcVldMain10, DxvaSurfaceFormat::P010};
      if (depth <= 10) return {DxvaProfile::HevcVldMain10Ext, DxvaSurfaceFormat::P010};
      return {DxvaProfile::HevcVldMain12, DxvaSurfaceFormat::P016};
    case 2:
      if (depth <= 8) return {DxvaProfile::HevcVldMain10_422, DxvaSurfaceFormat::Yuy2};
      if (depth <= 10) return {DxvaProfile::HevcVldMain10_422, DxvaSurfaceFormat::Y210};
      return {DxvaProfile::HevcVldMain12_422, DxvaSurfaceFormat::Y216};
    default:
      if (depth <= 8) return {DxvaProfile::HevcVldMain444, DxvaSurfaceFormat::Ayuv};
      if (depth <= 10) return {DxvaProfile::HevcVldMain10_444, DxvaSurfaceFormat::Y410};
      if (depth <= 12) return {DxvaProfile::HevcVldMain12_444, DxvaSurfaceFormat::Y416};
      return {DxvaProfile::HevcVldMain16, DxvaSurfaceFormat::Y416};
  }
}

bool hevc_route_is_rext(DxvaProfile p) { return p != DxvaProfile::HevcVldMain && p != DxvaProfile::HevcVldMain10; }

// ffmpeg's fill_picture_parameters for the range extension profiles (DXVA_PicParams_HEVC_RangeExt).
dxva::PicParamsHevcRangeExt hevc_fill_rext(const h265::OffloadPicture& o, const dxva::PicParamsHevc& base) {
  const h265::Sps& sps = *o.sps;
  const h265::Pps& pps = *o.pps;
  dxva::PicParamsHevcRangeExt r;
  memset(&r, 0, sizeof r);
  r.params = base;
  namespace rx = dxva::hevc_rext;
  r.dwRangeExtensionFlags = static_cast<uint16_t>((sps.transform_skip_rotation << rx::kTransformSkipRotation) | (sps.transform_skip_context << rx::kTransformSkipContext) |
                                                  (sps.implicit_rdpcm << rx::kImplicitRdpcm) | (sps.explicit_rdpcm << rx::kExplicitRdpcm) |
                                                  (sps.extended_precision << rx::kExtendedPrecision) | (sps.intra_smoothing_disabled << rx::kIntraSmoothingDisabled) |
                                                  (sps.persistent_rice << rx::kPersistentRice) | (sps.high_precision_offsets << rx::kHighPrecisionOffsets) |
                                                  (sps.cabac_bypass_alignment << rx::kCabacBypassAlignment) |
                                                  (pps.cross_component_prediction << rx::kCrossComponentPrediction) | (pps.chroma_qp_offset_list << rx::kChromaQpOffsetList));
  r.diff_cu_chroma_qp_offset_depth = static_cast<uint8_t>(pps.diff_cu_chroma_qp_offset_depth);
  r.log2_sao_offset_scale_luma = static_cast<uint8_t>(pps.log2_sao_offset_scale_luma);
  r.log2_sao_offset_scale_chroma = static_cast<uint8_t>(pps.log2_sao_offset_scale_chroma);
  r.log2_max_transform_skip_block_size_minus2 = static_cast<uint8_t>(pps.log2_max_transform_skip - 2);
  for (int i = 0; i < 6; ++i) {
    r.cb_qp_offset_list[i] = static_cast<int8_t>(pps.cb_qp_offset_list[i]);
    r.cr_qp_offset_list[i] = static_cast<int8_t>(pps.cr_qp_offset_list[i]);
  }
  r.chroma_qp_offset_list_len_minus1 = static_cast<uint8_t>(pps.chroma_qp_offset_list ? pps.chroma_qp_offset_list_len - 1 : 0);
  return r;
}

// A surface (DxvaDecodedPicture::frame of `surface`) back into the H.265 decoder's planes of `o`; the samples of the picture as one planar buffer (its chroma format,
// bytes or 16-bit little-endian words) into *native. False: something sat in the bits below a sample, or the monochrome chroma was not neutral.
bool hevc_surface_to_planes(const VideoFrame& f, DxvaSurfaceFormat surface, const h265::OffloadPicture& o, std::vector<uint8_t>* native) {
  const h265::Sps& sps = *o.sps;
  const uint32_t w = uint32_t(sps.width), h = uint32_t(sps.height);
  const int cf = sps.chroma_format_idc;
  const uint32_t cw = cf == 3 ? w : w / 2, ch = cf >= 2 ? h : h / 2;
  const bool deep = sps.bit_depth_luma > 8 || sps.bit_depth_chroma > 8;
  bool clean = true;
  auto word = [](const uint8_t* q) { return uint32_t(q[0] | (q[1] << 8)); };
  // a field of `bits` bits holding a sample of `depth` bits in its top
  auto take = [&](uint32_t field, int bits, int depth) {
    if (depth < bits && (field & ((1u << (bits - depth)) - 1)) != 0) clean = false;
    return depth < bits ? field >> (bits - depth) : field;
  };
  auto store = [&](int c, uint32_t x, uint32_t y, uint32_t v) {
    if (deep) {
      o.plane[c][uint64_t(y) * o.stride[c] + x * 2] = uint8_t(v);
      o.plane[c][uint64_t(y) * o.stride[c] + x * 2 + 1] = uint8_t(v >> 8);
    } else {
      o.plane[c][uint64_t(y) * o.stride[c] + x] = uint8_t(v);
    }
  };
  const int dy = sps.bit_depth_luma, dc = cf == 0 ? sps.bit_depth_luma : sps.bit_depth_chroma;
  const uint8_t* p = f.plane[0];
  switch (surface) {
    case DxvaSurfaceFormat::Ayuv:
    case DxvaSurfaceFormat::Y410:
    case DxvaSurfaceFormat::Y416:
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          const uint8_t* q = p + size_t(y) * f.stride[0];
          uint32_t Y, U, V, A;
          if (surface == DxvaSurfaceFormat::Ayuv) {
            V = take(q[x * 4], 8, dc), U = take(q[x * 4 + 1], 8, dc), Y = take(q[x * 4 + 2], 8, dy), A = q[x * 4 + 3];
            if (A != 0xFF) clean = false;
          } else if (surface == DxvaSurfaceFormat::Y410) {
            const uint32_t v = word(q + x * 4) | (word(q + x * 4 + 2) << 16);
            U = take(v & 0x3FF, 10, dc), Y = take((v >> 10) & 0x3FF, 10, dy), V = take((v >> 20) & 0x3FF, 10, dc);
            if ((v >> 30) != 3) clean = false;
          } else {
            U = take(word(q + x * 8), 16, dc), Y = take(word(q + x * 8 + 2), 16, dy), V = take(word(q + x * 8 + 4), 16, dc);
            if (word(q + x * 8 + 6) != 0xFFFF) clean = false;
          }
          store(0, x, y, Y);
          store(1, x, y, U);
          store(2, x, y, V);
        }
      break;
    case DxvaSurfaceFormat::Yuy2:
    case DxvaSurfaceFormat::Y210:
    case DxvaSurfaceFormat::Y216: {
      const bool bytes = surface == DxvaSurfaceFormat::Yuy2;
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; x += 2) {
          const uint8_t* q = p + size_t(y) * f.stride[0] + size_t(x / 2) * (bytes ? 4 : 8);
          auto get = [&](int k, int depth) { return bytes ? take(q[k], 8, depth) : take(word(q + 2 * k), 16, depth); };
          store(0, x, y, get(0, dy));
          store(1, x / 2, y, get(1, dc));
          if (x + 1 < w) store(0, x + 1, y, get(2, dy));
          store(2, x / 2, y, get(3, dc));
        }
      break;
    }
    default: {   // NV12, P010, P016
      const bool words = surface != DxvaSurfaceFormat::Nv12;
      auto get = [&](const uint8_t* row, uint32_t i, int depth) { return words ? take(word(row + i * 2), 16, depth) : take(row[i], 8, depth); };
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) store(0, x, y, get(f.plane[0] + size_t(y) * f.stride[0], x, dy));
      for (uint32_t y = 0; y < h / 2; ++y)
        for (uint32_t x = 0; x < w / 2; ++x) {
          const uint8_t* row = f.plane[1] + size_t(y) * f.stride[1];
          const uint32_t u = get(row, 2 * x, dc), v = get(row, 2 * x + 1, dc);
          if (cf == 0) {
            if (u != (1u << (dc - 1)) || v != (1u << (dc - 1))) clean = false;   // (neutral)
          } else {
            store(1, x, y, u);
            store(2, x, y, v);
          }
        }
      break;
    }
  }
  // the planar picture, as FFmpeg's frame MD5s hash it
  native->clear();
  const uint32_t planes = cf == 0 ? 1 : 3;
  for (uint32_t c = 0; c < planes; ++c) {
    const uint32_t pw = c ? cw : w, ph = c ? ch : h;
    for (uint32_t y = 0; y < ph; ++y) {
      const uint8_t* row = o.plane[c].data() + size_t(y) * o.stride[c];
      native->insert(native->end(), row, row + size_t(pw) * (deep ? 2 : 1));
    }
  }
  return clean;
}

struct HevcRextApp final : h265::IOffload {
  TestAllocator* alloc = nullptr;
  DxvaNaming naming = DxvaNaming::D3d11;
  bool forced = false;
  HevcRoute route{DxvaProfile::HevcVldMain, DxvaSurfaceFormat::Nv12};
  UniquePointer<IDxvaDecoder> decoder;
  uint32_t slot_key[h265::kMaxPics] = {};
  uint32_t next_key = 1000;
  std::mt19937 rng{11};
  int pictures = 0, failures = 0, unclean = 0;
  std::vector<DxvaRecordedCall>* record = nullptr;
  std::vector<std::vector<uint8_t>> native;   // each picture's planes, in decoding order
  std::vector<int> poc;                       // and its order count

  bool accepts(const h265::Sps&, const h265::Pps&) override { return true; }

  bool decode_picture(const h265::OffloadPicture& o) override {
    if (!decoder) {
      if (!forced) route = hevc_route(*o.sps, *o.pps);
      DxvaDecoderConfig cfg;
      cfg.profile = route.profile;
      cfg.surface = route.surface;
      cfg.width = static_cast<uint32_t>(o.sps->width);
      cfg.height = static_cast<uint32_t>(o.sps->height);
      cfg.surfaces = h265::kMaxPics;
      decoder = create_dxva_decoder(*alloc, cfg);
      CHECK(decoder);
      if (!decoder) return false;
    }
    DxvaRecordedCall call;
    uint8_t ref_index[h265::kMaxRefs] = {};
    uint8_t cur_index = 0;
    if (naming == DxvaNaming::D3d11) {
      for (uint32_t i = 0; i < o.ref_count; ++i) ref_index[i] = static_cast<uint8_t>(o.refs[i].slot);
      cur_index = static_cast<uint8_t>(o.slot);
    } else {
      slot_key[o.slot] = next_key++;
      std::vector<uint32_t> keys;
      for (uint32_t i = 0; i < o.ref_count; ++i) keys.push_back(slot_key[o.refs[i].slot]);
      if (naming == DxvaNaming::D3d12Shuffled) keys.push_back(slot_key[o.slot]);
      std::shuffle(keys.begin(), keys.end(), rng);
      auto position = [&](uint32_t key) { return static_cast<uint8_t>(std::find(keys.begin(), keys.end(), key) - keys.begin()); };
      for (uint32_t i = 0; i < o.ref_count; ++i) ref_index[i] = position(slot_key[o.refs[i].slot]);
      if (naming == DxvaNaming::D3d12Shuffled) {
        cur_index = position(slot_key[o.slot]);
      } else {
        cur_index = static_cast<uint8_t>(keys.size() + 5);
        call.output_key = slot_key[o.slot];
      }
      call.keys = keys;
    }
    const dxva::PicParamsHevc pp = hevc_fill_params(o, ref_index, cur_index);
    call.params = hevc_route_is_rext(route.profile) ? hevc_bytes(hevc_fill_rext(o, pp)) : hevc_bytes(pp);
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
      printf("  HEVC range extension picture %d (poc %d): result %d (%s)\n", pictures, o.poc, static_cast<int>(r), out.detail ? out.detail : "");
      ++failures;
      return false;
    }
    CHECK(out.surface == cur_index && out.surface_format == route.surface);
    CHECK(out.frame.format.width == uint32_t(o.sps->width) && out.frame.format.height == uint32_t(o.sps->height));
    std::vector<uint8_t> planes;
    if (!hevc_surface_to_planes(out.frame, route.surface, o, &planes)) ++unclean;
    native.push_back(planes);
    poc.push_back(o.poc);
    return true;
  }
};

// Decodes tests/data/<name>.h265 through the range extension profiles; returns how many pictures agreed with the hash they carry (and FFmpeg's MD5s, when there are).
// `route`: the profile and surface format (null: the one hevc_route() picks).
// `display` (when not null) gets every output picture in display order: the decode-order index of its picture and its planes.
int dxva_hevc_rext_stream(const std::string& name, DxvaNaming naming, const HevcRoute* route, int* total, std::vector<DxvaRecordedCall>* record = nullptr,
                          std::vector<std::pair<size_t, std::vector<uint8_t>>>* display = nullptr) {
  TestAllocator alloc;
  const std::vector<uint8_t> data = read_file("tests/data/" + name);
  CHECK(!data.empty());
  std::vector<std::string> md5s;
  {
    const std::vector<uint8_t> text = read_file("tests/data/" + name.substr(0, name.size() - 5) + ".md5");   // (x265's streams only)
    std::string line;
    for (uint8_t ch : text) {
      if (ch == '\n') {
        if (line.size() >= 32) md5s.push_back(line.substr(0, 32));
        line.clear();
      } else {
        line.push_back(char(ch));
      }
    }
  }
  int good = 0, n = 0;
  {
    HevcRextApp app;
    app.alloc = &alloc;
    app.naming = naming;
    app.record = record;
    if (route) {
      app.forced = true;
      app.route = *route;
    }
    DxvaMd5PlaneHasher hasher;
    H265Decoder dec(alloc);
    dec.set_offload(&app);
    dec.set_plane_hasher(&hasher);
    auto drain = [&] {
      VideoFrame f;
      while (dec.receive(&f)) {
        // the picture in display order: its planes as decoded (the pts is the decode order)
        const size_t at = size_t(f.pts_us);
        if (at < app.native.size()) {
          if (display) display->push_back({at, app.native[at]});
          const bool same = md5s.empty() || (size_t(n) < md5s.size() && dxva_md5_hex(app.native[at]) == md5s[size_t(n)]);
          if (same) ++good;
          else if (n - good < 3) printf("  %s %s: picture %d differs from FFmpeg's\n", name.c_str(), dxva_naming_name(naming), n);
        }
        ++n;
      }
    };
    int64_t au_index = 0;
    for (const auto& au : hevc_access_units(data)) {
      dec.decode(ConstByteView(data.data() + au.first, au.second), au_index++);
      drain();
    }
    dec.flush();
    drain();
    CHECK(app.failures == 0 && app.unclean == 0);
    CHECK(md5s.empty() || n == static_cast<int>(md5s.size()));
    const H265Decoder::HashStatistics stats = dec.hash_statistics();
    if (stats.mismatched != 0 || stats.checked != uint64_t(n)) printf("  %s: %llu of %d pictures checked, %llu mismatched\n", name.c_str(), (unsigned long long)stats.checked, n,
                                                                         (unsigned long long)stats.mismatched);
    CHECK(stats.mismatched == 0 && stats.checked == uint64_t(n));
    if (stats.mismatched != 0) good = 0;
  }
  CHECK(alloc.live == 0);
  *total = n;
  return good;
}

const char* const kRextStreams[] = {
    "hevcrext_400_8.h265",      "hevcrext_400_12.h265",           "hevcrext_420_12.h265",   "hevcrext_420_12_lossless.h265", "hevcrext_422_8.h265",
    "hevcrext_422_10.h265",     "hevcrext_422_10_intra.h265",     "hevcrext_422_12.h265",   "hevcrext_444_8.h265",           "hevcrext_444_8_intra.h265",
    "hevcrext_444_10.h265",     "hevcrext_444_12.h265",           "hevcrext_444_12_ctu64.h265", "hevcrext_422_10_oddsize.h265", "hevcrext_444_10_scaling.h265",
    "hevcrexttool_444_8.h265",  "hevcrexttool_444_8_lossless.h265", "hevcrexttool_444_12.h265", "hevcrexttool_422_10.h265",   "hevcrexttool_420_8.h265",
    "hevcrexttool_400_10.h265", "hevcrexttool_444_10_lossless.h265", "hevcrexttool_444_12_hpo.h265", "hevcrexttool_422_10_sao_lossless.h265",
    "hevcrexttool_400_12_hpo.h265"};

// The decoder of a recorded range extension stream (its first call's parameters).
UniquePointer<IDxvaDecoder> hevc_rext_decoder(TestAllocator& alloc, const DxvaRecordedCall& first, HevcRoute route, uint32_t surfaces = h265::kMaxPics) {
  dxva::PicParamsHevc pp;
  memcpy(&pp, first.params.data(), sizeof pp);
  DxvaDecoderConfig cfg;
  cfg.profile = route.profile;
  cfg.surface = route.surface;
  cfg.width = static_cast<uint32_t>(pp.PicWidthInMinCbsY) << (pp.log2_min_luma_coding_block_size_minus3 + 3);
  cfg.height = static_cast<uint32_t>(pp.PicHeightInMinCbsY) << (pp.log2_min_luma_coding_block_size_minus3 + 3);
  cfg.surfaces = surfaces;
  return create_dxva_decoder(alloc, cfg);
}

// What the range extension profiles take and refuse.
void dxva_hevc_rext_errors() {
  using S = DxvaSurfaceFormat;
  // the surface formats of each profile
  struct Want { DxvaProfile profile; S yes[2]; int n; };
  const Want wants[] = {{DxvaProfile::HevcVldMain12, {S::P016}, 1},          {DxvaProfile::HevcVldMain10_422, {S::Y210, S::Yuy2}, 2},
                        {DxvaProfile::HevcVldMain12_422, {S::Y216}, 1},      {DxvaProfile::HevcVldMain444, {S::Ayuv}, 1},
                        {DxvaProfile::HevcVldMain10Ext, {S::P010}, 1},       {DxvaProfile::HevcVldMain10_444, {S::Y410}, 1},
                        {DxvaProfile::HevcVldMain12_444, {S::Y416}, 1},      {DxvaProfile::HevcVldMain16, {S::Y416}, 1},
                        {DxvaProfile::HevcVldMonochrome, {S::Nv12}, 1},      {DxvaProfile::HevcVldMonochrome10, {S::P010}, 1}};
  for (const Want& w : wants)
    for (uint32_t f = 0; f < kDxvaSurfaceFormatCount; ++f) {
      bool yes = false;
      for (int i = 0; i < w.n; ++i) yes |= w.yes[i] == S(f);
      CHECK(dxva_profile_supports(w.profile, S(f), 128, 80) == yes);
    }
  TestAllocator alloc;
  {
    std::vector<DxvaRecordedCall> calls;
    int total = 0;
    dxva_hevc_rext_stream("hevcrext_422_10.h265", DxvaNaming::D3d11, nullptr, &total, &calls);
    CHECK(calls.size() >= 4);
    if (calls.size() < 4) return;
    const HevcRoute r422{DxvaProfile::HevcVldMain10_422, S::Y210};
    UniquePointer<IDxvaDecoder> d = hevc_rext_decoder(alloc, calls[0], r422);
    CHECK(d && d->profile() == DxvaProfile::HevcVldMain10_422);
    if (!d) return;
    DxvaDecodedPicture o;
    CHECK(d->decode(calls[0].picture(), &o) == DxvaResult::Ok && o.surface_format == S::Y210);
    dxva::PicParamsHevcRangeExt rx;
    memcpy(&rx, calls[0].params.data(), sizeof rx);
    DxvaRecordedCall c = calls[0];
    // a DXVA_PicParams_HEVC without the range extension fields
    c.params.resize(sizeof(dxva::PicParamsHevc));
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid && o.detail != nullptr);
    // another chroma format (4:4:4 into a 4:2:2 profile), 12-bit samples, separate colour planes: Unsupported
    dxva::PicParamsHevcRangeExt bad = rx;
    bad.params.wFormatAndSequenceInfoFlags = static_cast<uint16_t>((bad.params.wFormatAndSequenceInfoFlags & ~3u) | 3u);
    c.params = hevc_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Unsupported && o.detail != nullptr);
    bad = rx;
    bad.params.wFormatAndSequenceInfoFlags = static_cast<uint16_t>((bad.params.wFormatAndSequenceInfoFlags & ~(7u << 3)) | (4u << 3));
    c.params = hevc_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Unsupported);
    bad = rx;
    bad.params.wFormatAndSequenceInfoFlags = static_cast<uint16_t>(bad.params.wFormatAndSequenceInfoFlags | (1u << 2));
    c.params = hevc_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Unsupported);
    // range extension fields out of range: a transform skip block of 64x64, a chroma QP offset list of seven or with an offset of 13, cross-component prediction
    // with 4:2:2, a SAO offset scale above BitDepth - 10
    bad = rx;
    bad.params.dwCodingSettingPicturePropertyFlags |= 1u << dxva::hevc_pic::kTransformSkip;
    bad.log2_max_transform_skip_block_size_minus2 = 4;
    c.params = hevc_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    bad = rx;
    bad.dwRangeExtensionFlags = static_cast<uint16_t>(bad.dwRangeExtensionFlags | (1u << dxva::hevc_rext::kChromaQpOffsetList));
    bad.chroma_qp_offset_list_len_minus1 = 6;
    c.params = hevc_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    bad.chroma_qp_offset_list_len_minus1 = 1;
    bad.cb_qp_offset_list[1] = 13;
    c.params = hevc_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    bad = rx;
    bad.dwRangeExtensionFlags = static_cast<uint16_t>(bad.dwRangeExtensionFlags | (1u << dxva::hevc_rext::kCrossComponentPrediction));
    c.params = hevc_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    bad = rx;
    bad.log2_sao_offset_scale_luma = 1;
    c.params = hevc_bytes(bad);
    CHECK(d->decode(c.picture(), &o) == DxvaResult::Invalid);
    // and the whole stream again after all that
    d->reset();
    for (const DxvaRecordedCall& call : calls) CHECK(d->decode(call.picture(), &o) == DxvaResult::Ok);
    // damaged buffers: no fault, no sanitizer report
    auto make = [&]() { return hevc_rext_decoder(alloc, calls[0], r422); };
    int runs = dxva_damage(make, std::vector<DxvaRecordedCall>(calls.begin(), calls.begin() + 2), calls[2], 41);
    printf("  HEVC range extension damaged buffers: %d decodes", runs);
  }
  {
    uint32_t seed = 43;
    int runs = 0;
    for (const char* s : {"hevcrexttool_444_8.h265", "hevcrexttool_444_12_hpo.h265", "hevcrexttool_400_10.h265", "hevcrext_444_12.h265"}) {
      std::vector<DxvaRecordedCall> more;
      int total = 0;
      dxva_hevc_rext_stream(s, DxvaNaming::D3d11, nullptr, &total, &more);
      if (more.size() < 3) continue;
      dxva::PicParamsHevc pp;
      memcpy(&pp, more[0].params.data(), sizeof pp);
      h265::Sps sps;
      sps.chroma_format_idc = pp.wFormatAndSequenceInfoFlags & 3;
      sps.bit_depth_luma = 8 + ((pp.wFormatAndSequenceInfoFlags >> 3) & 7);
      sps.bit_depth_chroma = 8 + ((pp.wFormatAndSequenceInfoFlags >> 6) & 7);
      sps.persistent_rice = true;   // (a range extension route)
      const HevcRoute r = hevc_route(sps, h265::Pps());
      auto make = [&]() { return hevc_rext_decoder(alloc, more[0], r); };
      runs += dxva_damage(make, std::vector<DxvaRecordedCall>(more.begin(), more.begin() + 2), more[2], seed++);
    }
    printf(", %d more\n", runs);
  }
  CHECK(alloc.live == 0);
}

// DXVA_HEVCREXT_DUMP=<file>: writes the DXVA calls of two range extension streams for the Windows tests (tests/windows/hevcrext_dxva.h, for d3d12videotest.c and
// d3d11videotest.c): a 4:2:2 10-bit one into Y210, a 4:4:4 one with the range extension tools into AYUV and a 4:4:4 12-bit one into Y416, every picture's buffers with Direct3D 11's naming, and a
// 32-bit FNV-1a hash of each picture in display order as planar samples (its own chroma format, 16-bit little-endian words above eight bits) - written only when
// every picture agreed with its hash.
void hevcrext_dump_windows_data(const char* path) {
  FILE* out = fopen(path, "w");
  CHECK(out != nullptr);
  if (!out) return;
  fprintf(out, "/* Generated by tests/DxvaDecoderTest.cpp (DXVA_HEVCREXT_DUMP=tests/windows/hevcrext_dxva.h tests/run_host_tests.sh DxvaDecoderTest): the DXVA calls of HEVC\n"
               " * range extension streams of tests/data as ffmpeg's dxva2_hevc.c makes them (DXVA_PicParams_HEVC_RangeExt; Direct3D 11 naming: a surface index is a slot of the\n"
               " * picture buffer, 17 of them), and a 32-bit FNV-1a hash of every picture in display order (planar, its own chroma format, the coded size), checked against\n"
               " * the hash every picture carries. Do not edit. */\n#pragma once\n");
  fprintf(out, "typedef struct HevcRextPicture { unsigned pp, pp_size, qm, qm_size, slices, slices_size, bits, bits_size, surface, display; } HevcRextPicture;\n");
  fprintf(out, "/* profile: hardware::DxvaProfile; format: the DXGI_FORMAT of the surfaces; chroma: chroma_format_idc; depth: the bits of a sample */\n");
  fprintf(out, "typedef struct HevcRextStream { const char *name; unsigned profile, format, chroma, depth, width, height, surfaces, count; const unsigned char *bytes; "
               "const HevcRextPicture *pictures; const unsigned *fnv; } HevcRextStream;\n");
  struct Dump { const char* stream; HevcRoute route; unsigned dxgi; };
  const Dump dumps[] = {{"hevcrext_422_10", {DxvaProfile::HevcVldMain10_422, DxvaSurfaceFormat::Y210}, 108 /* DXGI_FORMAT_Y210 */},
                        {"hevcrexttool_444_8", {DxvaProfile::HevcVldMain444, DxvaSurfaceFormat::Ayuv}, 100 /* DXGI_FORMAT_AYUV */},
                        {"hevcrext_444_12", {DxvaProfile::HevcVldMain12_444, DxvaSurfaceFormat::Y416}, 102 /* DXGI_FORMAT_Y416 */}};
  std::vector<std::string> lines;
  for (const Dump& dump : dumps) {
    std::vector<DxvaRecordedCall> calls;
    std::vector<std::pair<size_t, std::vector<uint8_t>>> display;
    int total = 0;
    const int good = dxva_hevc_rext_stream(std::string(dump.stream) + ".h265", DxvaNaming::D3d11, &dump.route, &total, &calls, &display);
    CHECK(good == total && display.size() == calls.size());
    if (good != total || display.size() != calls.size()) break;
    std::vector<uint8_t> bytes;
    std::vector<unsigned> shown(calls.size(), 0);   // each picture's place in display order
    for (size_t i = 0; i < display.size(); ++i)
      if (display[i].first < shown.size()) shown[display[i].first] = unsigned(i);
    fprintf(out, "static const HevcRextPicture g_%s_pictures[%zu] = {\n", dump.stream, calls.size());
    for (size_t i = 0; i < calls.size(); ++i) {
      const DxvaRecordedCall& c = calls[i];
      dxva::PicParamsHevc pp;
      memcpy(&pp, c.params.data(), sizeof pp);
      unsigned at[4];
      const std::vector<uint8_t>* parts[4] = {&c.params, &c.qm, &c.slices, &c.bitstream};
      for (int k = 0; k < 4; ++k) {
        at[k] = unsigned(bytes.size());
        bytes.insert(bytes.end(), parts[k]->begin(), parts[k]->end());
        while (bytes.size() % 4) bytes.push_back(0);
      }
      fprintf(out, "    {%u, %zu, %u, %zu, %u, %zu, %u, %zu, %u, %u},\n", at[0], c.params.size(), at[1], c.qm.size(), at[2], c.slices.size(), at[3], c.bitstream.size(),
              unsigned(pp.CurrPic & 0x7F), shown[i]);
    }
    fprintf(out, "};\nstatic const unsigned g_%s_fnv[%zu] = {", dump.stream, display.size());
    for (size_t i = 0; i < display.size(); ++i) {
      uint32_t h = 0x811c9dc5u;
      for (uint8_t b : display[i].second) { h ^= b; h *= 0x01000193u; }
      fprintf(out, "%s0x%08xu", i ? ", " : "", h);
    }
    fprintf(out, "};\nstatic const unsigned char g_%s_bytes[%zu] __attribute__((aligned(4))) = {", dump.stream, bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) fprintf(out, "%s%u", i % 40 == 0 ? "\n    " : "", bytes[i]), fputc(i + 1 < bytes.size() ? ',' : '\n', out);
    fprintf(out, "};\n");
    dxva::PicParamsHevc first;
    memcpy(&first, calls[0].params.data(), sizeof first);
    const unsigned w = unsigned(first.PicWidthInMinCbsY) << (first.log2_min_luma_coding_block_size_minus3 + 3);
    const unsigned h = unsigned(first.PicHeightInMinCbsY) << (first.log2_min_luma_coding_block_size_minus3 + 3);
    char line[500];
    snprintf(line, sizeof line, "    {\"%s\", %u, %u, %u, %u, %u, %u, %u, %zu, g_%s_bytes, g_%s_pictures, g_%s_fnv},\n", dump.stream, unsigned(dump.route.profile), dump.dxgi,
             unsigned(first.wFormatAndSequenceInfoFlags & 3), 8 + ((first.wFormatAndSequenceInfoFlags >> 3) & 7), w, h, unsigned(h265::kMaxPics), calls.size(), dump.stream,
             dump.stream, dump.stream);
    lines.push_back(line);
  }
  fprintf(out, "static const HevcRextStream g_hevcrext_streams[%zu] = {\n", lines.size());
  for (const std::string& l : lines) fputs(l.c_str(), out);
  fprintf(out, "};\n");
  fclose(out);
  printf("  wrote %s\n", path);
}

}  // namespace

static void dxva_test_hevc_rext() {
  int good = 0, total = 0;
  for (DxvaNaming naming : {DxvaNaming::D3d11, DxvaNaming::D3d12Shuffled, DxvaNaming::D3d12OutputKey})
    for (const char* s : kRextStreams) {
      int t = 0;
      good += dxva_hevc_rext_stream(s, naming, nullptr, &t);
      total += t;
    }
  printf("DXVA HEVC range extensions: %d of %d pictures bit-exact with their hashes and FFmpeg's (%zu streams, 3 ways of naming surfaces)\n", good, total,
         sizeof kRextStreams / sizeof kRextStreams[0]);
  // Other routes: 8-bit pictures through the deeper profiles (4:2:2 into Y210 and Y216, 4:4:4 into Y410, Y416 and Main16, 4:2:0 into P016, monochrome into P016),
  // 8- to 10-bit 4:2:0 into Main10_Ext.
  struct Other { const char* stream; HevcRoute route; };
  using S = DxvaSurfaceFormat;
  const Other others[] = {{"hevcrext_422_8.h265", {DxvaProfile::HevcVldMain10_422, S::Y210}},     {"hevcrext_422_8.h265", {DxvaProfile::HevcVldMain12_422, S::Y216}},
                          {"hevcrext_422_10.h265", {DxvaProfile::HevcVldMain12_422, S::Y216}},    {"hevcrext_444_8.h265", {DxvaProfile::HevcVldMain10_444, S::Y410}},
                          {"hevcrext_444_10.h265", {DxvaProfile::HevcVldMain12_444, S::Y416}},    {"hevcrext_444_12.h265", {DxvaProfile::HevcVldMain16, S::Y416}},
                          {"hevcrexttool_444_8.h265", {DxvaProfile::HevcVldMain16, S::Y416}},     {"hevcrexttool_420_8.h265", {DxvaProfile::HevcVldMain12, S::P016}},
                          {"hevcrexttool_420_8.h265", {DxvaProfile::HevcVldMain10Ext, S::P010}},  {"hevcrext_400_8.h265", {DxvaProfile::HevcVldMonochrome10, S::P010}},
                          {"hevcrext_400_8.h265", {DxvaProfile::HevcVldMain12, S::P016}},         {"hevcrexttool_400_10.h265", {DxvaProfile::HevcVldMain12, S::P016}},
                          {"hevc_bframes.h265", {DxvaProfile::HevcVldMain12, S::P016}},           {"hevc10_bframes.h265", {DxvaProfile::HevcVldMain10Ext, S::P010}}};
  good = total = 0;
  for (const Other& o : others) {
    int t = 0;
    good += dxva_hevc_rext_stream(o.stream, DxvaNaming::D3d12Shuffled, &o.route, &t);
    total += t;
  }
  printf("DXVA HEVC range extensions, other profiles and surface formats: %d of %d pictures\n", good, total);
  dxva_hevc_rext_errors();
  if (const char* dump = getenv("DXVA_HEVCREXT_DUMP")) hevcrext_dump_windows_data(dump);
}
