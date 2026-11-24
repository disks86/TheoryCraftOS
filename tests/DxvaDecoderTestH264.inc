// DxvaDecoderTest: the H.264 cases. The test is the program: it parses each stream with the kernel's own H264StreamParser, keeps the decoded picture buffer with
// h264::Dpb (the rules every H.264 decoder follows: reference marking, order counts), assigns surfaces from a pool and fills DXVA_PicParams_H264, DXVA_Qmatrix_H264
// and DXVA_Slice_H264_Short as ffmpeg's libavcodec/dxva2_h264.c does (RefFrameList / FrameNumList / FieldOrderCntList / UsedForReferenceFlags from the buffer, the
// scaling lists in zigzag order, each slice NAL unit after a 00 00 01 start code, the last slice padded to 128 bytes). Every picture the DXVA decoder gives (decoding
// order, uncropped NV12) is cropped and compared with ffmpeg's decode of the stream (tests/data/dxva_h264_md5.txt, display order: see make_dxva_h264_references.sh)
// and with this project's H264Decoder - bit exact.
#include <algorithm>
#include <random>
#include "Crypto/Crypto.hpp"
#include "Hardware/VideoCodec/Dxva/DxvaTypesH264.hpp"
#include "Hardware/VideoCodec/Software/H264.hpp"
#include "Hardware/VideoCodec/Software/H264Parser.hpp"

namespace dxva_h264_test {

using dxva::PicParamsH264;
using dxva::QmatrixH264;
using dxva::SliceH264Short;

struct Call {
  std::vector<uint8_t> params, qm, slices, bitstream;
  uint32_t surface = 0;
  bool intra = false;     // every slice is I (decodes without references)
};

struct Stream {
  std::vector<Call> calls;   // decoding order
  uint32_t mb_w = 0, mb_h = 0;
  int crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
  std::vector<std::pair<size_t, size_t>> access_units;   // byte ranges for H264Decoder
};

constexpr uint32_t kPool = 20;   // surfaces in the program's pool

template <class T>
std::vector<uint8_t> bytes_of(const T& v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);   // test only: the packed mirrors are the DXVA bytes on this little-endian host
  return std::vector<uint8_t>(p, p + sizeof(T));
}

