// DxvaDecoderTest: the MPEG-4 Part 2 cases (D3D11_DECODER_PROFILE_MPEG4PT2_VLD_SIMPLE, _ADVSIMPLE_NOGMC and _ADVSIMPLE_GMC).
//
// ffmpeg has no DXVA MPEG-4 hwaccel, so the test fills DXVA_PicParams_MPEG4_PART2 the way the structure's fields are defined (the Windows SDK's dxva.h and the DXVA
// specification for MPEG-4 Part 2; the fields are the bitstream's, TRB / TRD the distances of direct mode as ffmpeg derives them from the plane times), from the
// layer and plane headers the program parses (Mpeg4Headers.hpp), DXVA_QmatrixData from the layer's matrices, and hands each plane over in one of four ways: from
// its start code (the decoder reads the header), from just after it with wMBbitOffset at the first macroblock (the decoder takes the parameters' header), split
// into two pieces with a gap between them in the buffer, and named by surface keys that move from picture to picture. The pictures, put in display order and
// cropped, must be ffmpeg's decode (tests/data/m4vdec_*.yuv and dxva_mpeg4_*.yuv, decoded with -threads 1) exactly; the short video header (H.263 baseline) streams likewise.
#include "Hardware/VideoCodec/Dxva/DxvaTypesMpeg4.hpp"
#include "Hardware/VideoCodec/Software/Mpeg4Headers.hpp"

namespace {

// One plane of the stream, as the program has parsed it.
struct M4Plane {
  mpeg4::Vol vol;
  mpeg4::VopHeader header;
  mpeg4::ShortHeader short_header;
  bool is_short = false;
  size_t start = 0, end = 0;   // [start code, end) in the stream
  int64_t pp_time = 0, pb_time = 0, pp_field_time = 0, pb_field_time = 0;
};

struct M4Stream {
  std::vector<uint8_t> bytes;
  std::vector<M4Plane> planes;
  uint32_t width = 0, height = 0;
  bool has_b = false;
  bool ok = true;
};

static M4Stream m4_parse(const std::vector<uint8_t>& d) {
  M4Stream s;
  s.bytes = d;
  if (mpeg4::is_short_header(ConstByteView(d.data(), d.size()))) {
    // H.263: pictures from picture start code to picture start code (byte aligned in these streams).
    std::vector<size_t> starts;
    for (size_t i = 0; i + 3 <= d.size(); ++i)
      if (mpeg4::is_short_header(ConstByteView(d.data() + i, d.size() - i))) starts.push_back(i);
    for (size_t k = 0; k < starts.size(); ++k) {
      M4Plane p;
      p.is_short = true;
      p.start = starts[k];
      p.end = k + 1 < starts.size() ? starts[k + 1] : d.size();
      if (!mpeg4::parse_short_header(ConstByteView(d.data() + p.start, p.end - p.start), nullptr, &p.short_header)) {
        s.ok = false;
        break;
      }
      s.width = p.short_header.width;
      s.height = p.short_header.height;
      s.planes.push_back(p);
    }
    return s;
  }
  std::vector<size_t> codes;
  for (size_t i = 0; i + 3 < d.size(); ++i)
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) codes.push_back(i);
  mpeg4::Vol vol;
  bool have_vol = false;
  uint8_t profile = 0;
  // The plane times, as ffmpeg's decode_vop_header keeps them.
  int64_t time_base = 0, last_time_base = 0, time = 0, last_non_b_time = 0, pp_time = 0, pb_time = 0, t_frame = 0;
  for (size_t k = 0; k < codes.size(); ++k) {
    const size_t at = codes[k], end = k + 1 < codes.size() ? codes[k + 1] : d.size();
    const uint8_t code = d[at + 3];
    const ConstByteView body(d.data() + at + 4, end - at - 4);
    if (code == 0xB0 && body.size() >= 1) profile = body[0];
    if (code >= 0x20 && code <= 0x2F) {
      if (!mpeg4::parse_vol_header(body, profile, &vol)) {
        s.ok = false;
        return s;
      }
      have_vol = true;
      s.width = vol.width;
      s.height = vol.height;
    }
    if (code != 0xB6 || !have_vol) continue;
    M4Plane p;
    p.vol = vol;
    p.start = at;
    p.end = end;
    if (!mpeg4::parse_vop_header(body, vol, &p.header)) {
      s.ok = false;
      return s;
    }
    const mpeg4::VopHeader& h = p.header;
    const int64_t resolution = vol.time_increment_resolution;
    if (h.type != mpeg4::VopType::B) {
      last_time_base = time_base;
      time_base += h.modulo_time_base;
      time = time_base * resolution + h.time_increment;
      pp_time = time - last_non_b_time;
      last_non_b_time = time;
    } else {
      s.has_b = true;
      time = (last_time_base + h.modulo_time_base) * resolution + h.time_increment;
      pb_time = pp_time - (last_non_b_time - time);
      if (t_frame == 0) t_frame = pb_time;
      if (t_frame == 0) t_frame = 1;
      auto rdiv = [](int64_t a, int64_t b) { return (a >= 0 ? a + (b >> 1) : a - (b >> 1)) / b; };
      p.pp_field_time = (rdiv(last_non_b_time, t_frame) - rdiv(last_non_b_time - pp_time, t_frame)) * 2;
      p.pb_field_time = (rdiv(time, t_frame) - rdiv(last_non_b_time - pp_time, t_frame)) * 2;
      if (p.pp_field_time <= p.pb_field_time || p.pb_field_time <= 1) {
        p.pb_field_time = 2;
        p.pp_field_time = 4;
      }
      p.pp_time = pp_time;
      p.pb_time = pb_time;
    }
    if (!h.coded) continue;   // (not coded: nothing to decode or show, as with ffmpeg)
    s.planes.push_back(p);
  }
  return s;
}

