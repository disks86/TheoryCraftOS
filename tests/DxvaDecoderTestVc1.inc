// DxvaDecoderTest: the VC-1 cases (D3D11_DECODER_PROFILE_VC1_VLD and _VC1_D2010), Simple, Main and Advanced profile.
//
// The program's part: the test runs this project's own VC-1 decoder over tests/data/vc1_*.rcv / .vc1 with a frame observer, which tells it, for every picture in
// decoding order, the sequence and picture headers, the picture's units (the frame and its slices as they are in the stream), where the macroblocks of each begin
// and which pool slots the picture and its references are in - what a program's own parser knows. From that it fills DXVA_PictureParameters and DXVA_SliceInfo as
// ffmpeg's libavcodec/dxva2_vc1.c does (the pool slots are the surfaces; Advanced profile pieces keep their start codes, the buffer is padded to 128 bytes) and
// decodes each picture through the DXVA decoder. A field interlace frame is two DXVA pictures, its first field (the frame unit and the first field's slices) and its
// second field (the field unit, 0C, and its slices), each of which comes back as a field picture: the test keeps each surface's samples and puts only the field's
// lines in, as the kernel does. Every frame must be the very frame the software decoder made (bit exact; its output: range reduction scaled up, range mapping
// applied), and the frames put in display order and cropped must be ffmpeg's decode (tests/data/vc1_*.yuv) exactly.
#include "Hardware/VideoCodec/Dxva/DxvaTypesVc1.hpp"
#include "Hardware/VideoCodec/Software/Vc1.hpp"
#include "Hardware/VideoCodec/Software/Vc1Core.hpp"

