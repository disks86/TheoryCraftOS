// DxvaDecoderTest: the VP9 cases (D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0 -> NV12, _VP9_VLD_10BIT_PROFILE2 -> P010).
//
// The test is the program: it splits the IVF packets' superframes into frames, shows show_existing_frame frames itself (from the surface the slot names) and sends
// every other frame to the decoder with DXVA_PicParams_VP9 filled as ffmpeg's libavcodec/dxva2_vp9.c fills it, from what the kernel's own VP9 parser says of the frame
// (the software decoder, run beside it with a frame observer). Surfaces come from a pool as ffmpeg's do: a frame goes into a surface no reference slot holds. Every
// displayed picture must be what ffmpeg's VP9 decoder made of the stream (tests/data/manifest_dxva_vp9.txt, made by tests/data/make_dxva_vp9_fixtures.py), and every
// decoded picture, hidden ones included, the software decoder's, bit for bit.
#include "Hardware/VideoCodec/Dxva/DxvaTypesVp9.hpp"
#include "Hardware/VideoCodec/Software/Vp9.hpp"
#include "Hardware/VideoCodec/Software/Vp9Core.hpp"

namespace dxva_vp9_test {

using hardware::dxva::PicParamsVp9;
using hardware::dxva::SliceVpxShort;

uint64_t fnv64(uint64_t h, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

// The frames of an IVF file, superframes split, in decoding order.
std::vector<std::vector<uint8_t>> ivf_frames(const std::vector<uint8_t>& d) {
  std::vector<std::vector<uint8_t>> out;
  size_t pos = 32;
  while (pos + 12 <= d.size()) {
    const size_t size = d[pos] | d[pos + 1] << 8 | d[pos + 2] << 16 | static_cast<size_t>(d[pos + 3]) << 24;
    pos += 12;
    if (size > d.size() - pos) break;
    const uint8_t* pk = d.data() + pos;
    pos += size;
    const uint8_t marker = size ? pk[size - 1] : 0;
    if ((marker & 0xe0) == 0xc0) {
      const size_t frames = (marker & 7) + 1, mag = ((marker >> 3) & 3) + 1, index = 2 + mag * frames;
      if (size >= index && pk[size - index] == marker) {
        size_t p = size - index + 1, offset = 0;
        for (size_t i = 0; i < frames; ++i) {
          size_t sz = 0;
          for (size_t b = 0; b < mag; ++b) sz |= static_cast<size_t>(pk[p++]) << (8 * b);
          if (sz > size - index - offset) break;
          out.emplace_back(pk + offset, pk + offset + sz);
          offset += sz;
        }
        continue;
      }
    }
    out.emplace_back(pk, pk + size);
  }
  return out;
}

// What the software decoder says of each frame it decodes, and the picture (16-bit samples, tight rows), in decoding order.
struct Decoded {
  vp9::OffloadFrame header;
  std::vector<uint16_t> planes[3];
};
struct Observer final : vp9::IFrameObserver {
  std::vector<Decoded> frames;
  void frame_decoded(const vp9::OffloadFrame& f) override {
    Decoded d;
    d.header = f;
    d.header.data = ConstByteView();
    for (int p = 0; p < 3; ++p) {
      d.header.plane[p] = ArrayView<uint16_t>();
      for (int y = 0; y < f.plane_height[p]; ++y)
        for (int x = 0; x < f.plane_width[p]; ++x) d.planes[p].push_back(f.plane[p][static_cast<uint64_t>(y) * f.stride[p] + static_cast<uint64_t>(x)]);
    }
    frames.push_back(static_cast<Decoded&&>(d));
  }
};

bool show_existing(const std::vector<uint8_t>& f, int* slot) {
  if (f.empty()) return false;
  const int profile = (f[0] >> 5 & 1) | (f[0] >> 4 & 1) << 1;
  const int bit = profile == 3 ? 5 : 4;   // frame_marker (2), profile (2), reserved (1, profile 3)
  if (!(f[0] >> (7 - bit) & 1)) return false;
  // frame_to_show_map_idx: the three bits after it (possibly into the next byte)
  const uint32_t word = static_cast<uint32_t>(f[0]) << 8 | (f.size() > 1 ? f[1] : 0);
  *slot = static_cast<int>(word >> (15 - bit - 3) & 7);
  return true;
}

// DXVA_PicParams_VP9 as dxva2_vp9.c's fill_picture_parameters makes it. `slots` are the surfaces of the eight reference slots before the frame (-1: none),
// `sizes` their pictures' sizes.
PicParamsVp9 fill_params(const vp9::OffloadFrame& h, int surface, const int slots[8], const int sizes[8][2], const int index_of[], bool ten_bit) {
  PicParamsVp9 pp;
  memset(&pp, 0, sizeof pp);
  auto entry = [&](int s) { return hardware::dxva::PicEntryVpx{static_cast<uint8_t>(s < 0 ? 0xFF : index_of[s])}; };
  pp.CurrPic = entry(surface);
  pp.profile = static_cast<uint8_t>(h.info.profile);
  pp.wFormatAndPictureInfoFlags = static_cast<uint16_t>((!h.info.key_frame) << 0 | h.info.show_frame << 1 | h.error_resilient << 2 | 1 << 3 | 1 << 4 |
                                                        h.refresh_frame_context << 6 | h.frame_parallel << 7 | h.info.intra_only << 8 | h.frame_context_idx << 9 |
                                                        h.reset_frame_context << 11 | (h.info.key_frame ? 0 : h.hp_mv) << 13);
  pp.width = static_cast<uint32_t>(h.info.width);
  pp.height = static_cast<uint32_t>(h.info.height);
  pp.BitDepthMinus8Luma = pp.BitDepthMinus8Chroma = ten_bit ? 2 : 0;
  pp.interp_filter = static_cast<uint8_t>(h.interp_filter);   // (libvpx's numbering, which DXVA uses)
  for (int i = 0; i < 8; ++i) {
    pp.ref_frame_map[i] = entry(slots[i]);
    if (slots[i] >= 0) {
      pp.ref_frame_coded_width[i] = static_cast<uint32_t>(sizes[i][0]);
      pp.ref_frame_coded_height[i] = static_cast<uint32_t>(sizes[i][1]);
    }
  }
  for (int i = 0; i < 3; ++i) {
    pp.frame_refs[i] = entry(slots[h.ref_slot[i]]);
    pp.ref_frame_sign_bias[i + 1] = static_cast<int8_t>(h.sign_bias[i]);
  }
  pp.filter_level = static_cast<int8_t>(h.filter_level);
  pp.sharpness_level = static_cast<int8_t>(h.sharpness_level);
  pp.wControlInfoFlags = static_cast<uint8_t>(h.mode_ref_delta_enabled << 0 | h.mode_ref_delta_update << 1 | h.use_prev_frame_mvs << 2);
  pp.base_qindex = static_cast<int16_t>(h.base_qindex);
  pp.y_dc_delta_q = static_cast<int8_t>(h.y_dc_delta_q);
  pp.uv_dc_delta_q = static_cast<int8_t>(h.uv_dc_delta_q);
  pp.uv_ac_delta_q = static_cast<int8_t>(h.uv_ac_delta_q);
  pp.stVP9Segments.wSegmentInfoFlags = static_cast<uint8_t>(h.seg.enabled << 0 | h.seg.update_map << 1 | h.seg.temporal_update << 2 | h.seg.abs_delta << 3);
  for (int i = 0; i < 7; ++i) pp.stVP9Segments.tree_probs[i] = h.seg.tree_probs[i];
  for (int i = 0; i < 3; ++i) pp.stVP9Segments.pred_probs[i] = h.seg.temporal_update ? h.seg.pred_probs[i] : 255;
  for (int i = 0; i < 8; ++i) {
    pp.stVP9Segments.feature_mask[i] = static_cast<uint8_t>(h.seg.feature_mask[i] & 15);
    for (int j = 0; j < 3; ++j) pp.stVP9Segments.feature_data[i][j] = h.seg.feature_data[i][j];
  }
  pp.log2_tile_cols = static_cast<uint8_t>(h.log2_tile_cols);
  pp.log2_tile_rows = static_cast<uint8_t>(h.log2_tile_rows);
  pp.uncompressed_header_size_byte_aligned = static_cast<uint16_t>(h.uncompressed_header_bytes);
  pp.first_partition_size = static_cast<uint16_t>(h.compressed_header_bytes);
  return pp;
}

// The native planar picture (what ffmpeg's rawvideo holds) of an NV12 or P010 picture, as the fnv64 of its bytes.
uint64_t hash_native(const VideoFrame& f) {
  const bool p010 = f.format.pixel_format == PixelFormat::P010;
  const uint32_t w = f.format.width, h = f.format.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
  std::vector<uint8_t> bytes;
  auto sample = [&](const uint8_t* row, uint32_t x) {
    if (p010) {
      const uint32_t v = (row[2 * x] | row[2 * x + 1] << 8) >> 6;
      bytes.push_back(static_cast<uint8_t>(v));
      bytes.push_back(static_cast<uint8_t>(v >> 8));
    } else {
      bytes.push_back(row[x]);
    }
  };
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) sample(f.plane[0] + static_cast<size_t>(y) * f.stride[0], x);
  for (int c = 0; c < 2; ++c)
    for (uint32_t y = 0; y < ch; ++y)
      for (uint32_t x = 0; x < cw; ++x) sample(f.plane[1] + static_cast<size_t>(y) * f.stride[1], 2 * x + c);
  return fnv64(0xcbf29ce484222325ull, bytes.data(), bytes.size());
}

// Whether the DXVA picture holds exactly the software decoder's samples.
bool same_picture(const VideoFrame& f, const Decoded& d) {
  const bool p010 = f.format.pixel_format == PixelFormat::P010;
  const uint32_t w = f.format.width, h = f.format.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
  if (static_cast<int>(w) != d.header.plane_width[0] || static_cast<int>(h) != d.header.plane_height[0]) return false;
  auto at = [&](const uint8_t* row, uint32_t x) { return p010 ? static_cast<uint32_t>((row[2 * x] | row[2 * x + 1] << 8) >> 6) : row[x]; };
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x)
      if (at(f.plane[0] + static_cast<size_t>(y) * f.stride[0], x) != d.planes[0][static_cast<size_t>(y) * w + x]) return false;
  for (int c = 0; c < 2; ++c)
    for (uint32_t y = 0; y < ch; ++y)
      for (uint32_t x = 0; x < cw; ++x)
        if (at(f.plane[1] + static_cast<size_t>(y) * f.stride[1], 2 * x + c) != d.planes[1 + c][static_cast<size_t>(y) * cw + x]) return false;
  return true;
}