template <class T>
static ConstByteView m4_bytes(const T& value) {
  // (the test fills the DXVA structures the way a program does and hands their bytes over; the structures are packed byte arrays)
  return ConstByteView(reinterpret_cast<const uint8_t*>(&value), sizeof value);
}

struct M4Buffers {
  dxva::PicParamsMpeg4Part2 pp = {};
  dxva::QmatrixMpeg2 qm = {};
  bool send_qmatrix = true;
  std::vector<dxva::SliceInfoMpeg2> slices;
  std::vector<uint8_t> bitstream;
  DxvaPicture view() const {
    DxvaPicture p;
    p.picture_parameters = m4_bytes(pp);
    if (send_qmatrix) p.quantization_matrix = m4_bytes(qm);
    p.slice_control = ConstByteView(reinterpret_cast<const uint8_t*>(slices.data()), slices.size() * sizeof(dxva::SliceInfoMpeg2));
    p.bitstream = ConstByteView(bitstream.data(), bitstream.size());
    return p;
  }
};

enum M4Mode { kM4StartCode = 0, kM4AfterStartCode = 1, kM4Split = 2 };

static M4Buffers m4_fill(const M4Stream& s, const M4Plane& p, uint16_t cur, uint16_t fwd, uint16_t bwd, uint16_t feedback, M4Mode mode) {
  M4Buffers b;
  dxva::PicParamsMpeg4Part2& pp = b.pp;
  pp.wDecodedPictureIndex = pp.wDeblockedPictureIndex = cur;
  pp.StatusReportFeedbackNumber = feedback;
  if (p.is_short) {
    const mpeg4::ShortHeader& h = p.short_header;
    pp.short_video_header = 1;
    pp.vop_coding_type = h.inter ? 1 : 0;
    pp.vop_quant = h.quant;
    pp.wForwardRefPictureIndex = h.inter ? fwd : 0xFFFF;
    pp.wBackwardRefPictureIndex = 0xFFFF;
    pp.vop_time_increment_resolution = 30000;
    pp.wPicFlagBitFields = dxva::kMpeg4PicVopCoded | dxva::kMpeg4PicResyncMarkerDisable;
    pp.video_object_layer_verid = 1;
    pp.vop_width = static_cast<uint16_t>(h.width);
    pp.vop_height = static_cast<uint16_t>(h.height);
    pp.wFcodeBitFields = 1;
    b.send_qmatrix = false;
  } else {
    const mpeg4::Vol& v = p.vol;
    const mpeg4::VopHeader& h = p.header;
    const bool b_plane = h.type == mpeg4::VopType::B;
    pp.vop_coding_type = static_cast<uint8_t>(h.type);
    pp.vop_quant = h.quant;
    pp.wForwardRefPictureIndex = h.type != mpeg4::VopType::I ? fwd : 0xFFFF;
    pp.wBackwardRefPictureIndex = b_plane ? bwd : 0xFFFF;
    pp.vop_time_increment_resolution = static_cast<uint16_t>(v.time_increment_resolution);
    if (b_plane) {
      pp.TRB[0] = static_cast<uint32_t>(p.pb_time);
      pp.TRD[0] = static_cast<uint32_t>(p.pp_time);
      pp.TRB[1] = static_cast<uint32_t>(p.pb_field_time);
      pp.TRD[1] = static_cast<uint32_t>(p.pp_field_time);
    }
    uint16_t f = 0;
    if (v.interlaced) f |= dxva::kMpeg4PicInterlaced;
    if (v.quant_type_mpeg) f |= dxva::kMpeg4PicQuantType;
    if (v.quarter_sample) f |= dxva::kMpeg4PicQuarterSample;
    if (v.resync_marker_disable) f |= dxva::kMpeg4PicResyncMarkerDisable;
    if (v.data_partitioned) f |= dxva::kMpeg4PicDataPartitioned;
    if (v.reversible_vlc) f |= dxva::kMpeg4PicReversibleVlc;
    if (h.coded) f |= dxva::kMpeg4PicVopCoded;
    if (h.rounding_type) f |= dxva::kMpeg4PicRoundingType;
    f |= static_cast<uint16_t>(h.intra_dc_vlc_thr << dxva::kMpeg4PicIntraDcVlcThrShift);
    if (h.top_field_first) f |= dxva::kMpeg4PicTopFieldFirst;
    if (h.alternate_vertical_scan) f |= dxva::kMpeg4PicAlternateVerticalScan;
    pp.wPicFlagBitFields = f;
    pp.profile_and_level_indication = v.profile_and_level;
    pp.video_object_layer_verid = v.verid;
    pp.vop_width = static_cast<uint16_t>(v.width);
    pp.vop_height = static_cast<uint16_t>(v.height);
    if (v.gmc) pp.wSpriteBitFields = static_cast<uint16_t>(2 | v.warping_points << dxva::kMpeg4SpritePointsShift | v.warping_accuracy << dxva::kMpeg4SpriteAccuracyShift);
    if (h.type == mpeg4::VopType::S)
      for (int i = 0; i < 3; ++i)
        for (int c = 0; c < 2; ++c) pp.warping_mv[i][c] = static_cast<int16_t>(h.sprite_trajectory[i][c]);
    pp.wFcodeBitFields = static_cast<uint8_t>((h.type != mpeg4::VopType::I ? h.fcode_forward : 0) | (b_plane ? h.fcode_backward : 0) << 3);
    for (int m = 0; m < 2; ++m) {
      b.qm.bNewQmatrix[m] = 1;
      for (int i = 0; i < 64; ++i) b.qm.Qmatrix[m][i] = m ? v.non_intra_matrix[i] : v.intra_matrix[i];
    }
  }
  const size_t skip = !p.is_short && mode != kM4StartCode ? 4 : 0;   // (the start code)
  std::vector<uint8_t> data(s.bytes.begin() + long(p.start + skip), s.bytes.begin() + long(p.end));
  const uint32_t mb_offset = p.is_short ? uint32_t(p.short_header.header_bits) : uint32_t(p.header.header_bits) + (skip ? 0 : 32);
  if (mode == kM4Split && data.size() > 8) {
    // Two pieces, with a gap between them in the buffer.
    const size_t cut = data.size() / 2;
    dxva::SliceInfoMpeg2 a = {}, c = {};
    a.dwSliceDataLocation = 0;
    a.dwSliceBitsInBuffer = static_cast<uint32_t>(8 * cut);
    a.wMBbitOffset = static_cast<uint16_t>(mb_offset);
    a.wQuantizerScaleCode = pp.vop_quant;
    b.bitstream.assign(data.begin(), data.begin() + long(cut));
    b.bitstream.insert(b.bitstream.end(), 13, 0xEE);
    c.dwSliceDataLocation = static_cast<uint32_t>(b.bitstream.size());
    c.dwSliceBitsInBuffer = static_cast<uint32_t>(8 * (data.size() - cut));
    c.wQuantizerScaleCode = pp.vop_quant;
    b.bitstream.insert(b.bitstream.end(), data.begin() + long(cut), data.end());
    b.slices = {a, c};
  } else {
    dxva::SliceInfoMpeg2 a = {};
    a.dwSliceBitsInBuffer = static_cast<uint32_t>(8 * data.size());
    a.wMBbitOffset = static_cast<uint16_t>(mb_offset);
    a.wQuantizerScaleCode = pp.vop_quant;
    a.wNumberMBsInSlice = static_cast<uint16_t>(((pp.vop_width + 15) / 16) * ((pp.vop_height + 15) / 16));
    b.bitstream = data;
    b.slices = {a};
  }
  while (b.bitstream.size() % 128) b.bitstream.push_back(0);
  return b;
}