namespace {

static uint32_t vc_le32(const std::vector<uint8_t>& d, size_t at) { return d[at] | d[at + 1] << 8 | d[at + 2] << 16 | uint32_t(d[at + 3]) << 24; }

// A test stream split into what the decoder takes: the configuration (Simple and Main profile) and one unit per frame.
struct VcStream {
  bool advanced = false;
  uint8_t struct_c[4] = {};
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> bytes;
  std::vector<std::pair<size_t, size_t>> units;   // offset, size
};

// The RCV container: a header (frame count, sequence header, size) then, per frame, a 24 bit size, a flag byte, a 32 bit timestamp and the data.
static bool vc_parse_rcv(VcStream* s) {
  const std::vector<uint8_t>& d = s->bytes;
  if (d.size() < 36 || (vc_le32(d, 0) >> 24) != 0xC5 || vc_le32(d, 4) != 4) return false;
  memcpy(s->struct_c, &d[8], 4);
  s->height = vc_le32(d, 12);
  s->width = vc_le32(d, 16);
  size_t pos = 36;
  while (pos + 8 <= d.size()) {
    const size_t n = d[pos] | d[pos + 1] << 8 | d[pos + 2] << 16;
    if (n > d.size() - pos - 8) return false;
    s->units.push_back({pos + 8, n});
    pos += 8 + n;
  }
  return pos == d.size();
}

// The Advanced profile elementary stream: a frame (start code 0D, then slices 0B) with the sequence header (0F) and entry point (0E) units before it, if any.
static void vc_split_advanced(VcStream* s) {
  const std::vector<uint8_t>& d = s->bytes;
  size_t begin = 0;
  bool in_frame = false, started = false;
  for (size_t i = 0; i + 4 <= d.size(); ++i) {
    if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
    const int code = d[i + 3];
    if (code != 0x0F && code != 0x0E && code != 0x0D && code != 0x0B) continue;
    if (code != 0x0B && (in_frame || !started)) {
      if (started) s->units.push_back({begin, i - begin});
      begin = i;
    }
    started = true;
    in_frame = code == 0x0D || code == 0x0B;
  }
  if (started) s->units.push_back({begin, d.size() - begin});
}

static bool vc_load(const char* name, VcStream* s) {
  s->bytes = read_file(std::string("tests/data/vc1_") + name + ".rcv");
  if (!s->bytes.empty()) return vc_parse_rcv(s);
  s->bytes = read_file(std::string("tests/data/vc1_") + name + ".vc1");
  s->advanced = true;
  vc_split_advanced(s);
  return !s->units.empty();
}

// One picture (a frame: a field interlace frame is both its fields), as the program's parser knows it.
struct VcPic {
  vc1::SequenceHeader seq;
  vc1::PictureHeader pic;                    // (a field pair: the first field's)
  bool fields = false;
  vc1::PictureHeader second;                 // a field pair: the second field's
  int slot = 0, forward = -1, backward = -1;
  int newer_anchor = -1;                     // the newer anchor when the frame starts (the forward reference of an I / P frame's second field)
  std::vector<std::vector<uint8_t>> units;   // as in the stream (Advanced profile: from the start code)
  std::vector<vc1::UnitInfo> info;
  std::vector<uint8_t> frame;                // the software decoder's output, I420 at the coded size (whole macroblocks)
};

struct VcObserver final : vc1::IFrameObserver {
  std::vector<VcPic> pics;
  void picture_decoded(const vc1::OffloadPicture& p) override {
    VcPic v;
    v.seq = *p.seq;
    v.pic = *p.pic;
    v.fields = p.second_field != nullptr;
    if (p.second_field) v.second = *p.second_field;
    v.slot = p.slot;
    v.forward = p.forward_slot;
    v.backward = p.backward_slot;
    v.newer_anchor = p.anchor_slot[1];
    for (const ConstByteView& u : p.units) v.units.push_back(std::vector<uint8_t>(u.data(), u.data() + u.size()));
    for (const vc1::UnitInfo& i : p.unit_info) v.info.push_back(i);
    // The output: the reconstruction through range reduction and range mapping, at the coded size (a field pair's planes may have a macroblock row more).
    vc1::Picture out;
    out.range_reduced = v.seq.profile != vc1::kAdvanced && v.pic.rangeredfrm;
    out.range_map_y = v.seq.profile == vc1::kAdvanced && v.seq.range_mapy_flag ? v.seq.range_mapy : -1;
    out.range_map_uv = v.seq.profile == vc1::kAdvanced && v.seq.range_mapuv_flag ? v.seq.range_mapuv : -1;
    const vc1::OutputMap map = vc1::output_map(out);
    for (int c = 0; c < 3; ++c) {
      const size_t rows = size_t(v.seq.mb_height) * (c ? 8 : 16), width = size_t(v.seq.mb_width) * (c ? 8 : 16);
      for (size_t y = 0; y < rows; ++y)
        for (size_t x = 0; x < width; ++x) v.frame.push_back((c ? map.uv : map.y)[p.plane[c][y * size_t(p.stride[c]) + x]]);
    }
    pics.push_back(std::move(v));
  }
};

template <class T>
static ConstByteView vc_bytes(const T& value) {
  // (the test fills the DXVA structures the way a program does and hands their bytes over; the structures are packed byte arrays)
  return ConstByteView(reinterpret_cast<const uint8_t*>(&value), sizeof value);
}

struct VcBuffers {
  dxva::PictureParametersVc1 pp = {};
  std::vector<dxva::SliceInfoVc1> slices;
  std::vector<uint8_t> bitstream;
  DxvaPicture view() const {
    DxvaPicture p;
    p.picture_parameters = vc_bytes(pp);
    p.slice_control = ConstByteView(reinterpret_cast<const uint8_t*>(slices.data()), slices.size() * sizeof(dxva::SliceInfoVc1));
    p.bitstream = ConstByteView(bitstream.data(), bitstream.size());
    return p;
  }
};

// The buffers of one picture, as ffmpeg's ff_dxva2_vc1_fill_picture_parameters / ff_dxva2_vc1_fill_slice / commit_bitstream_and_slice_buffer fill them.
// `strip_start_code`: an Advanced profile frame without its start code (one piece), which a decoder takes too. `part`: of a field pair, 1 the first field (the
// units before the field unit) and 2 the second (the field unit and the units after it); 0 a frame.
static VcBuffers vc_fill(const VcPic& v, uint16_t cur, uint16_t fwd, uint16_t bwd, uint32_t report_id, bool strip_start_code = false, int part = 0) {
  VcBuffers b;
  dxva::PictureParametersVc1& pp = b.pp;
  const vc1::SequenceHeader& s = v.seq;
  const vc1::PictureHeader& h = part == 2 ? v.second : v.pic;
  const bool adv = s.profile == vc1::kAdvanced;
  const bool intra = h.type == vc1::kPictureI || h.type == vc1::kPictureBI, bpic = h.type == vc1::kPictureB;
  const bool field = part != 0;
  const bool intcomp = h.type == vc1::kPictureP && h.use_ic && (h.lumscale != 32 || h.lumshift != 0 || (field && (h.lumscale2 != 32 || h.lumshift2 != 0)));
  // The units of the part: a field pair's field unit (start code 0C) begins the second field.
  size_t first_unit = 0, end_unit = v.units.size();
  for (size_t k = 0; k < v.units.size() && field; ++k)
    if (v.units[k].size() >= 4 && v.units[k][0] == 0 && v.units[k][1] == 0 && v.units[k][2] == 1 && v.units[k][3] == 0x0C) {
      if (part == 1) end_unit = k;
      else first_unit = k;
    }
  const int field_rows = (s.mb_height + 1) / 2;
  pp.wDecodedPictureIndex = pp.wDeblockedPictureIndex = cur;
  pp.wForwardRefPictureIndex = intra ? 0xFFFF : fwd;
  pp.wBackwardRefPictureIndex = bpic ? bwd : 0xFFFF;
  if (adv) {
    pp.wPicWidthInMBminus1 = static_cast<uint16_t>(s.width - 1);
    pp.wPicHeightInMBminus1 = static_cast<uint16_t>(s.height - 1);
  } else {
    pp.wPicWidthInMBminus1 = static_cast<uint16_t>(s.mb_width - 1);
    pp.wPicHeightInMBminus1 = static_cast<uint16_t>(s.mb_height - 1);
  }
  pp.bMacroblockWidthMinus1 = 15;
  pp.bMacroblockHeightMinus1 = 15;
  pp.bBlockWidthMinus1 = 7;
  pp.bBlockHeightMinus1 = 7;
  pp.bBPPminus1 = 7;
  pp.bPicStructure = !field ? 3 : (h.tff == (part == 1)) ? 1 : 2;   // (the first field is the top one when TFF is set)
  pp.bSecondField = part == 2;
  pp.bPicIntra = intra;
  pp.bPicBackwardPrediction = bpic;
  pp.bBidirectionalAveragingMode = static_cast<uint8_t>(0x80 | (intcomp ? 0x10 : 0) | (adv ? 0x08 : 0));
  pp.bMVprecisionAndChromaRelation = static_cast<uint8_t>((h.mv_mode == vc1::kMv1MvHpelBilin ? 8 : 0) | 4 | (h.quarter_sample ? 0 : 1));
  pp.bChromaFormat = 1;
  pp.bPicScanFixed = static_cast<uint8_t>(report_id >> 8);
  pp.bPicScanMethod = static_cast<uint8_t>(report_id & 0xFF);
  pp.bRcontrol = static_cast<uint8_t>(h.rnd);
  pp.bPicSpatialResid8 = static_cast<uint8_t>(s.panscanflag << 7 | s.refdist_flag << 6 | s.loop_filter << 5 | s.fastuvmc << 4 | s.extended_mv << 3 | s.dquant << 1 | s.vstransform);
  pp.bPicOverflowBlocks = static_cast<uint8_t>(s.quantizer_mode << 6 | s.multires << 5 | s.resync_marker << 4 | s.rangered << 3 | (adv ? 7 : s.max_b_frames));
  pp.bPicExtrapolation = h.fcm == vc1::kFcmProgressive ? 1 : 2;
  pp.bPicDeblocked = static_cast<uint8_t>((!bpic && s.overlap) << 6 | (!adv && h.rangeredfrm) << 5 | s.loop_filter << 1);
  pp.bPicDeblockConfined = static_cast<uint8_t>(s.postprocflag << 7 | s.broadcast << 6 | s.interlace << 5 | s.tfcntrflag << 4 | s.finterpflag << 3 | (!bpic) << 2 | s.psf << 1 | s.extended_dmv);
  if (h.type != vc1::kPictureI) pp.bPic4MVallowed = h.mv_mode == vc1::kMvMixed || (h.mv_mode == vc1::kMvIntensityComp && h.mv_mode2 == vc1::kMvMixed);
  if (adv) pp.bPicOBMC = static_cast<uint8_t>(s.range_mapy_flag << 7 | s.range_mapy << 4 | s.range_mapuv_flag << 3 | s.range_mapuv);
  pp.bMV_RPS = static_cast<uint8_t>(field && bpic ? h.refdist + 9 : 0);
  pp.bReservedBits = static_cast<uint8_t>(h.pq);
  if (!field) {
    pp.wBitstreamFcodes = static_cast<uint16_t>(intcomp ? h.lumscale : 32);
    pp.wBitstreamPCEelements = static_cast<uint16_t>(intcomp ? h.lumshift : 0);
  } else {
    pp.wBitstreamFcodes = static_cast<uint16_t>(intcomp ? h.lumscale << 8 | h.lumscale2 : 32 << 8 | 32);
    pp.wBitstreamPCEelements = static_cast<uint16_t>(intcomp ? h.lumshift << 8 | h.lumshift2 : 0);
  }
  const uint32_t mb_count = static_cast<uint32_t>(s.mb_width * (field ? s.mb_height >> 1 : s.mb_height));
  for (size_t k = first_unit; k < end_unit; ++k) {
    std::vector<uint8_t> unit = v.units[k];
    const bool coded = unit.size() >= 4 && unit[0] == 0 && unit[1] == 0 && unit[2] == 1;
    if (adv && !coded) unit.insert(unit.begin(), {0, 0, 1, uint8_t(k == 0 ? 0x0D : 0x0B)});   // (ffmpeg puts the start code there)
    if (adv && strip_start_code && k == first_unit) unit.erase(unit.begin(), unit.begin() + 4);
    dxva::SliceInfoVc1 si = {};
    // (ffmpeg: the slice's address as it is, a second field's first piece the first field's rows; the macroblock numbers of field pictures halve it again)
    const int row = k == first_unit ? (part == 2 ? field_rows : 0) : (k < v.info.size() ? v.info[k].start_row : 0);
    si.wVerticalPosition = static_cast<uint16_t>(row);
    si.dwSliceBitsInBuffer = static_cast<uint32_t>(8 * unit.size());
    si.dwSliceDataLocation = static_cast<uint32_t>(b.bitstream.size());
    si.bReservedBits = static_cast<uint8_t>(bpic ? h.bfraction_index + 9 : 0);
    si.wMBbitOffset = static_cast<uint16_t>(h.skipped ? 0xFFFF : (k < v.info.size() ? v.info[k].mb_bit_offset : 0) + (adv && !(strip_start_code && k == first_unit) ? 32 : 0));
    si.wNumberMBsInSlice = static_cast<uint16_t>((row >> (field ? 1 : 0)) * s.mb_width);   // (made a count below)
    si.wQuantizerScaleCode = static_cast<uint16_t>(h.pq);
    b.bitstream.insert(b.bitstream.end(), unit.begin(), unit.end());
    b.slices.push_back(si);
  }
  for (size_t i = 0; i < b.slices.size(); ++i)
    b.slices[i].wNumberMBsInSlice = static_cast<uint16_t>((i + 1 < b.slices.size() ? b.slices[i + 1].wNumberMBsInSlice : mb_count) - b.slices[i].wNumberMBsInSlice);
  (void)field_rows;
  const size_t padding = 128 - b.bitstream.size() % 128;
  b.bitstream.insert(b.bitstream.end(), padding, 0);
  if (!b.slices.empty()) b.slices.back().dwSliceBitsInBuffer += static_cast<uint32_t>(8 * padding);
  return b;
}

// The I420 frame (coded size) of an NV12 picture.
static std::vector<uint8_t> vc_i420(const VideoFrame& f) {
  std::vector<uint8_t> out;
  const uint32_t w = f.format.width, h = f.format.height;
  for (uint32_t y = 0; y < h; ++y) out.insert(out.end(), f.plane[0] + size_t(y) * f.stride[0], f.plane[0] + size_t(y) * f.stride[0] + w);
  for (int c = 0; c < 2; ++c)
    for (uint32_t y = 0; y < h / 2; ++y)
      for (uint32_t x = 0; x < w / 2; ++x) out.push_back(f.plane[1][size_t(y) * f.stride[1] + 2 * x + c]);
  return out;
}

static std::vector<VcPic> vc_observe(TestAllocator& a, const VcStream& s) {
  VcObserver observer;
  {
    Vc1Decoder soft(a);
    soft.set_frame_observer(&observer);
    if (!s.advanced) CHECK(soft.configure(ConstByteView(s.struct_c, 4), s.width, s.height));
    for (const auto& u : s.units) CHECK(soft.decode(ConstByteView(s.bytes.data() + u.first, u.second), 0));
    soft.flush();
  }
  return std::move(observer.pics);
}

constexpr uint32_t kVcSurfaces = 10;   // (vc1::kPoolSize: the software decoder's slots are the surfaces)

struct VcRun {
  std::vector<std::vector<uint8_t>> decoded;
  std::vector<DxvaResult> results;
};

// Decodes the pictures through the DXVA decoder. `keys`: a surface_keys table whose indices move from picture to picture; `output_key`: the current picture named
// by DxvaPicture::output_key with a CurrPic index (200) outside the table (Direct3D 12's way). `strip`: Advanced profile frames without their start code.
static VcRun vc_run(TestAllocator& a, DxvaProfile profile, const std::vector<VcPic>& pics, bool keys, bool output_key, bool strip = false) {
  VcRun run;
  DxvaDecoderConfig config;
  config.profile = profile;
  config.width = pics.empty() ? 16 : uint32_t(pics[0].seq.mb_width * 16);
  config.height = pics.empty() ? 16 : uint32_t(pics[0].seq.mb_height * 16);
  config.surfaces = kVcSurfaces;
  config.output = PixelFormat::Nv12;
  UniquePointer<IDxvaDecoder> dec = create_dxva_decoder(a, config);
  CHECK(dec);
  if (!dec) return run;
  uint32_t perm[kVcSurfaces], key_table[kVcSurfaces];
  for (uint32_t i = 0; i < kVcSurfaces; ++i) perm[i] = i;
  std::vector<uint8_t> surface_samples[kVcSurfaces];   // what each surface holds (by the software decoder's slot)
  for (size_t n = 0; n < pics.size(); ++n) {
    const VcPic& v = pics[n];
    if (keys) {
      for (uint32_t i = 0; i < kVcSurfaces; ++i) {
        const uint32_t j = static_cast<uint32_t>((n * 7 + i * 3) % kVcSurfaces);
        const uint32_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
      }
      for (uint32_t s = 0; s < kVcSurfaces; ++s) key_table[perm[s]] = 1000 + s;
    }
    auto index = [&](int s) { return static_cast<uint16_t>(s < 0 ? 0xFFFF : keys ? perm[s] : static_cast<uint32_t>(s)); };
    std::vector<uint8_t>& surface = surface_samples[v.slot];
    bool ok = true;
    for (int part = v.fields ? 1 : 0; part <= (v.fields ? 2 : 0); ++part) {
      const vc1::PictureHeader& h = part == 2 ? v.second : v.pic;
      // (an I / P frame's second field predicts from the newer anchor before the frame, whatever its first field is)
      const int forward = part == 2 && h.type == vc1::kPictureP ? v.newer_anchor : v.forward;
      VcBuffers b = vc_fill(v, index(v.slot), index(forward), index(v.backward), uint32_t(2 * n + 1 + (part == 2)), strip, part);
      if (keys && output_key) b.pp.wDecodedPictureIndex = 200;
      DxvaPicture view = b.view();
      if (keys) view.surface_keys = ArrayView<const uint32_t>(key_table, kVcSurfaces);
      if (keys && output_key) view.output_key = 1000 + static_cast<uint32_t>(v.slot);
      DxvaDecodedPicture out;
      const DxvaResult r = dec->decode(view, &out);
      run.results.push_back(r);
      if (r == DxvaResult::Ok || r == DxvaResult::MissingReference) {
        CHECK(out.surface == (keys && output_key ? 200u : index(v.slot)));
        CHECK(out.frame.format.pixel_format == PixelFormat::Nv12 && int(out.frame.format.width) == v.seq.mb_width * 16 && int(out.frame.format.height) == v.seq.mb_height * 16);
        CHECK(out.surface_format == DxvaSurfaceFormat::Nv12 && out.field == (part == 0 ? 0 : b.pp.bPicStructure));
        // Into the surface: the whole frame, or the lines of the field (luma and chroma lines of its parity), as the kernel writes them.
        const std::vector<uint8_t> frame = vc_i420(out.frame);
        if (surface.size() != frame.size()) surface.assign(frame.size(), 128);
        const size_t w = out.frame.format.width, hh = out.frame.format.height;
        const size_t plane_offset[3] = {0, w * hh, w * hh + (w / 2) * (hh / 2)};
        for (int c = 0; c < 3; ++c) {
          const size_t pw = c ? w / 2 : w, ph = c ? hh / 2 : hh;
          for (size_t y = 0; y < ph; ++y)
            if (out.field == 0 || (y & 1) == size_t(out.field - 1))
              for (size_t x = 0; x < pw; ++x) surface[plane_offset[c] + y * pw + x] = frame[plane_offset[c] + y * pw + x];
        }
      } else {
        printf("    picture %zu part %d (type %d): result %d (%s)\n", n, part, h.type, int(r), out.detail ? out.detail : "");
        ok = false;
      }
    }
    run.decoded.push_back(ok ? surface : std::vector<uint8_t>());
  }
  return run;
}

static void vc_test_stream(const char* name, DxvaProfile profile) {
  VcStream s;
  CHECK(vc_load(name, &s));
  const std::vector<uint8_t> ref = read_file(std::string("tests/data/vc1_") + name + ".yuv");
  CHECK(!s.units.empty() && !ref.empty());
  if (s.units.empty() || ref.empty()) return;
  TestAllocator a;
  {
    const std::vector<VcPic> pics = vc_observe(a, s);
    CHECK(pics.size() == s.units.size());
    const VcRun run = vc_run(a, profile, pics, false, false);
    int exact = 0;
    for (const DxvaResult r : run.results) CHECK(r == DxvaResult::Ok);
    for (size_t i = 0; i < run.decoded.size() && i < pics.size(); ++i) {
      if (run.decoded[i] == pics[i].frame) ++exact;
      else if (getenv("VC_DEBUG")) printf("    picture %zu (type %d) differs\n", i, pics[i].pic.type);
    }
    CHECK(exact == int(pics.size()));
    // Display order, cropped, against ffmpeg's frames: B and BI pictures come out at once, an anchor when the next one comes (at once without B pictures).
    const bool adv = s.advanced;
    const int w = adv ? (pics.empty() ? 0 : pics[0].seq.width) : int(s.width), h = adv ? (pics.empty() ? 0 : pics[0].seq.height) : int(s.height);
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
      const int cw = pics[i].seq.mb_width * 16, ch = pics[i].seq.mb_height * 16;
      const size_t plane_offset[3] = {0, size_t(cw) * ch, size_t(cw) * ch + size_t(cw / 2) * (ch / 2)};
      size_t p = offset;
      for (int c = 0; c < 3; ++c) {
        const int pw = c ? (w + 1) / 2 : w, ph = c ? (h + 1) / 2 : h, stride = c ? cw / 2 : cw;
        for (int y = 0; y < ph; ++y)
          for (int x = 0; x < pw; ++x) {
            const int d = abs(int(coded[plane_offset[c] + size_t(y) * stride + x]) - int(ref[p++]));
            if (d > max_diff) max_diff = d;
          }
      }
    };
    const bool low_delay = !adv && !pics.empty() && pics[0].seq.max_b_frames == 0;
    int held = -1;
    for (size_t i = 0; i < pics.size() && i < run.decoded.size(); ++i) {
      const int type = pics[i].pic.type;
      if (type == vc1::kPictureB || type == vc1::kPictureBI || low_delay) {
        compare(i);
      } else {
        if (held >= 0) compare(size_t(held));
        held = static_cast<int>(i);
      }
    }
    if (held >= 0) compare(size_t(held));
    printf("  %-14s %s %zu pictures, %d bit exact with Vc1Decoder; %d frames, largest difference to ffmpeg %d\n", name, profile == DxvaProfile::Vc1D2010 ? "D2010" : "VLD  ",
           pics.size(), exact, frames, max_diff);
    CHECK(size_t(frames) * frame_bytes == ref.size());
    CHECK(max_diff == 0);
    // The same with surface keys that move from picture to picture, with the output named by its key, and (Advanced profile) frames without their start code.
    CHECK(vc_run(a, profile, pics, true, false).decoded == run.decoded);
    CHECK(vc_run(a, profile, pics, true, true).decoded == run.decoded);
    if (adv) CHECK(vc_run(a, profile, pics, false, false, true).decoded == run.decoded);
  }
  CHECK(a.live == 0);
}

