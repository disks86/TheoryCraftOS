// DxvaDecoderTest: the MPEG-1 / MPEG-2 cases (D3D11_DECODER_PROFILE_MPEG2_VLD and _MPEG2and1_VLD).
//
// The test parses tests/data/mpeg2_*.m2v and mpeg1_*.m1v the way a program does and fills DXVA_PictureParameters, DXVA_QmatrixData and DXVA_SliceInfo as ffmpeg's
// libavcodec/dxva2_mpeg2.c does (surfaces from a pool, the slices copied back to back into the bitstream buffer). Every picture (every field) the DXVA decoder gives
// back must be the very frame this project's own MPEG-2 decoder has at that point of the same stream (its frame observer sees each picture in decoding order), and
// the frames put in display order and cropped must be within the tolerance Mpeg2Test allows against ffmpeg's decode (tests/data/*.yuv).
#include "Hardware/VideoCodec/Dxva/DxvaTypesMpeg2.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2Core.hpp"

namespace {

// One picture (frame or field) of the stream, as the program has parsed it.
struct M2Picture {
  bool mpeg1 = false;
  int type = 1;                 // 1 I, 2 P, 3 B
  int structure = 3;
  bool second_field = false;
  uint16_t fcodes = 0xFFFF, pce = 0;
  uint32_t mb_width = 0, frame_mb_height = 0;
  int horizontal_size = 0, vertical_size = 0;
  uint8_t intra[64] = {}, non_intra[64] = {};   // zigzag order
  bool matrices_changed = false;
  std::vector<std::pair<size_t, size_t>> slices;   // [start code, end) in the stream
};

constexpr uint8_t kM2DefaultIntra[64] = {8,  16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29, 29, 34, 34, 34, 29,
                                         29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83};

struct M2Bits {
  const std::vector<uint8_t>& d;
  size_t pos;   // in bits
  uint32_t get(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i, ++pos) v = (v << 1) | (pos / 8 < d.size() ? (d[pos / 8] >> (7 - pos % 8)) & 1 : 0);
    return v;
  }
};