// Parses `data` and records the decode calls ffmpeg's DXVA H.264 code would make.
bool record(TestAllocator& alloc, const std::vector<uint8_t>& data, Stream* st) {
  H264StreamParser parser(alloc);
  List<H264Slice> slices(alloc);
  if (!parser.parse(ConstByteView(data.data(), data.size()), &slices) || slices.count() == 0) return false;
  h264::PictureInfo infos[h264::kMaxPictures];
  h264::PictureInfo* slots[h264::kMaxPictures];
  for (int i = 0; i < h264::kMaxPictures; ++i) slots[i] = &infos[i];
  h264::Dpb dpb;
  dpb.attach(slots, h264::kMaxPictures);
  dpb.set_decode_order(true);
  uint32_t slot_surface[h264::kMaxPictures] = {};
  uint32_t next_surface = 0;
  uint32_t feedback = 0;
  for (uint64_t first = 0; first < slices.count();) {
    uint64_t end = first + 1;
    while (end < slices.count() && !slices[end].starts_picture) ++end;
    const H264Slice& s0 = slices[first];
    const h264::Sps& sps = *parser.sps_for(s0.header);
    const h264::Pps& pps = *parser.pps_for(s0.header);
    // The access unit for the CPU decoder: from the start of this picture's first slice (the parameter sets before the first one included) to the next picture's.
    const size_t au_begin = first == 0 ? 0 : static_cast<size_t>(s0.nal_offset - 3);
    const size_t au_end = end < slices.count() ? static_cast<size_t>(slices[end].nal_offset - 3) : data.size();
    st->access_units.push_back({au_begin, au_end});
    st->mb_w = static_cast<uint32_t>(sps.mb_w);
    st->mb_h = static_cast<uint32_t>(sps.mb_h);
    st->crop_l = sps.crop_l, st->crop_r = sps.crop_r, st->crop_t = sps.crop_t, st->crop_b = sps.crop_b;
    dpb.set_sps(&sps);
    dpb.release_returned();
    h264::PictureInfo* cur = dpb.begin_picture(s0.header, 0);
    if (!cur) return false;
    cur->has_memory = true;
    // A surface no reference is in, taken round the pool.
    for (uint32_t tries = 0; tries < kPool; ++tries) {
      const uint32_t s = (next_surface + tries) % kPool;
      bool busy = false;
      for (int i = 0; i < h264::kMaxPictures; ++i)
        if (&infos[i] != cur && infos[i].is_ref && slot_surface[i] == s) busy = true;
      if (!busy) {
        slot_surface[cur->slot] = s;
        next_surface = s + 1;
        break;
      }
    }
    PicParamsH264 pp;
    memset(&pp, 0, sizeof pp);
    pp.wFrameWidthInMbsMinus1 = static_cast<uint16_t>(sps.mb_w - 1);
    pp.wFrameHeightInMbsMinus1 = static_cast<uint16_t>(sps.mb_h - 1);
    pp.CurrPic = static_cast<uint8_t>(slot_surface[cur->slot]);
    pp.num_ref_frames = static_cast<uint8_t>(sps.max_num_ref_frames);
    bool intra = true;
    for (uint64_t k = first; k < end; ++k) intra = intra && slices[k].header.type == h264::kSliceI;
    uint16_t f = 0;
    f |= static_cast<uint16_t>(1u << dxva::kH264ChromaFormatShift);
    if (s0.header.nal_ref_idc) f |= dxva::kH264RefPic;
    if (pps.constrained_intra_pred) f |= dxva::kH264ConstrainedIntraPred;
    if (pps.weighted_pred) f |= dxva::kH264WeightedPred;
    f |= static_cast<uint16_t>(pps.weighted_bipred_idc << dxva::kH264WeightedBipredShift);
    f |= dxva::kH264MbsConsecutive;
    if (sps.frame_mbs_only) f |= dxva::kH264FrameMbsOnly;
    if (pps.transform_8x8) f |= dxva::kH264Transform8x8;
    if (sps.level_idc >= 31) f |= dxva::kH264MinLumaBipredSize8x8;
    if (s0.header.type == h264::kSliceI) f |= dxva::kH264IntraPic;
    pp.wBitFields = f;
    pp.Reserved16Bits = 3;
    pp.StatusReportFeedbackNumber = ++feedback;
    memset(pp.RefFrameList, 0xFF, sizeof pp.RefFrameList);
    int n = 0;
    for (int pass = 0; pass < 2; ++pass)   // short-term references, then long-term ones (ffmpeg's order)
      for (int i = 0; i < h264::kMaxPictures; ++i) {
        const h264::PictureInfo& r = infos[i];
        if (&r == cur || !r.is_ref || r.is_long != (pass == 1) || n >= 16) continue;
        pp.RefFrameList[n] = static_cast<uint8_t>(slot_surface[i] | (r.is_long ? 0x80 : 0));
        pp.FieldOrderCntList[n][0] = r.top_poc;
        pp.FieldOrderCntList[n][1] = r.bottom_poc;
        pp.FrameNumList[n] = static_cast<uint16_t>(r.is_long ? r.long_term_idx : r.frame_num);
        pp.UsedForReferenceFlags |= 3u << (2 * n);
        ++n;
      }
    pp.CurrFieldOrderCnt[0] = cur->top_poc;
    pp.CurrFieldOrderCnt[1] = cur->bottom_poc;
    pp.pic_init_qs_minus26 = static_cast<int8_t>(pps.pic_init_qs - 26);
    pp.chroma_qp_index_offset = static_cast<int8_t>(pps.chroma_qp_offset[0]);
    pp.second_chroma_qp_index_offset = static_cast<int8_t>(pps.chroma_qp_offset[1]);
    pp.ContinuationFlag = 1;
    pp.pic_init_qp_minus26 = static_cast<int8_t>(pps.pic_init_qp - 26);
    pp.num_ref_idx_l0_active_minus1 = static_cast<uint8_t>(pps.num_ref_idx_default[0] - 1);
    pp.num_ref_idx_l1_active_minus1 = static_cast<uint8_t>(pps.num_ref_idx_default[1] - 1);
    pp.frame_num = static_cast<uint16_t>(s0.header.frame_num);
    pp.log2_max_frame_num_minus4 = static_cast<uint8_t>(sps.log2_max_frame_num - 4);
    pp.pic_order_cnt_type = static_cast<uint8_t>(sps.poc_type);
    if (sps.poc_type == 0) pp.log2_max_pic_order_cnt_lsb_minus4 = static_cast<uint8_t>(sps.log2_max_poc_lsb - 4);
    else if (sps.poc_type == 1) pp.delta_pic_order_always_zero_flag = sps.delta_pic_order_always_zero;
    pp.direct_8x8_inference_flag = sps.direct_8x8_inference;
    pp.entropy_coding_mode_flag = pps.cabac;
    pp.pic_order_present_flag = pps.bottom_field_pic_order;
    pp.deblocking_filter_control_present_flag = pps.deblocking_control;
    pp.redundant_pic_cnt_present_flag = pps.redundant_pic_cnt_present;
    // The matrices in force, in the bitstream's order (DXVA_Qmatrix_H264; flat 16 when the stream has none).
    QmatrixH264 qm;
    uint8_t w4[6][16], w8[2][64];
    parser.parameter_sets()->effective_scaling(sps, pps, w4, w8);
    h264::ParameterSets::scaling_to_bitstream_order(w4, w8, qm.bScalingLists4x4, qm.bScalingLists8x8);
    Call call;
    call.surface = pp.CurrPic;
    call.intra = intra;
    call.params = bytes_of(pp);
    call.qm = bytes_of(qm);
    for (uint64_t k = first; k < end; ++k) {
      const H264Slice& s = slices[k];
      SliceH264Short sc;
      sc.BSNALunitDataLocation = static_cast<uint32_t>(call.bitstream.size());
      sc.SliceBytesInBuffer = static_cast<uint32_t>(3 + s.nal_size);
      sc.wBadSliceChopping = 0;
      call.bitstream.insert(call.bitstream.end(), {0, 0, 1});
      call.bitstream.insert(call.bitstream.end(), data.begin() + static_cast<long>(s.nal_offset), data.begin() + static_cast<long>(s.nal_offset + s.nal_size));
      if (k + 1 == end) {   // ffmpeg pads the bitstream to 128 bytes and counts the padding in the last slice
        const size_t padding = (128 - call.bitstream.size() % 128) % 128;
        call.bitstream.insert(call.bitstream.end(), padding, 0);
        sc.SliceBytesInBuffer += static_cast<uint32_t>(padding);
      }
      std::vector<uint8_t> b = bytes_of(sc);
      call.slices.insert(call.slices.end(), b.begin(), b.end());
    }
    st->calls.push_back(std::move(call));
    dpb.finish_picture(slices[end - 1].header);
    first = end;
  }
  return true;
}