static std::vector<uint8_t> m4_i420(const VideoFrame& f) {
  std::vector<uint8_t> out;
  const uint32_t w = f.format.width, h = f.format.height;
  for (uint32_t y = 0; y < h; ++y) out.insert(out.end(), f.plane[0] + size_t(y) * f.stride[0], f.plane[0] + size_t(y) * f.stride[0] + w);
  for (int c = 0; c < 2; ++c)
    for (uint32_t y = 0; y < h / 2; ++y)
      for (uint32_t x = 0; x < w / 2; ++x) out.push_back(f.plane[1][size_t(y) * f.stride[1] + 2 * x + c]);
  return out;
}

// The program's surface pool (the two anchors stay out of it).
struct M4Program {
  uint16_t pool = 8, next = 0;
  int older = -1, newer = -1;
  uint16_t pick() {
    for (;;) {
      const uint16_t s = next;
      next = static_cast<uint16_t>((next + 1) % pool);
      if (s != older && s != newer) return s;
    }
  }
};

static DxvaProfile m4_profile(const M4Stream& s) {
  bool asp = s.has_b, gmc = false;
  for (const M4Plane& p : s.planes) {
    if (p.is_short) continue;
    asp = asp || p.vol.interlaced || p.vol.quarter_sample || p.vol.quant_type_mpeg;
    gmc = gmc || p.vol.gmc;
  }
  return gmc ? DxvaProfile::Mpeg4Pt2AdvSimpleGmc : asp ? DxvaProfile::Mpeg4Pt2AdvSimpleNoGmc : DxvaProfile::Mpeg4Pt2Simple;
}