static std::vector<M2Picture> m2_parse(const std::vector<uint8_t>& d) {
  std::vector<M2Picture> pictures;
  std::vector<size_t> codes;
  for (size_t i = 0; i + 3 < d.size(); ++i)
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) codes.push_back(i);
  int width = 0, height = 0;
  bool mpeg1 = true, progressive_sequence = true;
  uint8_t intra[64], non_intra[64];
  bool changed = true;
  M2Picture cur;
  bool have_picture = false, first_field_open = false;
  int first_structure = 0;
  auto flush = [&]() {
    if (have_picture && !cur.slices.empty()) pictures.push_back(cur);
    have_picture = false;
  };
  for (size_t k = 0; k < codes.size(); ++k) {
    const size_t at = codes[k], end = k + 1 < codes.size() ? codes[k + 1] : d.size();
    const int code = d[at + 3];
    M2Bits br{d, (at + 4) * 8};
    if (code == 0xB3) {
      flush();
      width = static_cast<int>(br.get(12));
      height = static_cast<int>(br.get(12));
      br.get(4 + 4 + 18 + 1 + 10 + 1);
      if (br.get(1)) for (uint8_t& v : intra) v = static_cast<uint8_t>(br.get(8));
      else for (int i = 0; i < 64; ++i) intra[i] = kM2DefaultIntra[i];
      if (br.get(1)) for (uint8_t& v : non_intra) v = static_cast<uint8_t>(br.get(8));
      else for (uint8_t& v : non_intra) v = 16;
      mpeg1 = true;
      progressive_sequence = true;
      changed = true;
    } else if (code == 0xB5) {
      const uint32_t id = br.get(4);
      if (id == 1) {
        mpeg1 = false;
        br.get(8);
        progressive_sequence = br.get(1) != 0;
      } else if (id == 3) {
        if (br.get(1)) for (uint8_t& v : intra) v = static_cast<uint8_t>(br.get(8));
        if (br.get(1)) for (uint8_t& v : non_intra) v = static_cast<uint8_t>(br.get(8));
        changed = true;
      } else if (id == 8 && have_picture) {
        uint32_t f = 0;
        for (int i = 0; i < 4; ++i) f = (f << 4) | br.get(4);
        cur.fcodes = static_cast<uint16_t>(f);
        const uint32_t bits = br.get(13);   // intra_dc_precision .. progressive_frame
        cur.pce = static_cast<uint16_t>(bits << 3);
        cur.structure = static_cast<int>((bits >> 9) & 3);
        cur.mpeg1 = false;
      }
    } else if (code == 0x00) {
      flush();
      cur = M2Picture();
      have_picture = true;
      cur.mpeg1 = mpeg1;
      br.get(10);
      cur.type = static_cast<int>(br.get(3));
      br.get(16);
      uint32_t fwd = 15, bwd = 15;
      if (cur.type == 2 || cur.type == 3) {
        br.get(1);
        fwd = br.get(3);
      }
      if (cur.type == 3) {
        br.get(1);
        bwd = br.get(3);
      }
      if (mpeg1) cur.fcodes = static_cast<uint16_t>(fwd << 12 | fwd << 8 | bwd << 4 | bwd);
      cur.horizontal_size = width;
      cur.vertical_size = height;
      for (int i = 0; i < 64; ++i) {
        cur.intra[i] = intra[i];
        cur.non_intra[i] = non_intra[i];
      }
      cur.matrices_changed = changed;
      changed = false;
      cur.mb_width = static_cast<uint32_t>((width + 15) / 16);
      cur.frame_mb_height = static_cast<uint32_t>(progressive_sequence ? (height + 15) / 16 : 2 * ((height + 31) / 32));
    } else if (code >= 0x01 && code <= 0xAF && have_picture) {
      if (cur.slices.empty()) {
        // (the picture's structure is known now: is it the second field of a frame?)
        const bool field = cur.structure != 3;
        cur.second_field = field && first_field_open && first_structure != cur.structure;
        first_field_open = field && !cur.second_field;
        first_structure = cur.structure;
      }
      cur.slices.push_back({at, end});
    } else if (code == 0xB8 || code == 0xB7) {
      flush();
    }
  }
  flush();
  return pictures;
}

// Every picture the software decoder makes, in decoding order (the frame as it is after the picture or field).
struct M2Observer final : mpeg2::IFrameObserver {
  std::vector<std::vector<uint8_t>> frames;   // I420, coded size
  std::vector<std::pair<int, int>> sizes;
  void picture_decoded(const mpeg2::OffloadPicture& p) override {
    std::vector<uint8_t> f;
    for (int c = 0; c < 3; ++c) f.insert(f.end(), p.plane[c].data(), p.plane[c].data() + p.plane[c].size());
    frames.push_back(std::move(f));
  }
};

template <class T>
static ConstByteView m2_bytes(const T& value) {
  // (the test fills the DXVA structures the way a program does and hands their bytes over; the structures are packed byte arrays)
  return ConstByteView(reinterpret_cast<const uint8_t*>(&value), sizeof value);
}

// The buffers of one picture, as ffmpeg's dxva2_mpeg2.c fills them.
struct M2Buffers {
  dxva::PictureParametersMpeg2 pp = {};
  dxva::QmatrixMpeg2 qm = {};
  std::vector<dxva::SliceInfoMpeg2> slices;
  std::vector<uint8_t> bitstream;
  bool send_qmatrix = true;
  DxvaPicture view() const {
    DxvaPicture p;
    p.picture_parameters = m2_bytes(pp);
    if (send_qmatrix) p.quantization_matrix = m2_bytes(qm);
    p.slice_control = ConstByteView(reinterpret_cast<const uint8_t*>(slices.data()), slices.size() * sizeof(dxva::SliceInfoMpeg2));
    p.bitstream = ConstByteView(bitstream.data(), bitstream.size());
    return p;
  }
};