// A decoded picture as packed bytes.
std::vector<uint8_t> copy_nv12(const VideoFrame& f) {
  std::vector<uint8_t> out;
  const uint32_t w = f.format.width, h = f.format.height;
  for (uint32_t y = 0; y < h; ++y) out.insert(out.end(), f.plane[0] + uint64_t{y} * f.stride[0], f.plane[0] + uint64_t{y} * f.stride[0] + w);
  for (uint32_t y = 0; y < (h + 1) / 2; ++y)
    out.insert(out.end(), f.plane[1] + uint64_t{y} * f.stride[1], f.plane[1] + uint64_t{y} * f.stride[1] + 2 * ((w + 1) / 2));
  return out;
}

// The NV12 picture (`w` x `h`) cropped to I420, as ffmpeg's framemd5 hashes yuv420p.
std::vector<uint8_t> crop_to_i420(const std::vector<uint8_t>& nv12, uint32_t w, uint32_t h, const Stream& st) {
  std::vector<uint8_t> out;
  const uint32_t cw = w - st.crop_l - st.crop_r, ch = h - st.crop_t - st.crop_b;
  for (uint32_t y = 0; y < ch; ++y)
    for (uint32_t x = 0; x < cw; ++x) out.push_back(nv12[uint64_t{y + st.crop_t} * w + x + st.crop_l]);
  for (int c = 0; c < 2; ++c)
    for (uint32_t y = 0; y < ch / 2; ++y)
      for (uint32_t x = 0; x < cw / 2; ++x) out.push_back(nv12[uint64_t{w} * h + uint64_t{y + st.crop_t / 2} * w + 2 * (x + st.crop_l / 2) + c]);
  return out;
}