// Damaged and malformed buffers: Invalid (or, for damaged picture data, a picture) and never a fault, a sanitizer report or a leak.
static void vc_test_damaged(const char* name) {
  VcStream s;
  CHECK(vc_load(name, &s));
  if (s.units.empty()) return;
  TestAllocator a;
  int invalid = 0, decoded = 0;
  {
    const std::vector<VcPic> pics = vc_observe(a, s);
    DxvaDecoderConfig config;
    config.profile = DxvaProfile::Vc1Vld;
    config.width = uint32_t(pics[0].seq.mb_width * 16);
    config.height = uint32_t(pics[0].seq.mb_height * 16);
    config.surfaces = kVcSurfaces;
    config.output = PixelFormat::Nv12;
    UniquePointer<IDxvaDecoder> dec = create_dxva_decoder(a, config);
    CHECK(dec);
    if (!dec) return;
    for (size_t n = 0; n < pics.size(); ++n)
     for (int part = pics[n].fields ? 1 : 0; part <= (pics[n].fields ? 2 : 0); ++part) {
      const VcPic& v = pics[n];
      auto idx = [](int s) { return static_cast<uint16_t>(s < 0 ? 0xFFFF : s); };
      const int forward = part == 2 && v.second.type == vc1::kPictureP ? v.newer_anchor : v.forward;
      const VcBuffers good = vc_fill(v, idx(v.slot), idx(forward), idx(v.backward), uint32_t(n + 1), false, part);
      const VcBuffers first = part == 2 ? vc_fill(v, idx(v.slot), idx(v.forward), idx(v.backward), uint32_t(n + 1), false, 1) : VcBuffers();
      DxvaDecodedPicture out;
      // (a second field follows its first field: each damaged second field comes right after the good first one)
      const auto lead_in = [&]() {
        if (part == 2) CHECK(dec->decode(first.view(), &out) != DxvaResult::Invalid);
      };
      for (int variant = 0; variant < 30; ++variant) {
        lead_in();
        VcBuffers b = good;
        std::vector<uint8_t> pp(reinterpret_cast<const uint8_t*>(&b.pp), reinterpret_cast<const uint8_t*>(&b.pp) + sizeof b.pp);
        std::vector<uint8_t> sc(reinterpret_cast<const uint8_t*>(b.slices.data()), reinterpret_cast<const uint8_t*>(b.slices.data()) + b.slices.size() * sizeof(dxva::SliceInfoVc1));
        std::vector<uint8_t>& bs = b.bitstream;
        std::vector<uint8_t>* target = variant % 3 == 0 ? &pp : variant % 3 == 1 ? &sc : &bs;
        const size_t salt = n * 7919 + size_t(variant) * 104729;
        if (variant < 12) target->resize(target->size() * size_t(variant / 3 + 1) / 5);
        else if (!target->empty()) for (int k = 0; k < 1 + variant % 4; ++k) (*target)[(salt + size_t(k) * 31) % target->size()] ^= uint8_t(1u << ((salt + size_t(k)) % 8));
        DxvaPicture p;
        p.picture_parameters = ConstByteView(pp.data(), pp.size());
        p.slice_control = ConstByteView(sc.data(), sc.size());
        p.bitstream = ConstByteView(bs.data(), bs.size());
        const DxvaResult r = dec->decode(p, &out);
        if (variant < 12 && target == &pp) CHECK(r == DxvaResult::Invalid);
        if (variant < 12 && target == &sc && sc.size() % sizeof(dxva::SliceInfoVc1) != 0) CHECK(r == DxvaResult::Invalid);
        if (r == DxvaResult::Invalid || r == DxvaResult::Unsupported) ++invalid;
        else ++decoded;
        if (r == DxvaResult::Ok || r == DxvaResult::MissingReference) CHECK(out.frame.plane[0] != nullptr);
      }
      // Indices out of range, a slice outside the buffer, missing buffers.
      const vc1::PictureHeader& h = part == 2 ? v.second : v.pic;
      VcBuffers b = good;
      b.pp.wDecodedPictureIndex = kVcSurfaces;
      lead_in();
      CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
      if (forward >= 0 && (h.type == vc1::kPictureP || h.type == vc1::kPictureB)) {
        b = good;
        b.pp.wForwardRefPictureIndex = 0xFFFF;
        lead_in();
        CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
      }
      b = good;
      b.slices[0].dwSliceDataLocation = 0xFFFFFFF0u;
      lead_in();
      CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
      b = good;
      b.slices[0].dwSliceBitsInBuffer = 0xFFFFFFFFu;
      lead_in();
      CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
      DxvaPicture p = good.view();
      p.slice_control = ConstByteView();
      lead_in();
      CHECK(dec->decode(p, &out) == DxvaResult::Invalid);
      p = good.view();
      p.bitstream = ConstByteView();
      lead_in();
      CHECK(dec->decode(p, &out) == DxvaResult::Invalid);
      // Then the picture itself, so the next ones have their references.
      lead_in();
      const DxvaResult r = dec->decode(good.view(), &out);
      CHECK(r == DxvaResult::Ok || r == DxvaResult::MissingReference);
    }
  }
  printf("  %-14s damaged buffers: %d refused, %d decoded\n", name, invalid, decoded);
  CHECK(invalid > 0 && decoded > 0 && a.live == 0);
}