static M2Buffers m2_fill(const std::vector<uint8_t>& stream, const M2Picture& pic, uint16_t cur, uint16_t fwd, uint16_t bwd) {
  M2Buffers b;
  dxva::PictureParametersMpeg2& pp = b.pp;
  const bool field = pic.structure != 3;
  pp.wDecodedPictureIndex = cur;
  pp.wDeblockedPictureIndex = 0;
  pp.wForwardRefPictureIndex = pic.type != 1 ? fwd : 0xFFFF;
  pp.wBackwardRefPictureIndex = pic.type == 3 ? bwd : 0xFFFF;
  pp.wPicWidthInMBminus1 = static_cast<uint16_t>(pic.mb_width - 1);
  pp.wPicHeightInMBminus1 = static_cast<uint16_t>((pic.frame_mb_height >> (field ? 1 : 0)) - 1);
  pp.bMacroblockWidthMinus1 = 15;
  pp.bMacroblockHeightMinus1 = 15;
  pp.bBlockWidthMinus1 = 7;
  pp.bBlockHeightMinus1 = 7;
  pp.bBPPminus1 = 7;
  pp.bPicStructure = static_cast<uint8_t>(pic.structure);
  pp.bSecondField = pic.second_field;
  pp.bPicIntra = pic.type == 1;
  pp.bPicBackwardPrediction = pic.type == 3;
  pp.bChromaFormat = 1;
  pp.bPicScanFixed = 1;
  pp.bPicScanMethod = (pic.pce & dxva::kMpeg2PceAlternateScan) ? 1 : 0;
  pp.wBitstreamFcodes = pic.fcodes;
  pp.wBitstreamPCEelements = pic.mpeg1 ? 0 : pic.pce;
  for (int m = 0; m < 4; ++m) {
    b.qm.bNewQmatrix[m] = 1;
    for (int i = 0; i < 64; ++i) b.qm.Qmatrix[m][i] = (m & 1) ? pic.non_intra[i] : pic.intra[i];
  }
  for (const auto& s : pic.slices) {
    dxva::SliceInfoMpeg2 si = {};
    const size_t size = s.second - s.first;
    si.wVerticalPosition = static_cast<uint16_t>(stream[s.first + 3] - 1);
    si.dwSliceBitsInBuffer = static_cast<uint32_t>(8 * size);
    si.dwSliceDataLocation = static_cast<uint32_t>(b.bitstream.size());
    M2Bits br{stream, (s.first + 4) * 8};
    si.wQuantizerScaleCode = static_cast<uint16_t>(br.get(5));
    while (br.get(1)) br.get(8);
    si.wMBbitOffset = static_cast<uint16_t>(br.pos - s.first * 8);
    b.bitstream.insert(b.bitstream.end(), stream.begin() + static_cast<long>(s.first), stream.begin() + static_cast<long>(s.second));
    b.slices.push_back(si);
  }
  for (size_t i = 0; i + 1 < b.slices.size(); ++i) b.slices[i].wNumberMBsInSlice = static_cast<uint16_t>((b.slices[i + 1].wVerticalPosition - b.slices[i].wVerticalPosition) * pic.mb_width);
  if (!b.slices.empty()) b.slices.back().wNumberMBsInSlice = static_cast<uint16_t>((pic.frame_mb_height >> (field ? 1 : 0)) * pic.mb_width - b.slices.back().wVerticalPosition * pic.mb_width);
  while (b.bitstream.size() % 128) b.bitstream.push_back(0);   // (ffmpeg pads the buffer)
  return b;
}

// The program's surface pool: ffmpeg's frames come from a pool; the anchors (and a frame whose second field is to come) stay out of it.
struct M2Program {
  uint16_t pool = 8;
  uint16_t next = 0;
  int older = -1, newer = -1, open_frame = -1;
  uint16_t pick() {
    for (;;) {
      const uint16_t s = next;
      next = static_cast<uint16_t>((next + 1) % pool);
      if (s != older && s != newer && s != open_frame) return s;
    }
  }
};

// The I420 frame (coded size) of an NV12 picture.
static std::vector<uint8_t> m2_i420(const VideoFrame& f) {
  std::vector<uint8_t> out;
  const uint32_t w = f.format.width, h = f.format.height;
  for (uint32_t y = 0; y < h; ++y) out.insert(out.end(), f.plane[0] + size_t(y) * f.stride[0], f.plane[0] + size_t(y) * f.stride[0] + w);
  for (int c = 0; c < 2; ++c)
    for (uint32_t y = 0; y < h / 2; ++y)
      for (uint32_t x = 0; x < w / 2; ++x) out.push_back(f.plane[1][size_t(y) * f.stride[1] + 2 * x + c]);
  return out;
}