std::vector<uint8_t> frame_i420(const VideoFrame& f) {
  std::vector<uint8_t> out;
  for (uint32_t p = 0; p < 3; ++p) {
    const uint32_t w = p ? (f.format.width + 1) / 2 : f.format.width, h = p ? (f.format.height + 1) / 2 : f.format.height;
    for (uint32_t y = 0; y < h; ++y) out.insert(out.end(), f.plane[p] + uint64_t{y} * f.stride[p], f.plane[p] + uint64_t{y} * f.stride[p] + w);
  }
  return out;
}

std::string md5_hex(const std::vector<uint8_t>& bytes) {
  crypto::Hasher h(crypto::HashKind::Md5);
  h.update(ConstByteView(bytes.data(), bytes.size()));
  uint8_t digest[16];
  h.finish(digest);
  char hex[33];
  for (int i = 0; i < 16; ++i) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return std::string(hex, 32);
}

std::vector<std::string> reference_md5(const std::string& file) {
  std::vector<std::string> md5;
  const std::vector<uint8_t> text = read_file("tests/data/dxva_h264_md5.txt");
  const std::string all(text.begin(), text.end());
  size_t pos = 0;
  while (pos < all.size()) {
    size_t e = all.find('\n', pos);
    if (e == std::string::npos) e = all.size();
    const std::string line = all.substr(pos, e - pos);
    const size_t sp = line.find(' ');
    if (sp != std::string::npos && line.substr(0, sp) == file) md5.push_back(line.substr(sp + 1));
    pos = e + 1;
  }
  return md5;
}

DxvaPicture picture_of(const Call& c) {
  DxvaPicture p;
  p.picture_parameters = ConstByteView(c.params.data(), c.params.size());
  p.quantization_matrix = ConstByteView(c.qm.data(), c.qm.size());
  p.slice_control = ConstByteView(c.slices.data(), c.slices.size());
  p.bitstream = ConstByteView(c.bitstream.data(), c.bitstream.size());
  return p;
}

UniquePointer<IDxvaDecoder> make_decoder(TestAllocator& alloc, const Stream& st, uint32_t surfaces = kPool) {
  DxvaDecoderConfig config;
  config.profile = DxvaProfile::H264VldNoFgt;
  config.width = st.mb_w * 16;
  config.height = st.mb_h * 16;
  config.surfaces = surfaces;
  config.output = PixelFormat::Nv12;
  return create_dxva_decoder(alloc, config);
}

// How the indices are mapped to keys in a run.
enum class KeyMode { Plain, Permuted, OutputKey };

// Rewrites the surface indices of `c` for a run whose index -> key mapping is `keys` (a permutation of the stable keys 1000 + 7 * surface).
Call remap(const Call& c, const std::vector<uint32_t>& keys, bool output_key_index) {
  Call r = c;
  auto index_of = [&](uint32_t surface) {
    for (uint32_t i = 0; i < keys.size(); ++i)
      if (keys[i] == 1000 + 7 * surface) return i;
    return 0u;
  };
  r.params[4] = static_cast<uint8_t>(output_key_index ? 120 : index_of(c.params[4] & 0x7F));
  for (int i = 0; i < 16; ++i) {
    const uint8_t e = c.params[16 + i];
    if (e != 0xFF) r.params[16 + i] = static_cast<uint8_t>((e & 0x80) | index_of(e & 0x7F));
  }
  return r;
}

struct RunResult {
  std::vector<std::vector<uint8_t>> pictures;   // NV12, decoding order
  uint32_t width = 0, height = 0;
  int ok = 0;
};