struct M4Run {
  std::vector<std::vector<uint8_t>> decoded;
  std::vector<DxvaResult> results;
};

static M4Run m4_run(TestAllocator& a, DxvaProfile profile, const M4Stream& s, M4Mode mode, bool keys) {
  M4Run run;
  DxvaDecoderConfig config;
  config.profile = profile;
  config.width = s.width;
  config.height = s.height;
  config.surfaces = 8;
  config.output = PixelFormat::Nv12;
  UniquePointer<IDxvaDecoder> dec = create_dxva_decoder(a, config);
  CHECK(dec);
  if (!dec) return run;
  M4Program prog;
  uint32_t perm[8] = {0, 1, 2, 3, 4, 5, 6, 7}, key_table[8];
  for (size_t n = 0; n < s.planes.size(); ++n) {
    const M4Plane& p = s.planes[n];
    const bool b_plane = !p.is_short && p.header.type == mpeg4::VopType::B;
    const int cur = prog.pick();
    const int fwd = b_plane ? prog.older : prog.newer, bwd = prog.newer;
    if (keys) {
      for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t j = static_cast<uint32_t>((n * 5 + i * 3) % 8);
        const uint32_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
      }
      for (uint32_t k = 0; k < 8; ++k) key_table[perm[k]] = 1000 + k;
    }
    auto index = [&](int x) { return static_cast<uint16_t>(x < 0 ? 0xFFFF : keys ? perm[x] : static_cast<uint32_t>(x)); };
    const M4Buffers b = m4_fill(s, p, index(cur), index(fwd), index(bwd), uint16_t(n + 1), mode);
    DxvaPicture view = b.view();
    if (keys) view.surface_keys = ArrayView<const uint32_t>(key_table, 8);
    DxvaDecodedPicture out;
    const DxvaResult r = dec->decode(view, &out);
    run.results.push_back(r);
    if (r == DxvaResult::Ok || r == DxvaResult::MissingReference) {
      CHECK(out.surface == index(cur));
      CHECK(out.frame.format.pixel_format == PixelFormat::Nv12 && out.frame.format.width == (s.width + 15) / 16 * 16 && out.frame.format.height == (s.height + 15) / 16 * 16);
      run.decoded.push_back(m4_i420(out.frame));
    } else {
      printf("    plane %zu: result %d (%s)\n", n, int(r), out.detail ? out.detail : "");
      run.decoded.push_back({});
    }
    if (!b_plane) {
      prog.older = prog.newer;
      prog.newer = cur;
    }
  }
  return run;
}