struct M2Run {
  std::vector<std::vector<uint8_t>> decoded;   // per picture, I420 coded size
  std::vector<DxvaResult> results;
};

// Decodes the stream through the DXVA decoder. `keys`: give the decoder a surface_keys table and permute the indices from picture to picture (the key of a surface
// stays the same). `qmatrix_when_changed`: send the matrix buffer only when the matrices change.
// `output_key`: also name the current picture's surface by DxvaPicture::output_key, with a CurrPic index (200) that is not in the key table (Direct3D 12's way).
static M2Run m2_run(TestAllocator& a, DxvaProfile profile, const std::vector<uint8_t>& stream, const std::vector<M2Picture>& pics, bool keys, bool qmatrix_when_changed,
                    bool output_key = false) {
  M2Run run;
  DxvaDecoderConfig config;
  config.profile = profile;
  config.width = pics.empty() ? 16 : pics[0].mb_width * 16;
  config.height = pics.empty() ? 16 : pics[0].frame_mb_height * 16;
  config.surfaces = 8;
  config.output = PixelFormat::Nv12;
  UniquePointer<IDxvaDecoder> dec = create_dxva_decoder(a, config);
  CHECK(dec);
  if (!dec) return run;
  M2Program prog;
  uint32_t perm[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  uint32_t key_table[8];
  for (size_t n = 0; n < pics.size(); ++n) {
    const M2Picture& pic = pics[n];
    int cur;
    if (pic.second_field && prog.open_frame >= 0) {
      cur = prog.open_frame;
    } else {
      cur = prog.pick();
    }
    const int fwd = pic.type == 3 ? prog.older : prog.newer, bwd = prog.newer;
    // Surface s is index perm[s] in this call (keys: s + 1000 for surface s).
    if (keys) {
      for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t j = static_cast<uint32_t>((n * 5 + i * 3) % 8);
        const uint32_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
      }
      for (uint32_t s = 0; s < 8; ++s) key_table[perm[s]] = 1000 + s;
    }
    auto index = [&](int s) { return static_cast<uint16_t>(s < 0 ? 0xFFFF : keys ? perm[s] : static_cast<uint32_t>(s)); };
    M2Buffers b = m2_fill(stream, pic, index(cur), index(fwd), index(bwd));
    if (qmatrix_when_changed) b.send_qmatrix = pic.matrices_changed;
    DxvaPicture view = b.view();
    if (keys) view.surface_keys = ArrayView<const uint32_t>(key_table, 8);
    if (keys && output_key) {
      b.pp.wDecodedPictureIndex = 200;
      view.output_key = 1000 + static_cast<uint32_t>(cur);
    }
    DxvaDecodedPicture out;
    const DxvaResult r = dec->decode(view, &out);
    run.results.push_back(r);
    if (r == DxvaResult::Ok || r == DxvaResult::MissingReference) {
      CHECK(out.surface == (keys && output_key ? 200u : index(cur)));
      CHECK(out.frame.format.pixel_format == PixelFormat::Nv12 && out.frame.format.width == pic.mb_width * 16 && out.frame.format.height == pic.frame_mb_height * 16);
      run.decoded.push_back(m2_i420(out.frame));
    } else {
      printf("    picture %zu: result %d (%s)\n", n, int(r), out.detail ? out.detail : "");
      run.decoded.push_back({});
    }
    // The program's reference bookkeeping (ffmpeg: last_picture / next_picture).
    const bool field = pic.structure != 3;
    if (field && !pic.second_field) prog.open_frame = cur;
    else prog.open_frame = -1;
    if (pic.type != 3 && (!field || pic.second_field)) {   // (the anchors move when the frame is complete)
      prog.older = prog.newer;
      prog.newer = cur;
    }
  }
  return run;
}