// What the decoder refuses or reports for pictures that are well formed but cannot be decoded as asked.
static void vc_test_refusals() {
  VcStream s;
  CHECK(vc_load("adv_ipb", &s));
  if (s.units.empty()) return;
  TestAllocator a;
  {
    const std::vector<VcPic> pics = vc_observe(a, s);
    DxvaDecoderConfig config;
    config.profile = DxvaProfile::Vc1Vld;
    config.width = uint32_t(pics[0].seq.mb_width * 16);
    config.height = uint32_t(pics[0].seq.mb_height * 16);
    config.surfaces = 4;
    config.output = PixelFormat::Nv12;
    UniquePointer<IDxvaDecoder> dec = create_dxva_decoder(a, config);
    CHECK(dec && dec->profile() == DxvaProfile::Vc1Vld);
    if (!dec) return;
    CHECK(!create_dxva_decoder(a, DxvaDecoderConfig{DxvaProfile::Vc1Vld, 64, 64, 4, PixelFormat::P010}));
    CHECK(dxva_profile_supports(DxvaProfile::Vc1D2010, PixelFormat::Nv12, 1920, 1088) && !dxva_profile_supports(DxvaProfile::Vc1D2010, PixelFormat::P010, 64, 64));
    DxvaDecodedPicture out;
    size_t p_index = 0;
    while (p_index < pics.size() && pics[p_index].pic.type != vc1::kPictureP) ++p_index;
    CHECK(p_index < pics.size());
    if (p_index >= pics.size()) return;
    // A P picture whose reference surface holds nothing: decoded all the same, with MissingReference.
    VcBuffers pb = vc_fill(pics[p_index], 1, 2, 0xFFFF, 1);
    CHECK(dec->decode(pb.view(), &out) == DxvaResult::MissingReference && out.frame.plane[0] && out.surface == 1);
    const VcBuffers ib = vc_fill(pics[0], 0, 0xFFFF, 0xFFFF, 2);
    CHECK(dec->decode(ib.view(), &out) == DxvaResult::Ok);
    // A picture decoded into the surface it predicts from: the same picture as decoded elsewhere.
    pb = vc_fill(pics[p_index], 1, 0, 0xFFFF, 3);
    CHECK(dec->decode(pb.view(), &out) == DxvaResult::Ok);
    const std::vector<uint8_t> elsewhere = vc_i420(out.frame);
    VcBuffers same = vc_fill(pics[p_index], 0, 0, 0xFFFF, 4);
    CHECK(dec->decode(same.view(), &out) == DxvaResult::Ok && out.surface == 0 && vc_i420(out.frame) == elsewhere);
    CHECK(dec->decode(ib.view(), &out) == DxvaResult::Ok);
    // The type the parameters say is not the bitstream's: an I picture sent as a P picture.
    VcBuffers wrong = ib;
    wrong.pp.bPicIntra = 0;
    wrong.pp.wForwardRefPictureIndex = 1;
    CHECK(dec->decode(wrong.view(), &out) == DxvaResult::Invalid && out.detail);
    // Post-processing, pan-scan, chopped slices: refused as Unsupported.
    const auto result = [&](void (*change)(VcBuffers&)) {
      VcBuffers c = ib;
      change(c);
      const DxvaResult r = dec->decode(c.view(), &out);
      CHECK(r == DxvaResult::Ok || out.detail != nullptr);
      return r;
    };
    CHECK(result([](VcBuffers& c) { c.pp.bPicDeblocked |= dxva::kVc1DeblockedDeblocking; }) == DxvaResult::Unsupported);
    CHECK(result([](VcBuffers& c) { c.pp.bPicSpatialResid8 |= dxva::kVc1SpatialPanScan; }) == DxvaResult::Unsupported);
    CHECK(result([](VcBuffers& c) { c.slices[0].wBadSliceChopping = 1; }) == DxvaResult::Unsupported);
    // A progressive picture of a progressive sequence said to be interlaced, or a field; a frame behind a field start code: Invalid.
    CHECK(result([](VcBuffers& c) { c.pp.bPicExtrapolation = 2; }) == DxvaResult::Invalid);
    CHECK(result([](VcBuffers& c) { c.pp.bPicStructure = 1; }) == DxvaResult::Invalid);
    CHECK(result([](VcBuffers& c) { c.pp.bPicExtrapolation = 3; }) == DxvaResult::Invalid);
    CHECK(result([](VcBuffers& c) { c.pp.bSecondField = 1; }) == DxvaResult::Invalid);
    CHECK(result([](VcBuffers& c) { c.bitstream[3] = 0x0C; }) == DxvaResult::Invalid);
    // Intensity compensation said to be on for an I picture.
    CHECK(result([](VcBuffers& c) { c.pp.bBidirectionalAveragingMode |= dxva::kVc1AveragingIntensityComp; }) == DxvaResult::Invalid);
    // Range mapping (only the parameters have it): the output is mapped, RANGE_MAPY 0 is 9/8 around 128; a value without its flag is not used.
    CHECK(dec->decode(ib.view(), &out) == DxvaResult::Ok);
    const std::vector<uint8_t> unmapped = vc_i420(out.frame);
    CHECK(result([](VcBuffers& c) { c.pp.bPicOBMC = 0x80; }) == DxvaResult::Ok);
    const std::vector<uint8_t> mapped = vc_i420(out.frame);
    bool mapped_right = mapped.size() == unmapped.size();
    const size_t luma = size_t(config.width) * config.height;
    for (size_t i = 0; i < mapped.size() && mapped_right; ++i) {
      const int v = unmapped[i];
      const int m = i < luma ? ((((v - 128) * 9 + 4) >> 3) + 128) : v;
      mapped_right = mapped[i] == (m < 0 ? 0 : m > 255 ? 255 : m);
    }
    CHECK(mapped_right && mapped != unmapped);
    CHECK(result([](VcBuffers& c) { c.pp.bPicOBMC = 0x77; }) == DxvaResult::Ok && vc_i420(out.frame) == unmapped);
    // Malformed: other sample sizes, 4:2:2, larger than the surfaces, a second piece without a slice start code.
    VcBuffers c = ib;
    c.pp.bChromaFormat = 2;
    CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid);
    c = ib;
    c.pp.wPicWidthInMBminus1 = static_cast<uint16_t>(config.width + 32);
    CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid);
    c = ib;
    c.bitstream[3] = 0x0F;
    CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid);
    // A Simple / Main profile picture whose RANGEREDFRM (bit 5 of bPicDeblocked) is not the bitstream's.
    VcStream sm;
    CHECK(vc_load("sm_ipp", &sm));
    const std::vector<VcPic> smp = vc_observe(a, sm);
    if (!smp.empty()) {
      VcBuffers r = vc_fill(smp[0], 0, 0xFFFF, 0xFFFF, 5);
      r.pp.bPicDeblocked |= dxva::kVc1DeblockedRangeRedFrame;
      CHECK(dec->decode(r.view(), &out) == DxvaResult::Invalid);
    }
    // After reset() the reference is gone.
    dec->reset();
    pb = vc_fill(pics[p_index], 1, 0, 0xFFFF, 6);
    CHECK(dec->decode(pb.view(), &out) == DxvaResult::MissingReference);
  }
  CHECK(a.live == 0);
}