struct Expected {
  uint32_t width = 0, height = 0;
  uint64_t hash = 0;
};

std::vector<Expected> manifest(const std::string& file) {
  std::vector<Expected> out;
  FILE* m = fopen("tests/data/manifest_dxva_vp9.txt", "r");
  if (!m) return out;
  char name[96];
  unsigned n, w, h;
  unsigned long long hash;
  while (fscanf(m, "%95s %u %u %u %llx", name, &n, &w, &h, &hash) == 5)
    if (file == name) out.push_back({w, h, hash});
  fclose(m);
  return out;
}

struct Stats {
  int streams = 0, decoded = 0, hidden = 0, displayed = 0, exact = 0, matched_reference = 0;
};
Stats g_stats;

enum class Mode { Plain, Keys, OutputKey, Damage };

// Plays a stream through a DXVA decoder. Keys: the surfaces are named through surface_keys, by indices that change from picture to picture. Damage: before each frame
// the program sends broken versions of it (which must be refused without touching the decoder), then the frame itself.
void play(const char* file, bool ten_bit, Mode mode) {
  TestAllocator a;
  const std::vector<uint8_t> data = read_file(std::string("tests/data/") + file);
  CHECK(!data.empty());
  if (data.empty()) return;
  const std::vector<std::vector<uint8_t>> frames = ivf_frames(data);
  const std::vector<Expected> expected = manifest(file);
  CHECK(!expected.empty());
  // The software decoder parses (and decodes) the stream first.
  Observer obs;
  {
    Vp9Decoder sw(a);
    sw.set_frame_observer(&obs);
    for (const std::vector<uint8_t>& f : frames) CHECK(sw.decode(ConstByteView(f.data(), f.size()), 0));
  }
  uint32_t max_w = 0, max_h = 0;
  for (const Expected& e : expected) {
    if (e.width > max_w) max_w = e.width;
    if (e.height > max_h) max_h = e.height;
  }
  constexpr int kSurfaces = 10;
  DxvaDecoderConfig config;
  config.profile = ten_bit ? DxvaProfile::Vp9Vld10BitProfile2 : DxvaProfile::Vp9VldProfile0;
  config.output = ten_bit ? PixelFormat::P010 : PixelFormat::Nv12;
  config.width = max_w;
  config.height = max_h;
  config.surfaces = kSurfaces;
  {
    UniquePointer<IDxvaDecoder> dec = create_dxva_decoder(a, config);
    CHECK(dec != nullptr);
    if (!dec) return;
    CHECK(dec->profile() == config.profile);
    int slots[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int sizes[8][2] = {};
    std::vector<uint64_t> surface_hash(kSurfaces, 0);
    std::vector<uint32_t> surface_w(kSurfaces, 0), surface_h(kSurfaces, 0);
    size_t decoded = 0, displayed = 0;
    uint32_t seed = 12345;
    auto rnd = [&seed]() {
      seed = seed * 1103515245u + 12345u;
      return seed >> 8;
    };
    for (const std::vector<uint8_t>& f : frames) {
      int slot = 0;
      if (show_existing(f, &slot)) {
        // The program shows the surface itself.
        CHECK(slots[slot] >= 0);
        if (slots[slot] >= 0 && displayed < expected.size()) {
          CHECK(surface_hash[slots[slot]] == expected[displayed].hash);
          if (surface_hash[slots[slot]] == expected[displayed].hash) ++g_stats.matched_reference;
        }
        ++displayed;
        continue;
      }
      CHECK(decoded < obs.frames.size());
      if (decoded >= obs.frames.size()) break;
      const Decoded& ref = obs.frames[decoded];
      // A surface no slot holds.
      int surface = -1;
      for (int s = 0; s < kSurfaces && surface < 0; ++s) {
        bool busy = false;
        for (int r : slots) busy |= r == s;
        if (!busy) surface = s;
      }
      CHECK(surface >= 0);
      // The indices the parameters use: the surface itself, or (Keys) a position in a list that is shuffled for every picture, with the surface's key there.
      int index_of[kSurfaces];
      uint32_t keys[kSurfaces];
      for (int s = 0; s < kSurfaces; ++s) index_of[s] = s;
      if (mode == Mode::Keys || mode == Mode::OutputKey) {
        for (int s = kSurfaces - 1; s > 0; --s) {
          const int t = static_cast<int>(rnd() % static_cast<uint32_t>(s + 1));
          const int tmp = index_of[s];
          index_of[s] = index_of[t];
          index_of[t] = tmp;
        }
        for (int s = 0; s < kSurfaces; ++s) keys[index_of[s]] = 0x51000000u + 7919u * static_cast<uint32_t>(s);
      }
      // OutputKey: as Direct3D 12 names it, the output surface is apart from the reference list - its key is output_key and CurrPic's index is past the list.
      const int list_size = mode == Mode::OutputKey ? kSurfaces - 1 : kSurfaces;
      int output_index = 0;
      if (mode == Mode::OutputKey) {
        output_index = index_of[surface];
        index_of[surface] = 100 + static_cast<int>(rnd() % 27);   // (CurrPic only: no reference names the current surface)
        for (int s = 0; s < kSurfaces; ++s)
          if (index_of[s] == kSurfaces - 1) index_of[s] = output_index;   // the last position goes to the surface that had the output's
        for (int s = 0; s < kSurfaces; ++s)
          if (s != surface) keys[index_of[s]] = 0x51000000u + 7919u * static_cast<uint32_t>(s);
      }
      PicParamsVp9 pp = fill_params(ref.header, surface, slots, sizes, index_of, ten_bit);
      // The frame with ffmpeg's zero padding to a multiple of 128 bytes, which the slice control counts.
      std::vector<uint8_t> bits = f;
      bits.resize((bits.size() + 127) / 128 * 128 + (bits.size() % 128 == 0 ? 128 : 0), 0);
      SliceVpxShort slice = {0, static_cast<uint32_t>(bits.size()), 0};
      DxvaPicture pic;
      pic.picture_parameters = ConstByteView(reinterpret_cast<const uint8_t*>(&pp), sizeof pp);
      pic.slice_control = ConstByteView(reinterpret_cast<const uint8_t*>(&slice), sizeof slice);
      pic.bitstream = ConstByteView(bits.data(), bits.size());
      if (mode == Mode::Keys) pic.surface_keys = ArrayView<const uint32_t>(keys, kSurfaces);
      if (mode == Mode::OutputKey) {
        pic.surface_keys = ArrayView<const uint32_t>(keys, static_cast<uint64_t>(list_size));
        pic.output_key = 0x51000000u + 7919u * static_cast<uint32_t>(surface);
      }
      DxvaDecodedPicture out;
      if (mode == Mode::Damage) {
        // Refused before anything changes.
        auto refused = [&](const DxvaPicture& p) {
          DxvaDecodedPicture o;
          const DxvaResult r = dec->decode(p, &o);
          CHECK(r == DxvaResult::Invalid || r == DxvaResult::Unsupported);
          CHECK(o.detail != nullptr);
        };
        DxvaPicture p = pic;
        p.picture_parameters = pic.picture_parameters.first(sizeof pp - 1);
        refused(p);
        p = pic;
        p.picture_parameters = ConstByteView();
        refused(p);
        p = pic;
        p.slice_control = pic.slice_control.first(sizeof slice - 1);
        refused(p);
        p = pic;
        p.bitstream = pic.bitstream.first(pic.bitstream.size() - 1);
        refused(p);
        p = pic;
        p.bitstream = ConstByteView();
        refused(p);
        SliceVpxShort bad = slice;
        bad.BSNALunitDataLocation = 0xFFFFFFF0u;
        p = pic;
        p.slice_control = ConstByteView(reinterpret_cast<const uint8_t*>(&bad), sizeof bad);
        refused(p);
        bad = slice;
        bad.wBadSliceChopping = 1;
        p.slice_control = ConstByteView(reinterpret_cast<const uint8_t*>(&bad), sizeof bad);
        refused(p);
        PicParamsVp9 bpp = pp;
        p = pic;
        p.picture_parameters = ConstByteView(reinterpret_cast<const uint8_t*>(&bpp), sizeof bpp);
        bpp.CurrPic.bPicEntry = kSurfaces;   // past the surfaces
        refused(p);
        bpp = pp;
        bpp.CurrPic.bPicEntry = 0xFF;
        refused(p);
        bpp = pp;
        bpp.ref_frame_map[rnd() % 8].bPicEntry = 100;
        refused(p);
        bpp = pp;
        bpp.profile = 1;
        refused(p);
        bpp = pp;
        bpp.BitDepthMinus8Luma = ten_bit ? 0 : 2;
        refused(p);
        bpp = pp;
        bpp.wFormatAndPictureInfoFlags = static_cast<uint16_t>(bpp.wFormatAndPictureInfoFlags ^ hardware::dxva::kVp9ShowFrame);
        refused(p);
        bpp = pp;
        bpp.wFormatAndPictureInfoFlags = static_cast<uint16_t>(bpp.wFormatAndPictureInfoFlags & ~hardware::dxva::kVp9SubsamplingY);
        refused(p);
        bpp = pp;
        bpp.width = max_w + 64;
        refused(p);
        if (pp.wFormatAndPictureInfoFlags & hardware::dxva::kVp9FrameType && !(pp.wFormatAndPictureInfoFlags & hardware::dxva::kVp9IntraOnly)) {
          bpp = pp;
          bpp.frame_refs[rnd() % 3].bPicEntry = 0xFF;
          refused(p);
        }
        // Keys: an index past the list.
        uint32_t two_keys[2] = {1, 2};
        p = pic;
        p.surface_keys = ArrayView<const uint32_t>(two_keys, 2);
        if (pp.CurrPic.index() >= 2) refused(p);
      }
      const DxvaResult r = dec->decode(pic, &out);
      CHECK(r == DxvaResult::Ok);
      if (r != DxvaResult::Ok) {
        printf("  %s frame %zu: %s\n", file, decoded, out.detail ? out.detail : "?");
        break;
      }
      CHECK(out.surface == static_cast<uint32_t>(index_of[surface]));
      CHECK(out.frame.format.pixel_format == config.output);
      CHECK(out.frame.format.width == static_cast<uint32_t>(ref.header.info.width) && out.frame.format.height == static_cast<uint32_t>(ref.header.info.height));
      const bool exact = same_picture(out.frame, ref);
      CHECK(exact);
      if (exact) ++g_stats.exact;
      ++g_stats.decoded;
      if (!ref.header.info.show_frame) ++g_stats.hidden;
      surface_hash[surface] = hash_native(out.frame);
      surface_w[surface] = out.frame.format.width;
      surface_h[surface] = out.frame.format.height;
      if (ref.header.info.show_frame) {
        CHECK(displayed < expected.size());
        if (displayed < expected.size()) {
          CHECK(surface_hash[surface] == expected[displayed].hash);
          if (surface_hash[surface] == expected[displayed].hash) ++g_stats.matched_reference;
        }
        ++displayed;
      }
      for (int i = 0; i < 8; ++i)
        if (ref.header.refresh_frame_flags >> i & 1) {
          slots[i] = surface;
          sizes[i][0] = ref.header.info.width;
          sizes[i][1] = ref.header.info.height;
        }
      ++decoded;
    }
    CHECK(decoded == obs.frames.size());
    CHECK(displayed == expected.size());
    g_stats.displayed += static_cast<int>(displayed);
    ++g_stats.streams;
  }
  CHECK(a.live == 0);
}

// Bits flipped in the parameters and the frame, frames cut short: whatever the decoder answers, it must not fault, and a picture it gives is whole.
void fuzz(const char* file, bool ten_bit) {
  TestAllocator a;
  const std::vector<uint8_t> data = read_file(std::string("tests/data/") + file);
  const std::vector<std::vector<uint8_t>> frames = ivf_frames(data);
  Observer obs;
  {
    Vp9Decoder sw(a);
    sw.set_frame_observer(&obs);
    for (const std::vector<uint8_t>& f : frames) (void)sw.decode(ConstByteView(f.data(), f.size()), 0);
  }
  DxvaDecoderConfig config;
  config.profile = ten_bit ? DxvaProfile::Vp9Vld10BitProfile2 : DxvaProfile::Vp9VldProfile0;
  config.output = ten_bit ? PixelFormat::P010 : PixelFormat::Nv12;
  config.width = 1024;
  config.height = 1024;
  config.surfaces = 16;
  uint32_t seed = 777;
  auto rnd = [&seed]() {
    seed = seed * 1103515245u + 12345u;
    return seed >> 8;
  };
  int results[4] = {};
  for (int round = 0; round < 6; ++round) {
    UniquePointer<IDxvaDecoder> dec = create_dxva_decoder(a, config);
    CHECK(dec != nullptr);
    if (!dec) return;
    int slots[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int sizes[8][2] = {};
    int index_of[16];
    for (int s = 0; s < 16; ++s) index_of[s] = s;
    size_t decoded = 0;
    for (const std::vector<uint8_t>& f : frames) {
      int slot = 0;
      if (show_existing(f, &slot)) continue;
      if (decoded >= obs.frames.size()) break;
      const Decoded& ref = obs.frames[decoded++];
      const int surface = static_cast<int>(decoded % 12);
      PicParamsVp9 pp = fill_params(ref.header, surface, slots, sizes, index_of, ten_bit);
      std::vector<uint8_t> bits = f;
      uint8_t* ppb = reinterpret_cast<uint8_t*>(&pp);
      const int kind = static_cast<int>(rnd() % 8);   // (4 to 7: the frame as it is)
      if (kind == 0 && !bits.empty()) {
        for (int k = 0; k < 4; ++k) bits[rnd() % bits.size()] ^= static_cast<uint8_t>(1u << (rnd() % 8));
      } else if (kind == 1) {
        for (int k = 0; k < 3; ++k) ppb[rnd() % sizeof pp] ^= static_cast<uint8_t>(1u << (rnd() % 8));
      } else if (kind == 2 && !bits.empty()) {
        bits.resize(rnd() % bits.size());
      }
      SliceVpxShort slice = {0, static_cast<uint32_t>(bits.size()), 0};
      if (kind == 3) slice.SliceBytesInBuffer += rnd() % 64;
      DxvaPicture pic;
      pic.picture_parameters = ConstByteView(ppb, sizeof pp);
      pic.slice_control = ConstByteView(reinterpret_cast<const uint8_t*>(&slice), sizeof slice);
      pic.bitstream = ConstByteView(bits.data(), bits.size());
      DxvaDecodedPicture out;
      const DxvaResult r = dec->decode(pic, &out);
      ++results[static_cast<int>(r)];
      if (r == DxvaResult::Ok || r == DxvaResult::MissingReference) {
        CHECK(out.frame.format.valid() && out.frame.plane[0] && out.frame.plane[1]);
        uint64_t sum = 0;   // (reads every sample: the sanitizer sees a picture that is not all there)
        for (uint32_t y = 0; y < out.frame.format.height; ++y) sum += out.frame.plane[0][static_cast<size_t>(y) * out.frame.stride[0]];
        (void)sum;
      } else {
        CHECK(out.detail != nullptr);
      }
      for (int i = 0; i < 8; ++i)
        if (ref.header.refresh_frame_flags >> i & 1) {
          slots[i] = surface;
          sizes[i][0] = ref.header.info.width;
          sizes[i][1] = ref.header.info.height;
        }
    }
    if (round == 3) dec->reset();
  }
  printf("  VP9 damaged %s: %d ok, %d invalid, %d unsupported, %d missing reference\n", file, results[0], results[1], results[2], results[3]);
  CHECK(a.live == 0);
}

// A reference whose surface never held a picture: the frame is still decoded (MissingReference) and given back whole.
void missing_reference() {
  TestAllocator a;
  const std::vector<std::vector<uint8_t>> frames = ivf_frames(read_file("tests/data/vp9_inter.ivf"));
  Observer obs;
  {
    Vp9Decoder sw(a);
    sw.set_frame_observer(&obs);
    for (const std::vector<uint8_t>& f : frames) (void)sw.decode(ConstByteView(f.data(), f.size()), 0);
  }
  CHECK(obs.frames.size() >= 3);
  if (obs.frames.size() < 3) return;
  DxvaDecoderConfig config;
  config.profile = DxvaProfile::Vp9VldProfile0;
  config.output = PixelFormat::Nv12;
  config.width = 96;
  config.height = 64;
  config.surfaces = 8;
  {
    UniquePointer<IDxvaDecoder> dec = create_dxva_decoder(a, config);
    CHECK(dec != nullptr);
    if (!dec) return;
    int index_of[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    int slots[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int sizes[8][2] = {};
    // The key frame into surface 0.
    PicParamsVp9 pp = fill_params(obs.frames[0].header, 0, slots, sizes, index_of, false);
    SliceVpxShort slice = {0, static_cast<uint32_t>(frames[0].size()), 0};
    DxvaPicture pic;
    pic.picture_parameters = ConstByteView(reinterpret_cast<const uint8_t*>(&pp), sizeof pp);
    pic.slice_control = ConstByteView(reinterpret_cast<const uint8_t*>(&slice), sizeof slice);
    pic.bitstream = ConstByteView(frames[0].data(), frames[0].size());
    DxvaDecodedPicture out;
    CHECK(dec->decode(pic, &out) == DxvaResult::Ok);
    // The next frame, told its references are in surface 5, which holds nothing.
    for (int& s : slots) s = 5;
    for (auto& s : sizes) s[0] = obs.frames[0].header.info.width, s[1] = obs.frames[0].header.info.height;
    pp = fill_params(obs.frames[1].header, 1, slots, sizes, index_of, false);
    slice.SliceBytesInBuffer = static_cast<uint32_t>(frames[1].size());
    pic.bitstream = ConstByteView(frames[1].data(), frames[1].size());
    const DxvaResult r = dec->decode(pic, &out);
    CHECK(r == DxvaResult::MissingReference);
    CHECK(out.surface == 1 && out.frame.format.width == static_cast<uint32_t>(obs.frames[1].header.info.width));
    // An inter frame after reset(): no key frame since, so nothing to decode with.
    dec->reset();
    for (int& s : slots) s = 0;
    pp = fill_params(obs.frames[2].header, 2, slots, sizes, index_of, false);
    slice.SliceBytesInBuffer = static_cast<uint32_t>(frames[2].size());
    pic.bitstream = ConstByteView(frames[2].data(), frames[2].size());
    const DxvaResult after_reset = dec->decode(pic, &out);
    CHECK(after_reset == DxvaResult::Invalid || after_reset == DxvaResult::MissingReference);
  }
  CHECK(a.live == 0);
}

// Streams the profiles do not take.
void refused_profiles() {
  TestAllocator a;
  CHECK(create_dxva_decoder(a, DxvaDecoderConfig{DxvaProfile::Vp9VldProfile0, 64, 64, 4, PixelFormat::P010}) == nullptr);
  CHECK(create_dxva_decoder(a, DxvaDecoderConfig{DxvaProfile::Vp9Vld10BitProfile2, 64, 64, 4, PixelFormat::Nv12}) == nullptr);
  CHECK(create_dxva_decoder(a, DxvaDecoderConfig{DxvaProfile::Vp9VldProfile0, 64, 64, 0, PixelFormat::Nv12}) == nullptr);
  struct Case {
    const char* file;
    DxvaProfile profile;
    PixelFormat output;
  };
  // 4:4:4 (profile 1) and 10-bit streams into an 8-bit profile 0 decoder; an 8-bit stream into the 10-bit one.
  for (const Case& c : {Case{"vp9_444.ivf", DxvaProfile::Vp9VldProfile0, PixelFormat::Nv12}, Case{"vp9_10bit.ivf", DxvaProfile::Vp9VldProfile0, PixelFormat::Nv12},
                        Case{"vp9_intra.ivf", DxvaProfile::Vp9Vld10BitProfile2, PixelFormat::P010}, Case{"vp9_12bit_444.ivf", DxvaProfile::Vp9Vld10BitProfile2, PixelFormat::P010}}) {
    const std::vector<std::vector<uint8_t>> frames = ivf_frames(read_file(std::string("tests/data/") + c.file));
    CHECK(!frames.empty());
    if (frames.empty()) continue;
    Observer obs;
    {
      Vp9Decoder sw(a);
      sw.set_frame_observer(&obs);
      (void)sw.decode(ConstByteView(frames[0].data(), frames[0].size()), 0);
    }
    CHECK(!obs.frames.empty());
    if (obs.frames.empty()) continue;
    UniquePointer<IDxvaDecoder> dec = create_dxva_decoder(a, DxvaDecoderConfig{c.profile, 128, 128, 4, c.output});
    CHECK(dec != nullptr);
    if (!dec) continue;
    int index_of[4] = {0, 1, 2, 3};
    int slots[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int sizes[8][2] = {};
    const bool ten_bit = c.profile == DxvaProfile::Vp9Vld10BitProfile2;
    SliceVpxShort slice = {0, static_cast<uint32_t>(frames[0].size()), 0};
    for (int honest = 0; honest < 2; ++honest) {
      // Parameters that tell the truth (the profile does not take them), and parameters that claim the decoder's format (the frame header says otherwise).
      PicParamsVp9 pp = fill_params(obs.frames[0].header, 0, slots, sizes, index_of, ten_bit);
      if (honest) {
        pp.BitDepthMinus8Luma = pp.BitDepthMinus8Chroma = static_cast<uint8_t>(obs.frames[0].header.info.bit_depth - 8);
      } else {
        pp.profile = ten_bit ? 2 : 0;
      }
      DxvaPicture pic;
      pic.picture_parameters = ConstByteView(reinterpret_cast<const uint8_t*>(&pp), sizeof pp);
      pic.slice_control = ConstByteView(reinterpret_cast<const uint8_t*>(&slice), sizeof slice);
      pic.bitstream = ConstByteView(frames[0].data(), frames[0].size());
      DxvaDecodedPicture out;
      CHECK(dec->decode(pic, &out) == DxvaResult::Invalid);
      CHECK(out.detail != nullptr);
    }
  }
  CHECK(a.live == 0);
}

}  // namespace dxva_vp9_test

static void dxva_test_vp9() {
  using namespace dxva_vp9_test;
  struct Stream {
    const char* file;
    bool ten_bit;
  };
  static const Stream streams[] = {{"vp9_intra.ivf", false}, {"vp9_inter.ivf", false},   {"vp9_tiles.ivf", false},      {"vp9_lossless.ivf", false},
                                   {"vp9_segments.ivf", false}, {"vp9_scaled.ivf", false}, {"vp9_altref.ivf", false},     {"vp9_10bit.ivf", true},
                                   {"vp9_altref_10bit.ivf", true}};
  for (const Stream& s : streams) play(s.file, s.ten_bit, Mode::Plain);
  for (const Stream& s : streams) play(s.file, s.ten_bit, Mode::Keys);
  for (const Stream& s : streams) play(s.file, s.ten_bit, Mode::OutputKey);
  for (const char* f : {"vp9_inter.ivf", "vp9_altref.ivf", "vp9_segments.ivf"}) play(f, false, Mode::Damage);
  play("vp9_altref_10bit.ivf", true, Mode::Damage);
  missing_reference();
  refused_profiles();
  for (const char* f : {"vp9_inter.ivf", "vp9_altref.ivf", "vp9_tiles.ivf", "vp9_scaled.ivf"}) fuzz(f, false);
  fuzz("vp9_altref_10bit.ivf", true);
  printf("  VP9: %d stream runs, %d frames decoded (%d hidden), %d bit exact with the software decoder; %d displayed, %d equal to ffmpeg's\n", g_stats.streams,
         g_stats.decoded, g_stats.hidden, g_stats.exact, g_stats.displayed, g_stats.matched_reference);
}