static void m2_test_stream(const char* file, const char* reference, DxvaProfile profile) {
  const std::vector<uint8_t> stream = read_file(std::string("tests/data/") + file);
  const std::vector<uint8_t> ref = read_file(std::string("tests/data/") + reference);
  CHECK(!stream.empty() && !ref.empty());
  if (stream.empty() || ref.empty()) return;
  const std::vector<M2Picture> pics = m2_parse(stream);
  TestAllocator a;
  {
    // The software decoder's pictures, in decoding order.
    M2Observer observer;
    {
      Mpeg2Decoder soft(a);
      soft.set_frame_observer(&observer);
      soft.decode(ConstByteView(stream.data(), stream.size()), 0);
      soft.flush();
    }
    const M2Run run = m2_run(a, profile, stream, pics, false, false);
    int exact = 0;
    CHECK(run.decoded.size() == pics.size() && observer.frames.size() == pics.size());
    for (size_t i = 0; i < run.decoded.size() && i < observer.frames.size(); ++i) {
      CHECK(run.results[i] == DxvaResult::Ok);
      if (run.decoded[i] == observer.frames[i]) ++exact;
      else if (getenv("M2_DEBUG")) printf("    picture %zu (type %d, structure %d, second %d) differs\n", i, pics[i].type, pics[i].structure, int(pics[i].second_field));
    }
    CHECK(exact == int(pics.size()));
    // Display order, cropped, against ffmpeg's pictures.
    int max_diff = 0, frames = 0;
    double sum = 0;
    uint64_t count = 0;
    const int w = pics.empty() ? 0 : pics[0].horizontal_size, h = pics.empty() ? 0 : pics[0].vertical_size;
    const size_t frame_bytes = size_t(w) * h + 2 * size_t((w + 1) / 2) * ((h + 1) / 2);
    auto compare = [&](const std::vector<uint8_t>& coded, const M2Picture& pic) {
      const size_t offset = size_t(frames) * frame_bytes;
      ++frames;
      if (coded.empty() || offset + frame_bytes > ref.size()) {
        max_diff = 255;
        return;
      }
      const int cw = static_cast<int>(pic.mb_width * 16), ch = static_cast<int>(pic.frame_mb_height * 16);
      size_t p = offset;
      const size_t plane_offset[3] = {0, size_t(cw) * ch, size_t(cw) * ch + size_t(cw / 2) * (ch / 2)};
      for (int c = 0; c < 3; ++c) {
        const int pw = c ? (w + 1) / 2 : w, ph = c ? (h + 1) / 2 : h, stride = c ? cw / 2 : cw;
        for (int y = 0; y < ph; ++y)
          for (int x = 0; x < pw; ++x) {
            const int d = abs(int(coded[plane_offset[c] + size_t(y) * stride + x]) - int(ref[p++]));
            if (d > max_diff) max_diff = d;
            sum += d;
            ++count;
          }
      }
    };
    int held = -1;
    for (size_t i = 0; i < pics.size() && i < run.decoded.size(); ++i) {
      const M2Picture& pic = pics[i];
      if (pic.structure != 3 && !pic.second_field) continue;   // the frame is complete after its second field
      if (pic.type == 3) {
        compare(run.decoded[i], pic);
      } else {
        if (held >= 0) compare(run.decoded[size_t(held)], pics[size_t(held)]);
        held = static_cast<int>(i);
      }
    }
    if (held >= 0) compare(run.decoded[size_t(held)], pics[size_t(held)]);
    const double mean = count ? sum / double(count) : 0;
    printf("  %-24s %zu pictures, %d bit exact with Mpeg2Decoder; %d frames, largest difference to ffmpeg %d, mean %.4f\n", file, pics.size(), exact, frames, max_diff, mean);
    CHECK(size_t(frames) * frame_bytes == ref.size());
    CHECK(max_diff <= 3 && mean < 0.05);
    // The same with surface keys whose indices move from picture to picture, and with the matrix buffer sent only when the matrices change.
    const M2Run keyed = m2_run(a, profile, stream, pics, true, false);
    CHECK(keyed.decoded == run.decoded);
    const M2Run output_keyed = m2_run(a, profile, stream, pics, true, false, true);
    CHECK(output_keyed.decoded == run.decoded);
    const M2Run sparse = m2_run(a, profile, stream, pics, false, true);
    CHECK(sparse.decoded == run.decoded);
  }
  CHECK(a.live == 0);
}