RunResult run(TestAllocator& alloc, const Stream& st, KeyMode mode, uint32_t seed) {
  RunResult res;
  UniquePointer<IDxvaDecoder> dec = make_decoder(alloc, st);
  CHECK(dec.get() != nullptr);
  if (!dec) return res;
  std::mt19937 rng(seed);
  std::vector<uint32_t> keys(kPool);
  for (uint32_t i = 0; i < kPool; ++i) keys[i] = 1000 + 7 * i;
  for (const Call& c : st.calls) {
    Call use = c;
    DxvaPicture p;
    std::vector<uint32_t> list;
    if (mode != KeyMode::Plain) {
      std::shuffle(keys.begin(), keys.end(), rng);
      list = keys;
      if (mode == KeyMode::OutputKey) {
        // Direct3D 12: the output is named apart; its index (120) is not in the list, and its key is not either unless a reference needs it.
        const uint32_t out_key = 1000 + 7 * c.surface;
        bool referenced = false;
        for (int i = 0; i < 16; ++i)
          if (c.params[16 + i] != 0xFF && (c.params[16 + i] & 0x7F) == c.surface) referenced = true;
        if (!referenced) list.erase(std::find(list.begin(), list.end(), out_key));
        use = remap(c, list, true);
        p = picture_of(use);
        p.output_key = out_key;
      } else {
        use = remap(c, list, false);
        p = picture_of(use);
      }
      p.surface_keys = ArrayView<const uint32_t>(list.data(), list.size());
    } else {
      p = picture_of(use);
    }
    DxvaDecodedPicture out;
    const DxvaResult r = dec->decode(p, &out);
    CHECK(r == DxvaResult::Ok);
    if (r != DxvaResult::Ok) {
      printf("  picture %zu: result %d (%s)\n", res.pictures.size(), static_cast<int>(r), out.detail ? out.detail : "");
      res.pictures.push_back({});
      continue;
    }
    CHECK(out.surface == use.params[4]);
    CHECK(out.frame.format.pixel_format == PixelFormat::Nv12 && out.frame.format.width == st.mb_w * 16 && out.frame.format.height == st.mb_h * 16);
    res.width = out.frame.format.width;
    res.height = out.frame.format.height;
    res.pictures.push_back(copy_nv12(out.frame));
    ++res.ok;
  }
  return res;
}