static void m4_test_stream(const char* file, const char* reference) {
  const std::vector<uint8_t> bytes = read_file(std::string("tests/data/") + file);
  const std::vector<uint8_t> ref = read_file(std::string("tests/data/") + reference);
  CHECK(!bytes.empty() && !ref.empty());
  if (bytes.empty() || ref.empty()) return;
  const M4Stream s = m4_parse(bytes);
  CHECK(s.ok && !s.planes.empty());
  if (!s.ok || s.planes.empty()) return;
  const DxvaProfile profile = m4_profile(s);
  TestAllocator a;
  {
    const M4Run run = m4_run(a, profile, s, kM4StartCode, false);
    for (DxvaResult r : run.results) CHECK(r == DxvaResult::Ok);
    // Display order, cropped, against ffmpeg's frames.
    const uint32_t w = s.width, h = s.height, cw = (w + 15) / 16 * 16, ch = (h + 15) / 16 * 16;
    const size_t frame_bytes = size_t(w) * h + 2 * size_t((w + 1) / 2) * ((h + 1) / 2);
    int max_diff = 0, frames = 0;
    auto compare = [&](size_t i) {
      const size_t offset = size_t(frames) * frame_bytes;
      ++frames;
      const std::vector<uint8_t>& coded = run.decoded[i];
      if (coded.empty() || offset + frame_bytes > ref.size()) {
        max_diff = 255;
        return;
      }
      const size_t plane_offset[3] = {0, size_t(cw) * ch, size_t(cw) * ch + size_t(cw / 2) * (ch / 2)};
      size_t p = offset;
      for (int c = 0; c < 3; ++c) {
        const uint32_t pw = c ? (w + 1) / 2 : w, ph = c ? (h + 1) / 2 : h, stride = c ? cw / 2 : cw;
        for (uint32_t y = 0; y < ph; ++y)
          for (uint32_t x = 0; x < pw; ++x) {
            const int d = abs(int(coded[plane_offset[c] + size_t(y) * stride + x]) - int(ref[p++]));
            if (d > max_diff) max_diff = d;
          }
      }
    };
    int held = -1;
    for (size_t i = 0; i < s.planes.size() && i < run.decoded.size(); ++i) {
      if (!s.planes[i].is_short && s.planes[i].header.type == mpeg4::VopType::B) {
        compare(i);
      } else {
        if (held >= 0) compare(size_t(held));
        held = static_cast<int>(i);
      }
    }
    if (held >= 0) compare(size_t(held));
    const char* names[] = {"Simple", "AdvSimple", "AdvSimple GMC"};
    printf("  %-22s %-13s %zu planes; %d frames, largest difference to ffmpeg %d\n", file,
           names[profile == DxvaProfile::Mpeg4Pt2Simple ? 0 : profile == DxvaProfile::Mpeg4Pt2AdvSimpleNoGmc ? 1 : 2], s.planes.size(), frames, max_diff);
    CHECK(size_t(frames) * frame_bytes == ref.size());
    CHECK(max_diff == 0);
    // The other ways of handing a plane over give the same pictures.
    CHECK(m4_run(a, profile, s, kM4AfterStartCode, false).decoded == run.decoded);
    CHECK(m4_run(a, profile, s, kM4Split, false).decoded == run.decoded);
    CHECK(m4_run(a, profile, s, kM4StartCode, true).decoded == run.decoded);
    // The GMC profile takes every stream the others take.
    if (profile != DxvaProfile::Mpeg4Pt2AdvSimpleGmc) CHECK(m4_run(a, DxvaProfile::Mpeg4Pt2AdvSimpleGmc, s, kM4StartCode, false).decoded == run.decoded);
  }
  CHECK(a.live == 0);
}