// Damaged and malformed buffers: Invalid (or, for damaged slice data, a picture) and never a fault, a sanitizer report or a leak.
static void m2_test_damaged(const char* file, DxvaProfile profile) {
  const std::vector<uint8_t> stream = read_file(std::string("tests/data/") + file);
  CHECK(!stream.empty());
  if (stream.empty()) return;
  const std::vector<M2Picture> pics = m2_parse(stream);
  TestAllocator a;
  int invalid = 0, decoded = 0;
  {
    DxvaDecoderConfig config;
    config.profile = profile;
    config.width = pics[0].mb_width * 16;
    config.height = pics[0].frame_mb_height * 16;
    config.surfaces = 8;
    config.output = PixelFormat::Nv12;
    UniquePointer<IDxvaDecoder> dec = create_dxva_decoder(a, config);
    CHECK(dec);
    if (!dec) return;
    M2Program prog;
    for (size_t n = 0; n < pics.size(); ++n) {
      const M2Picture& pic = pics[n];
      const int cur = pic.second_field && prog.open_frame >= 0 ? prog.open_frame : prog.pick();
      const int fwd = pic.type == 3 ? prog.older : prog.newer, bwd = prog.newer;
      const M2Buffers good = m2_fill(stream, pic, uint16_t(cur), uint16_t(fwd < 0 ? 0xFFFF : fwd), uint16_t(bwd < 0 ? 0xFFFF : bwd));
      DxvaDecodedPicture out;
      auto tally = [&](DxvaResult r) {
        if (r == DxvaResult::Invalid || r == DxvaResult::Unsupported) ++invalid;
        else ++decoded;
        if (r == DxvaResult::Ok || r == DxvaResult::MissingReference) CHECK(out.frame.plane[0] != nullptr);
      };
      // Each buffer cut short and with bits flipped.
      for (int variant = 0; variant < 24; ++variant) {
        M2Buffers b = good;
        std::vector<uint8_t> pp(reinterpret_cast<const uint8_t*>(&b.pp), reinterpret_cast<const uint8_t*>(&b.pp) + sizeof b.pp);
        std::vector<uint8_t> qm(reinterpret_cast<const uint8_t*>(&b.qm), reinterpret_cast<const uint8_t*>(&b.qm) + sizeof b.qm);
        std::vector<uint8_t> sc(reinterpret_cast<const uint8_t*>(b.slices.data()), reinterpret_cast<const uint8_t*>(b.slices.data()) + b.slices.size() * sizeof(dxva::SliceInfoMpeg2));
        std::vector<uint8_t>& bs = b.bitstream;
        std::vector<uint8_t>* target = variant % 4 == 0 ? &pp : variant % 4 == 1 ? &qm : variant % 4 == 2 ? &sc : &bs;
        const size_t salt = n * 7919 + size_t(variant) * 104729;
        if (variant < 12) target->resize(target->size() * size_t(variant / 4 + 1) / 4);
        else if (!target->empty()) for (int k = 0; k < 1 + variant % 3; ++k) (*target)[(salt + size_t(k) * 31) % target->size()] ^= uint8_t(1u << ((salt + size_t(k)) % 8));
        DxvaPicture p;
        p.picture_parameters = ConstByteView(pp.data(), pp.size());
        p.quantization_matrix = ConstByteView(qm.data(), qm.size());
        p.slice_control = ConstByteView(sc.data(), sc.size());
        p.bitstream = ConstByteView(bs.data(), bs.size());
        const DxvaResult r = dec->decode(p, &out);
        if (variant < 12 && target != &bs && (target != &sc || sc.size() % sizeof(dxva::SliceInfoMpeg2) != 0)) CHECK(r == DxvaResult::Invalid);   // a structure cut short
        tally(r);
      }
      // Indices out of range: past config.surfaces, past the surface_keys table.
      {
        M2Buffers b = good;
        b.pp.wDecodedPictureIndex = 8;
        CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
        if (pic.type != 1) {
          b = good;
          b.pp.wForwardRefPictureIndex = 200;
          CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
        }
        if (pic.type == 3) {
          b = good;
          b.pp.wBackwardRefPictureIndex = 0xFFFF;
          CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
        }
        b = good;
        const uint32_t keys[4] = {1000, 1001, 1002, 1003};
        DxvaPicture p = b.view();
        p.surface_keys = ArrayView<const uint32_t>(keys, 4);
        b.pp.wDecodedPictureIndex = 4;
        p = b.view();
        p.surface_keys = ArrayView<const uint32_t>(keys, 4);
        CHECK(dec->decode(p, &out) == DxvaResult::Invalid);
        // A slice past the end of the bitstream buffer, a slice that does not start with a start code.
        b = good;
        b.slices[0].dwSliceDataLocation = 0xFFFFFFF0u;
        CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
        b = good;
        b.slices[0].dwSliceBitsInBuffer = 0xFFFFFFFFu;
        CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
        b = good;
        b.slices[0].dwSliceDataLocation += 1;
        CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
        b = good;
        b.qm.Qmatrix[0][5] = 0;
        CHECK(dec->decode(b.view(), &out) == DxvaResult::Invalid);
        // Missing buffers.
        p = good.view();
        p.picture_parameters = ConstByteView();
        CHECK(dec->decode(p, &out) == DxvaResult::Invalid);
        p = good.view();
        p.slice_control = ConstByteView();
        CHECK(dec->decode(p, &out) == DxvaResult::Invalid);
        p = good.view();
        p.bitstream = ConstByteView();
        CHECK(dec->decode(p, &out) == DxvaResult::Invalid);
      }
      // Then the picture itself, so the next ones have their references.
      const DxvaResult r = dec->decode(good.view(), &out);
      CHECK(r == DxvaResult::Ok || r == DxvaResult::MissingReference);
      const bool field = pic.structure != 3;
      prog.open_frame = field && !pic.second_field ? cur : -1;
      if (pic.type != 3 && (!field || pic.second_field)) {   // (the anchors move when the frame is complete)
        prog.older = prog.newer;
        prog.newer = cur;
      }
    }
  }
  printf("  %-24s damaged buffers: %d refused, %d decoded\n", file, invalid, decoded);
  CHECK(invalid > 0 && decoded > 0 && a.live == 0);
}