// Interlaced pictures that are well formed but not what the bitstream says, and field pairs that are not pairs.
static void vc_test_interlaced_refusals() {
  TestAllocator a;
  {
    VcStream fs, rs;
    CHECK(vc_load("adv_fi_b", &fs) && vc_load("adv_fr_p", &rs));
    const std::vector<VcPic> fp = vc_observe(a, fs), rp = vc_observe(a, rs);
    CHECK(!fp.empty() && fp[0].fields && !rp.empty() && !rp[0].fields && rp[0].pic.fcm == vc1::kFcmFrameInterlace);
    if (fp.empty() || rp.empty()) return;
    DxvaDecoderConfig config;
    config.profile = DxvaProfile::Vc1Vld;
    config.width = uint32_t(std::max(fp[0].seq.mb_width, rp[0].seq.mb_width) * 16);
    config.height = uint32_t(std::max(fp[0].seq.mb_height, rp[0].seq.mb_height) * 16);
    config.surfaces = 4;
    config.output = PixelFormat::Nv12;
    UniquePointer<IDxvaDecoder> dec = create_dxva_decoder(a, config);
    CHECK(dec);
    if (!dec) return;
    DxvaDecodedPicture out;
    const VcBuffers first = vc_fill(fp[0], 0, 0xFFFF, 0xFFFF, 1, false, 1), second = vc_fill(fp[0], 0, 0xFFFF, 0xFFFF, 2, false, 2);
    // The pair as it should be.
    CHECK(dec->decode(first.view(), &out) == DxvaResult::Ok && out.field == first.pp.bPicStructure);
    CHECK(dec->decode(second.view(), &out) == DxvaResult::Ok && out.field == second.pp.bPicStructure && out.field != first.pp.bPicStructure);
    // A second field with no first field before it, after another picture, into another surface, or of the same parity.
    CHECK(dec->decode(second.view(), &out) == DxvaResult::Invalid && out.detail);
    CHECK(dec->decode(first.view(), &out) == DxvaResult::Ok);
    VcBuffers c = second;
    c.pp.wDecodedPictureIndex = c.pp.wDeblockedPictureIndex = 1;
    CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid);
    CHECK(dec->decode(first.view(), &out) == DxvaResult::Ok);
    c = second;
    c.pp.bPicStructure = first.pp.bPicStructure;
    CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid);
    CHECK(dec->decode(first.view(), &out) == DxvaResult::Ok);
    CHECK(dec->decode(vc_fill(fp[0], 2, 0xFFFF, 0xFFFF, 3, false, 1).view(), &out) == DxvaResult::Ok);
    CHECK(dec->decode(second.view(), &out) == DxvaResult::Invalid);
    // A first field of the wrong parity (TFF), sent as a frame, without bPicExtrapolation 2; a second field's start code on a first field.
    c = first;
    c.pp.bPicStructure = static_cast<uint8_t>(3 - c.pp.bPicStructure);
    CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid && out.detail);
    c = first;
    c.pp.bPicStructure = 3;
    CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid && out.detail);
    c = first;
    c.pp.bPicExtrapolation = 1;
    CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid);
    c = first;
    c.bitstream[3] = 0x0C;
    CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid);
    // A second field without its field start code is taken; with a frame start code it is not.
    CHECK(dec->decode(first.view(), &out) == DxvaResult::Ok);
    c = second;
    c.bitstream[3] = 0x0D;
    CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid);
    // An interlaced frame picture sent as a field, or as progressive.
    const VcBuffers frame = vc_fill(rp[0], 0, 0xFFFF, 0xFFFF, 4);
    CHECK(dec->decode(frame.view(), &out) == DxvaResult::Ok && out.field == 0);
    c = frame;
    c.pp.bPicStructure = 1;
    CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid);
    c = frame;
    c.pp.bPicExtrapolation = 1;
    CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid);
    // A B field pair: bMV_RPS out of its range; intensity compensation said to be on for a B field.
    size_t bi = 0;
    while (bi < fp.size() && fp[bi].pic.type != vc1::kPictureB) ++bi;
    CHECK(bi < fp.size());
    if (bi < fp.size()) {
      c = vc_fill(fp[bi], 1, 0, 2, 5, false, 1);
      c.pp.bMV_RPS = 5;
      CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid && out.detail);
      c.pp.bMV_RPS = 26;
      CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid);
      c = vc_fill(fp[bi], 1, 0, 2, 5, false, 1);
      c.pp.bBidirectionalAveragingMode |= dxva::kVc1AveragingIntensityComp;
      CHECK(dec->decode(c.view(), &out) == DxvaResult::Invalid);
    }
  }
  CHECK(a.live == 0);
}

}  // namespace