static void m4_test_damaged(const char* file) {
  const std::vector<uint8_t> bytes = read_file(std::string("tests/data/") + file);
  const M4Stream s = m4_parse(bytes);
  CHECK(s.ok && !s.planes.empty());
  if (!s.ok || s.planes.empty()) return;
  TestAllocator a;
  int invalid = 0, decoded = 0;
  {
    DxvaDecoderConfig config;
    config.profile = DxvaProfile::Mpeg4Pt2AdvSimpleGmc;
    config.width = s.width;
    config.height = s.height;
    config.surfaces = 8;
    config.output = PixelFormat::Nv12;
    UniquePointer<IDxvaDecoder> dec = create_dxva_decoder(a, config);
    CHECK(dec);
    if (!dec) return;
    M4Program prog;
    for (size_t n = 0; n < s.planes.size(); ++n) {
      const M4Plane& p = s.planes[n];
      const bool b_plane = !p.is_short && p.header.type == mpeg4::VopType::B;
      const int cur = prog.pick();
      const int fwd = b_plane ? prog.older : prog.newer, bwd = prog.newer;
      const M4Buffers good = m4_fill(s, p, uint16_t(cur), uint16_t(fwd < 0 ? 0xFFFF : fwd), uint16_t(bwd < 0 ? 0xFFFF : bwd), 1, M4Mode(n % 2));
      DxvaDecodedPicture out;
      for (int variant = 0; variant < 32; ++variant) {
        M4Buffers b = good;
        std::vector<uint8_t> pp(reinterpret_cast<const uint8_t*>(&b.pp), reinterpret_cast<const uint8_t*>(&b.pp) + sizeof b.pp);
        std::vector<uint8_t> qm(reinterpret_cast<const uint8_t*>(&b.qm), reinterpret_cast<const uint8_t*>(&b.qm) + sizeof b.qm);
        std::vector<uint8_t> sc(reinterpret_cast<const uint8_t*>(b.slices.data()), reinterpret_cast<const uint8_t*>(b.slices.data()) + b.slices.size() * sizeof(dxva::SliceInfoMpeg2));
        std::vector<uint8_t>& bs = b.bitstream;
        std::vector<uint8_t>* target = variant % 4 == 0 ? &pp : variant % 4 == 1 ? &qm : variant % 4 == 2 ? &sc : &bs;
        const size_t salt = n * 7919 + size_t(variant) * 104729;
        if (variant < 12) target->resize(target->size() * size_t(variant / 4 + 1) / 4);
        else if (!target->empty()) for (int k = 0; k < 1 + variant % 3; ++k) (*target)[(salt + size_t(k) * 31) % target->size()] ^= uint8_t(1u << ((salt + size_t(k)) % 8));
        DxvaPicture v;
        v.picture_parameters = ConstByteView(pp.data(), pp.size());
        if (b.send_qmatrix) v.quantization_matrix = ConstByteView(qm.data(), qm.size());
        v.slice_control = ConstByteView(sc.data(), sc.size());
        v.bitstream = ConstByteView(bs.data(), bs.size());
        const DxvaResult r = dec->decode(v, &out);
        if (variant < 12 && target == &pp) CHECK(r == DxvaResult::Invalid);
        if (variant < 12 && target == &qm && b.send_qmatrix) CHECK(r == DxvaResult::Invalid);
        if (variant < 12 && target == &sc && sc.size() % sizeof(dxva::SliceInfoMpeg2) != 0) CHECK(r == DxvaResult::Invalid);
        if (r == DxvaResult::Invalid || r == DxvaResult::Unsupported) ++invalid;
        else ++decoded;
        if (r == DxvaResult::Ok || r == DxvaResult::MissingReference) CHECK(out.frame.plane[0] != nullptr);
      }
      M4Buffers b = good;
      b.pp.wDecodedPictureIndex = 8;
      CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
      b = good;
      b.slices[0].dwSliceDataLocation = 0xFFFFFFF0u;
      CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
      b = good;
      b.slices[0].wMBbitOffset = 0xFFFF;
      CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid || good.slices[0].dwSliceBitsInBuffer >= 0xFFFF);
      const DxvaResult r = dec->decode(good.view(), &out);
      CHECK(r == DxvaResult::Ok || r == DxvaResult::MissingReference);
      if (!b_plane) {
        prog.older = prog.newer;
        prog.newer = cur;
      }
    }
  }
  printf("  %-22s damaged buffers: %d refused, %d decoded\n", file, invalid, decoded);
  CHECK(invalid > 0 && decoded > 0 && a.live == 0);
}