// What the decoder refuses or reports for pictures that are well formed but cannot be decoded as asked.
static void m2_test_refusals() {
  const std::vector<uint8_t> stream = read_file("tests/data/mpeg2_ibp.m2v");
  const std::vector<uint8_t> mpeg1 = read_file("tests/data/mpeg1_ibp.m1v");
  CHECK(!stream.empty() && !mpeg1.empty());
  if (stream.empty() || mpeg1.empty()) return;
  const std::vector<M2Picture> pics = m2_parse(stream), pics1 = m2_parse(mpeg1);
  TestAllocator a;
  {
    DxvaDecoderConfig config;
    config.profile = DxvaProfile::Mpeg2Vld;
    config.width = pics[0].mb_width * 16;
    config.height = pics[0].frame_mb_height * 16;
    config.surfaces = 4;
    config.output = PixelFormat::Nv12;
    UniquePointer<IDxvaDecoder> dec = create_dxva_decoder(a, config);
    CHECK(dec);
    if (!dec) return;
    DxvaDecodedPicture out;
    // An MPEG-1 picture with the MPEG-2-only profile.
    M2Buffers one = m2_fill(mpeg1, pics1[0], 0, 0xFFFF, 0xFFFF);
    CHECK(dec->decode(one.view(), &out) == DxvaResult::Invalid);
    // A P picture whose reference surface holds nothing: decoded all the same, with MissingReference.
    size_t p_index = 0;
    while (p_index < pics.size() && pics[p_index].type != 2) ++p_index;
    CHECK(p_index < pics.size());
    if (p_index < pics.size()) {
      M2Buffers b = m2_fill(stream, pics[p_index], 1, 2, 0xFFFF);
      CHECK(dec->decode(b.view(), &out) == DxvaResult::MissingReference && out.frame.plane[0] && out.surface == 1);
    }
    M2Buffers b = m2_fill(stream, pics[0], 0, 0xFFFF, 0xFFFF);
    CHECK(dec->decode(b.view(), &out) == DxvaResult::Ok);
    // A picture decoded into the surface it predicts from: the same picture as decoded elsewhere (the reference is read before the surface is overwritten).
    if (p_index < pics.size()) {
      M2Buffers p = m2_fill(stream, pics[p_index], 1, 0, 0xFFFF);
      CHECK(dec->decode(p.view(), &out) == DxvaResult::Ok);
      const std::vector<uint8_t> elsewhere = m2_i420(out.frame);
      p = m2_fill(stream, pics[p_index], 0, 0, 0xFFFF);
      CHECK(dec->decode(p.view(), &out) == DxvaResult::Ok && out.surface == 0);
      CHECK(m2_i420(out.frame) == elsewhere);
      CHECK(dec->decode(b.view(), &out) == DxvaResult::Ok);
    }
    // Larger than the surfaces; 4:2:2; H.263 prediction; a chopped slice; a start code not at the first bit.
    M2Buffers big = b;
    big.pp.wPicWidthInMBminus1 = static_cast<uint16_t>(pics[0].mb_width + 1);
    CHECK(dec->decode(big.view(), &out) == DxvaResult::Invalid);
    M2Buffers c422 = b;
    c422.pp.bChromaFormat = 2;
    CHECK(dec->decode(c422.view(), &out) == DxvaResult::Invalid);
    M2Buffers obmc = b;
    obmc.pp.bPicOBMC = 1;
    CHECK(dec->decode(obmc.view(), &out) == DxvaResult::Unsupported && out.detail);
    M2Buffers chopped = b;
    chopped.slices[0].wBadSliceChopping = 1;
    CHECK(dec->decode(chopped.view(), &out) == DxvaResult::Unsupported && out.detail);
    M2Buffers shifted = b;
    shifted.slices[0].bStartCodeBitOffset = 3;
    CHECK(dec->decode(shifted.view(), &out) == DxvaResult::Unsupported);
    M2Buffers structure = b;
    structure.pp.bPicStructure = 1;   // not the picture_structure the coding extension says
    CHECK(dec->decode(structure.view(), &out) == DxvaResult::Invalid);
    // After reset() the reference is gone.
    dec->reset();
    if (p_index < pics.size()) {
      M2Buffers p = m2_fill(stream, pics[p_index], 1, 0, 0xFFFF);
      CHECK(dec->decode(p.view(), &out) == DxvaResult::MissingReference);
    }
  }
  CHECK(a.live == 0);
}

}  // namespace