// The deterministic refusals, sent before picture `c` of a decoder that then gets the real call: none may change what the decoder keeps.
void refusals(IDxvaDecoder& dec, const Call& c) {
  DxvaDecodedPicture out;
  auto expect = [&](const DxvaPicture& p, DxvaResult want) {
    const DxvaResult r = dec.decode(p, &out);
    CHECK(r == want);
    if (r != want) printf("  got %d (%s)\n", static_cast<int>(r), out.detail ? out.detail : "");
    if (r == DxvaResult::Invalid || r == DxvaResult::Unsupported) CHECK(out.detail != nullptr);
  };
  DxvaPicture p = picture_of(c);
  {
    DxvaPicture q = p;
    q.picture_parameters = ConstByteView();
    expect(q, DxvaResult::Invalid);
    q = p;
    q.slice_control = ConstByteView();
    expect(q, DxvaResult::Invalid);
    q = p;
    q.bitstream = ConstByteView();
    expect(q, DxvaResult::Invalid);
    q = p;
    q.picture_parameters = p.picture_parameters.first(p.picture_parameters.size() - 1);
    expect(q, DxvaResult::Invalid);
    q = p;
    q.quantization_matrix = p.quantization_matrix.first(223);
    expect(q, DxvaResult::Invalid);
    q = p;
    q.slice_control = p.slice_control.first(p.slice_control.size() - 1);
    expect(q, DxvaResult::Invalid);
    q = p;
    q.bitstream = p.bitstream.first(p.bitstream.size() - 1);
    expect(q, DxvaResult::Invalid);
    // An index list too short for the indices used.
    const uint32_t one_key = 5;
    q = p;
    q.surface_keys = ArrayView<const uint32_t>(&one_key, 1);
    if ((c.params[4] & 0x7F) != 0) expect(q, DxvaResult::Invalid);
  }
  auto with_params = [&](auto&& change, DxvaResult want) {
    Call d = c;
    PicParamsH264 pp;
    memcpy(&pp, d.params.data(), sizeof pp);
    change(pp);
    d.params = bytes_of(pp);
    expect(picture_of(d), want);
  };
  with_params([](PicParamsH264& pp) { pp.CurrPic = static_cast<uint8_t>(kPool + 3); }, DxvaResult::Invalid);
  with_params([](PicParamsH264& pp) { pp.CurrPic = 0xFF; }, DxvaResult::Invalid);
  with_params([](PicParamsH264& pp) { pp.RefFrameList[15] = 100; pp.UsedForReferenceFlags |= 3u << 30; }, DxvaResult::Invalid);
  with_params([](PicParamsH264& pp) { pp.wBitFields |= dxva::kH264FieldPic; }, DxvaResult::Unsupported);
  with_params([](PicParamsH264& pp) { pp.wBitFields |= dxva::kH264MbaffFrame; }, DxvaResult::Unsupported);
  with_params([](PicParamsH264& pp) { pp.wBitFields &= static_cast<uint16_t>(~(3u << dxva::kH264ChromaFormatShift)); }, DxvaResult::Unsupported);
  with_params([](PicParamsH264& pp) { pp.wBitFields |= static_cast<uint16_t>(2u << dxva::kH264ChromaFormatShift); }, DxvaResult::Invalid);
  with_params([](PicParamsH264& pp) { pp.bit_depth_luma_minus8 = 2; }, DxvaResult::Invalid);
  with_params([](PicParamsH264& pp) { pp.num_slice_groups_minus1 = 1; }, DxvaResult::Unsupported);
  with_params([](PicParamsH264& pp) { pp.ContinuationFlag = 0; }, DxvaResult::Invalid);
  with_params([](PicParamsH264& pp) { pp.wFrameWidthInMbsMinus1 = static_cast<uint16_t>(pp.wFrameWidthInMbsMinus1 + 1); }, DxvaResult::Invalid);
  with_params([](PicParamsH264& pp) { pp.num_ref_frames = 17; }, DxvaResult::Invalid);
  {
    Call d = c;
    SliceH264Short sc;
    memcpy(&sc, d.slices.data(), sizeof sc);
    sc.wBadSliceChopping = 1;
    memcpy(d.slices.data(), &sc, sizeof sc);
    expect(picture_of(d), DxvaResult::Unsupported);
    memcpy(&sc, c.slices.data(), sizeof sc);
    sc.BSNALunitDataLocation = 0xFFFFFFF0u;
    memcpy(d.slices.data(), &sc, sizeof sc);
    expect(picture_of(d), DxvaResult::Invalid);
    d = c;
    d.bitstream[2] = 2;   // no start code
    expect(picture_of(d), DxvaResult::Invalid);
    d = c;
    d.qm[5] = 0;
    expect(picture_of(d), DxvaResult::Invalid);
  }
}

