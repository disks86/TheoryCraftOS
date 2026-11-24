// DxvaDecoderTest: the VC-1 cases (D3D11_DECODER_PROFILE_VC1_VLD and _VC1_D2010), Simple, Main and Advanced profile.
//
// The program's part: the test runs this project's own VC-1 decoder over tests/data/vc1_*.rcv / .vc1 with a frame observer, which tells it, for every picture in
// decoding order, the sequence and picture headers, the picture's units (the frame and its slices as they are in the stream), where the macroblocks of each begin
// and which pool slots the picture and its references are in - what a program's own parser knows. From that it fills DXVA_PictureParameters and DXVA_SliceInfo as
// ffmpeg's libavcodec/dxva2_vc1.c does (the pool slots are the surfaces; Advanced profile pieces keep their start codes, the buffer is padded to 128 bytes) and
// decodes each picture through the DXVA decoder. Every picture must be the very frame the software decoder made (bit exact), and the frames put in display order and
// cropped must be ffmpeg's decode (tests/data/vc1_*.yuv) exactly.
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

// One picture, as the program's parser knows it.
struct VcPic {
  vc1::SequenceHeader seq;
  vc1::PictureHeader pic;
  int slot = 0, forward = -1, backward = -1;
  std::vector<std::vector<uint8_t>> units;   // as in the stream (Advanced profile: from the start code)
  std::vector<vc1::UnitInfo> info;
  std::vector<uint8_t> frame;                // the software decoder's picture, I420 at the coded size (whole macroblocks)
};