static void dxva_test_mpeg2() {
  printf("MPEG-2 / MPEG-1 (DXVA)\n");
  static const char* const streams[] = {"intra", "ipp", "ibp", "interlaced", "interlaced_bff", "alt", "dc10", "highrate", "lowrate", "matrix", "oddsize", "syn_frames", "syn_fields", "syn_fields_bff"};
  for (const char* name : streams) {
    const std::string base = std::string("mpeg2_") + name;
    m2_test_stream((base + ".m2v").c_str(), (base + ".yuv").c_str(), DxvaProfile::Mpeg2Vld);
  }
  m2_test_stream("mpeg2_ibp.m2v", "mpeg2_ibp.yuv", DxvaProfile::Mpeg2and1Vld);
  m2_test_stream("mpeg2_syn_fields.m2v", "mpeg2_syn_fields.yuv", DxvaProfile::Mpeg2and1Vld);
  for (const char* name : {"ibp", "highrate", "matrix"}) {
    const std::string base = std::string("mpeg1_") + name;
    m2_test_stream((base + ".m1v").c_str(), (base + ".yuv").c_str(), DxvaProfile::Mpeg2and1Vld);
  }
  m2_test_damaged("mpeg2_ibp.m2v", DxvaProfile::Mpeg2Vld);
  m2_test_damaged("mpeg2_interlaced.m2v", DxvaProfile::Mpeg2Vld);
  m2_test_damaged("mpeg2_syn_fields.m2v", DxvaProfile::Mpeg2Vld);
  m2_test_damaged("mpeg1_ibp.m1v", DxvaProfile::Mpeg2and1Vld);
  m2_test_refusals();
}