// Bits flipped and buffers cut short: the decoder must not fault; whatever it says, a picture it gives is whole.
void fuzz(TestAllocator& alloc, const Stream& st, uint32_t seed) {
  UniquePointer<IDxvaDecoder> dec = make_decoder(alloc, st);
  if (!dec) return;
  std::mt19937 rng(seed);
  int counts[4] = {};
  for (const Call& c : st.calls) {
    for (int variant = 0; variant < 6; ++variant) {
      Call d = c;
      std::vector<uint8_t>* bufs[4] = {&d.params, &d.qm, &d.slices, &d.bitstream};
      std::vector<uint8_t>& b = *bufs[rng() % 4];
      if (b.empty()) continue;
      if (variant % 2 == 0) {
        b.resize(rng() % b.size());
      } else {
        const int flips = 1 + static_cast<int>(rng() % 8);
        for (int k = 0; k < flips; ++k) b[rng() % b.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
      }
      DxvaDecodedPicture out;
      const DxvaResult r = dec->decode(picture_of(d), &out);
      ++counts[static_cast<int>(r)];
      if (r == DxvaResult::Ok || r == DxvaResult::MissingReference) {
        CHECK(out.frame.plane[0] != nullptr && out.frame.plane[1] != nullptr && out.frame.format.width <= st.mb_w * 16 + 16);
        // Touch every sample (the sanitizer sees a picture that is not whole).
        uint64_t sum = 0;
        for (uint8_t v : copy_nv12(out.frame)) sum += v;
        CHECK(sum > 0);
      } else {
        CHECK(out.detail != nullptr);
      }
    }
    DxvaDecodedPicture out;
    const DxvaResult r = dec->decode(picture_of(c), &out);
    CHECK(r == DxvaResult::Ok || r == DxvaResult::MissingReference);
  }
  printf("  fuzzed: %d ok, %d invalid, %d unsupported, %d missing reference\n", counts[0], counts[1], counts[2], counts[3]);
}

void test_stream(const char* name, bool damage) {
  TestAllocator alloc;
  {
    const std::string file = std::string(name) + ".h264";
    const std::vector<uint8_t> data = read_file("tests/data/" + file);
    CHECK(!data.empty());
    if (data.empty()) return;
    Stream st;
    CHECK(record(alloc, data, &st));
    const std::vector<std::string> ref = reference_md5(file);
    CHECK(!ref.empty());
    // This project's CPU decoder, in display order: the picture each output came from is its pts (the decoding index).
    std::vector<std::pair<int64_t, std::vector<uint8_t>>> shown;
    {
      H264Decoder sw(alloc);
      VideoFrame f;
      for (size_t i = 0; i < st.access_units.size(); ++i) {
        const auto& au = st.access_units[i];
        (void)sw.decode(ConstByteView(data.data() + au.first, au.second - au.first), static_cast<int64_t>(i));
        while (sw.receive(&f)) shown.push_back({f.pts_us, frame_i420(f)});
      }
      sw.flush();
      while (sw.receive(&f)) shown.push_back({f.pts_us, frame_i420(f)});
    }
    CHECK(shown.size() == ref.size());
    int sw_exact = 0;
    for (size_t i = 0; i < shown.size() && i < ref.size(); ++i) sw_exact += md5_hex(shown[i].second) == ref[i] ? 1 : 0;
    CHECK(sw_exact == static_cast<int>(ref.size()));

    const KeyMode modes[] = {KeyMode::Plain, KeyMode::Permuted, KeyMode::OutputKey};
    RunResult plain;
    for (KeyMode mode : modes) {
      RunResult r = run(alloc, st, mode, 7 + static_cast<uint32_t>(mode));
      CHECK(r.pictures.size() == st.calls.size());
      if (mode == KeyMode::Plain) {
        // Every picture against ffmpeg (through the display order) and against the CPU decoder's output.
        int exact = 0;
        for (size_t i = 0; i < shown.size() && i < ref.size(); ++i) {
          const int64_t k = shown[i].first;
          if (k < 0 || static_cast<size_t>(k) >= r.pictures.size() || r.pictures[k].empty()) continue;
          const std::vector<uint8_t> cropped = crop_to_i420(r.pictures[k], r.width, r.height, st);
          const bool same = md5_hex(cropped) == ref[i] && cropped == shown[i].second;
          if (!same) printf("  %s: display picture %zu (decoded %lld) differs\n", name, i, static_cast<long long>(k));
          exact += same ? 1 : 0;
        }
        CHECK(exact == static_cast<int>(ref.size()));
        printf("  %s: %zu pictures decoded, %d of %zu shown bit exact with ffmpeg\n", name, r.pictures.size(), exact, ref.size());
        plain = std::move(r);
      } else {
        CHECK(r.pictures == plain.pictures);   // the same pictures whatever the index -> key mapping
      }
    }
    if (damage) {
      // Refusals before every picture leave the decoding unchanged.
      UniquePointer<IDxvaDecoder> dec = make_decoder(alloc, st);
      std::vector<std::vector<uint8_t>> got;
      for (const Call& c : st.calls) {
        refusals(*dec, c);
        DxvaDecodedPicture out;
        CHECK(dec->decode(picture_of(c), &out) == DxvaResult::Ok);
        got.push_back(out.frame.plane[0] ? copy_nv12(out.frame) : std::vector<uint8_t>());
      }
      CHECK(got == plain.pictures);
      // After reset() the references are gone: a picture that has some decodes with mid-grey for them and says so.
      dec->reset();
      for (const Call& c : st.calls) {
        if (c.intra) continue;
        DxvaDecodedPicture out;
        CHECK(dec->decode(picture_of(c), &out) == DxvaResult::MissingReference);
        CHECK(out.frame.plane[0] != nullptr && out.detail != nullptr);
        break;
      }
      // A reference whose key is the output's own is the picture that was there before: decoding picture k into the surface of one of its references.
      for (size_t k = 0; k < st.calls.size(); ++k) {
        const Call& c = st.calls[k];
        if (c.intra || c.params[16] == 0xFF) continue;
        UniquePointer<IDxvaDecoder> d2 = make_decoder(alloc, st);
        for (size_t j = 0; j < k; ++j) {
          DxvaDecodedPicture o;
          (void)d2->decode(picture_of(st.calls[j]), &o);
        }
        Call same = c;
        same.params[4] = static_cast<uint8_t>(c.params[16] & 0x7F);   // into the surface of its first reference
        DxvaDecodedPicture o;
        CHECK(d2->decode(picture_of(same), &o) == DxvaResult::Ok);
        CHECK(o.frame.plane[0] && copy_nv12(o.frame) == plain.pictures[k]);
        break;
      }
      // The order counts are the program's: the same B picture with two references' counts swapped is predicted differently (list order, weights, direct).
      for (size_t k = 0; k < st.calls.size(); ++k) {
        const Call& c = st.calls[k];
        PicParamsH264 pp;
        memcpy(&pp, c.params.data(), sizeof pp);
        if (c.intra || pp.RefFrameList[1] == 0xFF || pp.CurrFieldOrderCnt[0] <= pp.FieldOrderCntList[0][0]) continue;
        bool after = false;   // a B picture has references on both sides
        for (int i = 0; i < 16; ++i) after = after || (pp.RefFrameList[i] != 0xFF && pp.FieldOrderCntList[i][0] > pp.CurrFieldOrderCnt[0]);
        if (!after) continue;
        UniquePointer<IDxvaDecoder> d2 = make_decoder(alloc, st);
        for (size_t j = 0; j < k; ++j) {
          DxvaDecodedPicture o;
          (void)d2->decode(picture_of(st.calls[j]), &o);
        }
        for (int e = 0; e < 2; ++e) {
          const int32_t t = pp.FieldOrderCntList[0][e];
          pp.FieldOrderCntList[0][e] = pp.FieldOrderCntList[1][e];
          pp.FieldOrderCntList[1][e] = t;
        }
        Call swapped = c;
        swapped.params = bytes_of(pp);
        DxvaDecodedPicture o;
        CHECK(d2->decode(picture_of(swapped), &o) == DxvaResult::Ok);
        CHECK(o.frame.plane[0] && copy_nv12(o.frame) != plain.pictures[k]);
        printf("  %s: order counts of the program used (picture %zu)\n", name, k);
        break;
      }
      fuzz(alloc, st, 99);
    }
  }
  CHECK(alloc.live == 0);
}

}  // namespace dxva_h264_test

static void dxva_test_h264() {
  using namespace dxva_h264_test;
  struct Case {
    const char* name;
    bool damage;
  };
  const Case cases[] = {{"h264_bframes", true}, {"h264_cavlc", true}, {"h264_cqm", true}, {"h264_intra", false}, {"h264_ltr", true}, {"h264_noise", false},
                        {"h264_pcm_cabac", false}, {"h264_pcm_cavlc", false}, {"h264_dxva_poc2", true}, {"h264_dxva_poc1", true}, {"h264_dxva_pyramid", true}};
  for (const Case& c : cases) test_stream(c.name, c.damage);
  // The profile takes NV12 only.
  TestAllocator alloc;
  DxvaDecoderConfig config;
  config.profile = DxvaProfile::H264VldNoFgt;
  config.width = 64;
  config.height = 64;
  config.surfaces = 4;
  config.output = PixelFormat::P010;
  CHECK(!create_dxva_decoder(alloc, config));
}