static void m4_test_refusals() {
  const M4Stream s = m4_parse(read_file("tests/data/m4vdec_bframes.m4v"));
  const M4Stream g = m4_parse(read_file("tests/data/dxva_mpeg4_gmc.m4v"));
  CHECK(s.ok && g.ok && s.planes.size() > 3 && !g.planes.empty());
  if (!s.ok || !g.ok || s.planes.size() <= 3 || g.planes.empty()) return;
  TestAllocator a;
  {
    DxvaDecoderConfig config{DxvaProfile::Mpeg4Pt2Simple, s.width, s.height, 4, PixelFormat::Nv12};
    UniquePointer<IDxvaDecoder> simple = create_dxva_decoder(a, config);
    config.profile = DxvaProfile::Mpeg4Pt2AdvSimpleNoGmc;
    UniquePointer<IDxvaDecoder> asp = create_dxva_decoder(a, config);
    CHECK(simple && asp && asp->profile() == DxvaProfile::Mpeg4Pt2AdvSimpleNoGmc);
    CHECK(!create_dxva_decoder(a, DxvaDecoderConfig{DxvaProfile::Mpeg4Pt2Simple, 64, 64, 4, PixelFormat::P010}));
    if (!simple || !asp) return;
    DxvaDecodedPicture out;
    size_t bi = 0, pi = 0;
    while (bi < s.planes.size() && s.planes[bi].header.type != mpeg4::VopType::B) ++bi;
    while (pi < s.planes.size() && s.planes[pi].header.type != mpeg4::VopType::P) ++pi;
    CHECK(bi < s.planes.size() && pi < s.planes.size());
    if (bi >= s.planes.size() || pi >= s.planes.size()) return;
    const M4Buffers ib = m4_fill(s, s.planes[0], 0, 0xFFFF, 0xFFFF, 1, kM4StartCode);
    CHECK(asp->decode(ib.view(), &out) == DxvaResult::Ok);
    // A B plane with the Simple profile; GMC without the GMC profile; static sprites; reduced resolution; post-processing.
    const M4Buffers bb = m4_fill(s, s.planes[bi], 1, 0, 2, 2, kM4StartCode);
    CHECK(simple->decode(bb.view(), &out) == DxvaResult::Invalid);
    const M4Buffers gi = m4_fill(g, g.planes[0], 0, 0xFFFF, 0xFFFF, 1, kM4StartCode);
    CHECK(asp->decode(gi.view(), &out) == DxvaResult::Invalid);
    M4Buffers c = ib;
    c.pp.wSpriteBitFields = 1;
    CHECK(asp->decode(c.view(), &out) == DxvaResult::Unsupported && out.detail);
    c = ib;
    c.pp.wPicFlagBitFields |= dxva::kMpeg4PicReducedResolution;
    CHECK(asp->decode(c.view(), &out) == DxvaResult::Unsupported);
    c = ib;
    c.pp.wPicFlagBitFields |= 1;
    CHECK(asp->decode(c.view(), &out) == DxvaResult::Unsupported);
    // The header in the bitstream is not the parameters' (another quantiser); TRB not below TRD; a version 1 layer with quarter samples.
    c = ib;
    c.pp.vop_quant = static_cast<uint8_t>(c.pp.vop_quant == 31 ? 30 : c.pp.vop_quant + 1);
    CHECK(asp->decode(c.view(), &out) == DxvaResult::Invalid && out.detail);
    c = bb;
    c.pp.TRB[0] = c.pp.TRD[0];
    CHECK(asp->decode(c.view(), &out) == DxvaResult::Invalid);
    c = ib;
    c.pp.video_object_layer_verid = 1;
    c.pp.wPicFlagBitFields |= dxva::kMpeg4PicQuarterSample;
    CHECK(asp->decode(c.view(), &out) == DxvaResult::Invalid);
    // A P plane whose reference holds nothing: MissingReference; into the surface it predicts from: the same picture as elsewhere.
    M4Buffers pb = m4_fill(s, s.planes[pi], 1, 3, 0xFFFF, 3, kM4StartCode);
    CHECK(asp->decode(pb.view(), &out) == DxvaResult::MissingReference && out.surface == 1);
    pb = m4_fill(s, s.planes[pi], 1, 0, 0xFFFF, 4, kM4StartCode);
    CHECK(asp->decode(pb.view(), &out) == DxvaResult::Ok);
    const std::vector<uint8_t> elsewhere = m4_i420(out.frame);
    pb = m4_fill(s, s.planes[pi], 0, 0, 0xFFFF, 5, kM4StartCode);
    CHECK(asp->decode(pb.view(), &out) == DxvaResult::Ok && out.surface == 0 && m4_i420(out.frame) == elsewhere);
    // A plane that is not coded repeats its reference.
    CHECK(asp->decode(ib.view(), &out) == DxvaResult::Ok);
    const std::vector<uint8_t> intra = m4_i420(out.frame);
    c = m4_fill(s, s.planes[pi], 2, 0, 0xFFFF, 6, kM4AfterStartCode);
    c.pp.wPicFlagBitFields &= static_cast<uint16_t>(~dxva::kMpeg4PicVopCoded);
    CHECK(asp->decode(c.view(), &out) == DxvaResult::Ok && m4_i420(out.frame) == intra);
    // After reset() the reference is gone.
    asp->reset();
    pb = m4_fill(s, s.planes[pi], 1, 0, 0xFFFF, 7, kM4StartCode);
    CHECK(asp->decode(pb.view(), &out) == DxvaResult::MissingReference);
  }
  CHECK(a.live == 0);
}

}  // namespace

static void dxva_test_mpeg4() {
  printf("MPEG-4 Part 2 (DXVA)\n");
  static const char* const streams[] = {"bframes", "interlaced", "oddsize", "packets", "partitioned", "qpel", "rvlc"};
  for (const char* name : streams) {
    const std::string base = std::string("m4vdec_") + name;
    m4_test_stream((base + ".m4v").c_str(), (base + ".yuv").c_str());
  }
  // Xvid's streams without their user data (tests/data/make_dxva_mpeg4_fixtures.sh): the standard IDCT, as DXVA decodes them. GMC with 1 .. 3 warping points.
  m4_test_stream("dxva_mpeg4_gmc.m4v", "dxva_mpeg4_gmc.yuv");
  m4_test_stream("dxva_mpeg4_xvid.m4v", "dxva_mpeg4_xvid.yuv");
  m4_test_stream("h263dec_ip.h263", "h263dec_ip.yuv");
  m4_test_damaged("m4vdec_bframes.m4v");
  m4_test_damaged("dxva_mpeg4_gmc.m4v");
  m4_test_damaged("m4vdec_partitioned.m4v");
  m4_test_refusals();
}