static void dxva_test_vc1() {
  printf("VC-1 (DXVA)\n");
  static const char* const streams[] = {"simple_intra", "sp_ipp", "sm_intra", "sm_intra_lf", "sm_intra_nu", "sm_intra_q", "sm_ipp", "sm_ipp_nu", "sm_ipp_q", "sm_ipb",
                                        "sm_ipb_f", "adv_intra", "adv_intra_lf", "adv_intra_odd", "adv_ipp", "adv_ipp_odd", "adv_ipb", "adv_skip", "adv_il_ipp",
                                        "adv_il_ipb", "adv_il_skip",
                                        "adv_fi_ipp", "adv_fi_mv", "adv_fi_b", "adv_fi_slices",                        // (field pictures)
                                        "adv_fr_i", "adv_fr_p", "adv_fr_b", "adv_fr_slices", "adv_fr_narrow",          // (interlaced frame pictures)
                                        "adv_mix_a", "adv_mix_b", "adv_mix_odd",                                       // (all three in one sequence)
                                        "main_rr_ipb", "main_rr_ipp", "adv_rmap_p", "adv_rmap_il"};                    // (range reduction, range mapping)
  int n = 0;
  for (const char* name : streams) vc_test_stream(name, n++ % 2 ? DxvaProfile::Vc1D2010 : DxvaProfile::Vc1Vld);
  vc_test_damaged("sm_ipb");
  vc_test_damaged("adv_ipb");
  vc_test_damaged("adv_skip");
  vc_test_damaged("adv_fi_b");
  vc_test_damaged("adv_fr_b");
  vc_test_damaged("adv_mix_a");
  vc_test_damaged("main_rr_ipb");
  vc_test_refusals();
  vc_test_interlaced_refusals();
}