struct VcObserver final : vc1::IFrameObserver {
  std::vector<VcPic> pics;
  void picture_decoded(const vc1::OffloadPicture& p) override {
    VcPic v;
    v.seq = *p.seq;
    v.pic = *p.pic;
    v.slot = p.slot;
    v.forward = p.forward_slot;
    v.backward = p.backward_slot;
    for (const ConstByteView& u : p.units) v.units.push_back(std::vector<uint8_t>(u.data(), u.data() + u.size()));
    for (const vc1::UnitInfo& i : p.unit_info) v.info.push_back(i);
    for (int c = 0; c < 3; ++c) v.frame.insert(v.frame.end(), p.plane[c].data(), p.plane[c].data() + p.plane[c].size());
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
// `strip_start_code`: an Advanced profile frame without its start code (one piece), which a decoder takes too.
static VcBuffers vc_fill(const VcPic& v, uint16_t cur, uint16_t fwd, uint16_t bwd, uint32_t report_id, bool strip_start_code = false) {
  VcBuffers b;
  dxva::PictureParametersVc1& pp = b.pp;
  const vc1::SequenceHeader& s = v.seq;
  const vc1::PictureHeader& h = v.pic;
  const bool adv = s.profile == vc1::kAdvanced;
  const bool intra = h.type == vc1::kPictureI || h.type == vc1::kPictureBI, bpic = h.type == vc1::kPictureB;
  const bool intcomp = h.type == vc1::kPictureP && h.use_ic && (h.lumscale != 32 || h.lumshift != 0);
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
  pp.bPicStructure = 3;
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
  pp.bPicExtrapolation = 1;
  pp.bPicDeblocked = static_cast<uint8_t>((!bpic && s.overlap) << 6 | (!adv && h.rangeredfrm) << 5 | s.loop_filter << 1);
  pp.bPicDeblockConfined = static_cast<uint8_t>(s.postprocflag << 7 | s.broadcast << 6 | s.interlace << 5 | s.tfcntrflag << 4 | s.finterpflag << 3 | (!bpic) << 2 | s.psf << 1 | s.extended_dmv);
  if (h.type != vc1::kPictureI) pp.bPic4MVallowed = h.mv_mode == vc1::kMvMixed || (h.mv_mode == vc1::kMvIntensityComp && h.mv_mode2 == vc1::kMvMixed);
  pp.bReservedBits = static_cast<uint8_t>(h.pq);
  pp.wBitstreamFcodes = static_cast<uint16_t>(intcomp ? h.lumscale : 32);
  pp.wBitstreamPCEelements = static_cast<uint16_t>(intcomp ? h.lumshift : 0);
  const uint32_t mb_count = static_cast<uint32_t>(s.mb_width * s.mb_height);
  for (size_t k = 0; k < v.units.size(); ++k) {
    std::vector<uint8_t> unit = v.units[k];
    const bool coded = unit.size() >= 4 && unit[0] == 0 && unit[1] == 0 && unit[2] == 1;
    if (adv && !coded) unit.insert(unit.begin(), {0, 0, 1, uint8_t(k == 0 ? 0x0D : 0x0B)});   // (ffmpeg puts the start code there)
    if (adv && strip_start_code && k == 0) unit.erase(unit.begin(), unit.begin() + 4);
    dxva::SliceInfoVc1 si = {};
    si.wVerticalPosition = static_cast<uint16_t>(k < v.info.size() ? v.info[k].start_row : 0);
    si.dwSliceBitsInBuffer = static_cast<uint32_t>(8 * unit.size());
    si.dwSliceDataLocation = static_cast<uint32_t>(b.bitstream.size());
    si.bReservedBits = static_cast<uint8_t>(bpic ? h.bfraction_index + 9 : 0);
    si.wMBbitOffset = static_cast<uint16_t>(h.skipped ? 0xFFFF : (k < v.info.size() ? v.info[k].mb_bit_offset : 0) + (adv && !(strip_start_code && k == 0) ? 32 : 0));
    si.wNumberMBsInSlice = static_cast<uint16_t>(si.wVerticalPosition * s.mb_width);   // (made a count below)
    si.wQuantizerScaleCode = static_cast<uint16_t>(h.pq);
    b.bitstream.insert(b.bitstream.end(), unit.begin(), unit.end());
    b.slices.push_back(si);
  }
  for (size_t i = 0; i < b.slices.size(); ++i)
    b.slices[i].wNumberMBsInSlice = static_cast<uint16_t>((i + 1 < b.slices.size() ? b.slices[i + 1].wNumberMBsInSlice : mb_count) - b.slices[i].wNumberMBsInSlice);
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
    VcBuffers b = vc_fill(v, index(v.slot), index(v.forward), index(v.backward), uint32_t(n + 1), strip);
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
      run.decoded.push_back(vc_i420(out.frame));
    } else {
      printf("    picture %zu (type %d): result %d (%s)\n", n, v.pic.type, int(r), out.detail ? out.detail : "");
      run.decoded.push_back({});
    }
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
    for (size_t i = 0; i < run.decoded.size() && i < pics.size(); ++i) {
      CHECK(run.results[i] == DxvaResult::Ok);
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
    for (size_t n = 0; n < pics.size(); ++n) {
      const VcPic& v = pics[n];
      auto idx = [](int s) { return static_cast<uint16_t>(s < 0 ? 0xFFFF : s); };
      const VcBuffers good = vc_fill(v, idx(v.slot), idx(v.forward), idx(v.backward), uint32_t(n + 1));
      DxvaDecodedPicture out;
      for (int variant = 0; variant < 30; ++variant) {
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
      VcBuffers b = good;
      b.pp.wDecodedPictureIndex = kVcSurfaces;
      CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
      if (v.forward >= 0 && (v.pic.type == vc1::kPictureP || v.pic.type == vc1::kPictureB)) {
        b = good;
        b.pp.wForwardRefPictureIndex = 0xFFFF;
        CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
      }
      b = good;
      b.slices[0].dwSliceDataLocation = 0xFFFFFFF0u;
      CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
      b = good;
      b.slices[0].dwSliceBitsInBuffer = 0xFFFFFFFFu;
      CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
      DxvaPicture p = good.view();
      p.slice_control = ConstByteView();
      CHECK(dec->decode(p, &out) == DxvaResult::Invalid);
      p = good.view();
      p.bitstream = ConstByteView();
      CHECK(dec->decode(p, &out) == DxvaResult::Invalid);
      // Then the picture itself, so the next ones have their references.
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
    // Interlaced pictures, field structures, range reduction, post-processing, pan-scan, range mapping: refused as Unsupported.
    const auto unsupported = [&](void (*change)(VcBuffers&)) {
      VcBuffers c = ib;
      change(c);
      return dec->decode(c.view(), &out) == DxvaResult::Unsupported && out.detail != nullptr;
    };
    CHECK(unsupported([](VcBuffers& c) { c.pp.bPicExtrapolation = 2; }));
    CHECK(unsupported([](VcBuffers& c) { c.pp.bPicStructure = 1; }));
    CHECK(unsupported([](VcBuffers& c) { c.pp.bPicDeblocked |= dxva::kVc1DeblockedDeblocking; }));
    CHECK(unsupported([](VcBuffers& c) { c.pp.bPicSpatialResid8 |= dxva::kVc1SpatialPanScan; }));
    CHECK(unsupported([](VcBuffers& c) { c.pp.bPicOBMC = 0x80; }));
    CHECK(unsupported([](VcBuffers& c) { c.slices[0].wBadSliceChopping = 1; }));
    CHECK(unsupported([](VcBuffers& c) { c.bitstream[3] = 0x0C; }));
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
    // A Simple / Main profile picture with range reduction (RANGEREDFRM).
    VcStream sm;
    CHECK(vc_load("sm_ipp", &sm));
    const std::vector<VcPic> smp = vc_observe(a, sm);
    if (!smp.empty()) {
      VcBuffers r = vc_fill(smp[0], 0, 0xFFFF, 0xFFFF, 5);
      r.pp.bPicDeblocked |= dxva::kVc1DeblockedRangeRedFrame;
      CHECK(dec->decode(r.view(), &out) == DxvaResult::Unsupported);
    }
    // After reset() the reference is gone.
    dec->reset();
    pb = vc_fill(pics[p_index], 1, 0, 0xFFFF, 6);
    CHECK(dec->decode(pb.view(), &out) == DxvaResult::MissingReference);
  }
  CHECK(a.live == 0);
}

}  // namespace

static void dxva_test_vc1() {
  printf("VC-1 (DXVA)\n");
  static const char* const streams[] = {"simple_intra", "sp_ipp", "sm_intra", "sm_intra_lf", "sm_intra_nu", "sm_intra_q", "sm_ipp", "sm_ipp_nu", "sm_ipp_q", "sm_ipb",
                                        "sm_ipb_f", "adv_intra", "adv_intra_lf", "adv_intra_odd", "adv_ipp", "adv_ipp_odd", "adv_ipb", "adv_skip", "adv_il_ipp",
                                        "adv_il_ipb", "adv_il_skip"};
  int n = 0;
  for (const char* name : streams) vc_test_stream(name, n++ % 2 ? DxvaProfile::Vc1D2010 : DxvaProfile::Vc1Vld);
  vc_test_damaged("sm_ipb");
  vc_test_damaged("adv_ipb");
  vc_test_damaged("adv_skip");
  vc_test_refusals();
}
