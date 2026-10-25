/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Host-side tests for the Intel hardware MPEG-2 decode session (IntelMpeg2Decoder) against a simulated GPU. The simulated engine plays the decode hardware: for every batch
// it reads the commands the way the engine would, checks that every address in them is mapped in the context's address space, that the picture parameters, the quantiser
// matrices and each slice's place in the picture and in the bitstream are the stream's (compared with what a separate CPU decoder reports for the same stream), and that
// the pictures named as references are the ones it painted for them; then it "decodes" by writing the picture the CPU decoder made at the same position in the stream
// into the output surface, tiled as the engine does. The session must hand back exactly the CPU decoder's pictures, in the same order.
//
// This does not show that the real engine decodes anything: the commands the stand-in checks follow the reference driver, as far as is known.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <memory>
#include <vector>

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "FakeIntelGpu.hpp"
#include "Hardware/VideoCodec/Intel/IntelMediaVideoEngine.hpp"
#include "Hardware/VideoCodec/Intel/IntelMpeg2Session.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2.hpp"

using namespace hardware;
using namespace hardware::intel_mfx;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    ++live;
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static std::vector<uint8_t> read_file(const char* path) {
  std::vector<uint8_t> bytes;
  FILE* f = fopen(path, "rb");
  if (!f) return bytes;
  uint8_t chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
  fclose(f);
  return bytes;
}

// Splits an elementary stream into units that each end before the next picture's headers (a sequence header, a group of pictures header or a picture header that
// follows slices starts a unit).
static std::vector<std::pair<size_t, size_t>> split_pictures(const std::vector<uint8_t>& d) {
  std::vector<std::pair<size_t, size_t>> units;
  size_t begin = 0;
  bool slices = false;
  for (size_t i = 0; i + 4 <= d.size(); ++i) {
    if (d[i] || d[i + 1] || d[i + 2] != 1) continue;
    const int code = d[i + 3];
    if ((code == 0xB3 || code == 0xB8 || code == 0x00) && slices) {
      units.push_back({begin, i});
      begin = i;
      slices = false;
    }
    if (code >= 0x01 && code <= 0xAF) slices = true;
  }
  units.push_back({begin, d.size()});
  return units;
}

struct Output {
  std::vector<int64_t> pts;
  std::vector<std::vector<uint8_t>> bytes;
  std::vector<VideoFormat> format;
};

static void take(Mpeg2Decoder& dec, Output* out) {
  VideoFrame f;
  while (dec.receive(&f)) {
    std::vector<uint8_t> bytes;
    for (uint32_t p = 0; p < 3; ++p)
      for (uint32_t y = 0; y < f.format.plane_height(p); ++y) bytes.insert(bytes.end(), f.plane[p] + uint64_t{y} * f.stride[p], f.plane[p] + uint64_t{y} * f.stride[p] + f.format.plane_width(p));
    out->pts.push_back(f.pts_us);
    out->bytes.push_back(bytes);
    out->format.push_back(f.format);
  }
}

static void drain(IVideoDecoder& dec, Output* out) {
  VideoFrame f;
  while (dec.receive(&f)) {
    std::vector<uint8_t> bytes;
    for (uint32_t p = 0; p < 3; ++p)
      for (uint32_t y = 0; y < f.format.plane_height(p); ++y) bytes.insert(bytes.end(), f.plane[p] + uint64_t{y} * f.stride[p], f.plane[p] + uint64_t{y} * f.stride[p] + f.format.plane_width(p));
    out->pts.push_back(f.pts_us);
    out->bytes.push_back(bytes);
    out->format.push_back(f.format);
  }
}

static bool feed(IVideoDecoder& dec, ConstByteView au, int64_t pts, Output* out) {
  VideoPacket p;
  p.data = au;
  p.pts_us = pts;
  SubmitResult r = dec.submit(p);
  if (r == SubmitResult::Full) {
    drain(dec, out);
    r = dec.submit(p);
  }
  drain(dec, out);
  return r == SubmitResult::Accepted;
}


// What the CPU decoder says about each picture it decodes (a field is a picture): the description an engine is given, the picture it makes, and which earlier pictures it
// predicts from (by position in the stream).
struct RefPicture {
  int type = 0, structure = 0, f_code[2][2] = {{0, 0}, {0, 0}};
  int intra_dc_precision = 0;
  bool top_field_first = false, frame_pred_frame_dct = false, concealment_motion_vectors = false, q_scale_type = false, intra_vlc_format = false, alternate_scan = false, second_field = false;
  uint8_t intra_matrix[64] = {}, non_intra_matrix[64] = {};
  int forward = -1, backward = -1;
  uint32_t width = 0, height = 0, mb_width = 0, mb_height = 0;
  std::vector<uint8_t> plane[3];
  uint32_t stride[3] = {0, 0, 0};
  std::vector<std::vector<uint8_t>> slices;
  std::vector<mpeg2::SliceInfo> info;
};

class Observer final : public mpeg2::IFrameObserver {
 public:
  std::vector<RefPicture> pictures;
  void picture_decoded(const mpeg2::OffloadPicture& o) override {
    RefPicture r;
    r.type = o.pic->type;
    r.structure = o.pic->structure;
    for (int d = 0; d < 2; ++d)
      for (int c = 0; c < 2; ++c) r.f_code[d][c] = o.pic->f_code[d][c];
    r.intra_dc_precision = o.pic->intra_dc_precision;
    r.top_field_first = o.pic->top_field_first;
    r.frame_pred_frame_dct = o.pic->frame_pred_frame_dct;
    r.concealment_motion_vectors = o.pic->concealment_motion_vectors;
    r.q_scale_type = o.pic->q_scale_type;
    r.intra_vlc_format = o.pic->intra_vlc_format;
    r.alternate_scan = o.pic->alternate_scan;
    r.second_field = o.second_field;
    memcpy(r.intra_matrix, o.intra_matrix, 64);
    memcpy(r.non_intra_matrix, o.non_intra_matrix, 64);
    r.mb_width = uint32_t(o.seq->mb_width);
    r.mb_height = uint32_t(o.seq->mb_height);
    r.width = r.mb_width * 16;
    r.height = r.mb_height * 16;
    if (o.forward_slot >= 0) r.forward = owner_[o.forward_slot];
    if (o.backward_slot >= 0) r.backward = owner_[o.backward_slot];
    for (int c = 0; c < 3; ++c) {
      const uint32_t h = c == 0 ? r.height : r.height / 2;
      r.stride[c] = uint32_t(o.stride[c]);
      r.plane[c].assign(o.plane[c].data(), o.plane[c].data() + uint64_t{r.stride[c]} * h);
    }
    for (const ConstByteView& s : o.slices) r.slices.emplace_back(s.data(), s.data() + s.size());
    for (const mpeg2::SliceInfo& i : o.slice_info) r.info.push_back(i);
    owner_[o.slot] = int(pictures.size());
    pictures.push_back(std::move(r));
  }

 private:
  int owner_[mpeg2::kPoolSize] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
};

// The CPU decoder's view of a stream: its units, its pictures in coding order and its output.
struct Reference {
  std::vector<uint8_t> file;
  std::vector<std::pair<size_t, size_t>> units;
  Output out;
  Observer observer;
  ConstByteView unit(size_t k) const { return ConstByteView(file.data() + units[k].first, units[k].second - units[k].first); }
};

static bool make_reference(allocators::IAllocator& a, const char* name, Reference* ref) {
  char path[160];
  snprintf(path, sizeof path, "tests/data/mpeg2_%s.m2v", name);
  ref->file = read_file(path);
  if (ref->file.empty()) return false;
  ref->units = split_pictures(ref->file);
  Mpeg2Decoder dec(a);
  dec.set_frame_observer(&ref->observer);
  for (size_t k = 0; k < ref->units.size(); ++k) {
    dec.decode(ref->unit(k), int64_t(k) * 1000);
    take(dec, &ref->out);
  }
  dec.flush();
  take(dec, &ref->out);
  return !ref->out.pts.empty() && ref->observer.pictures.size() >= ref->units.size();
}

// The decode engine of the simulated GPU.
struct FakeDecoder {
  const Reference* reference = nullptr;
  bool check_content = true;
  std::map<uint64_t, int> painted;   // surface address -> the picture (by position in the stream) painted in it
  int pictures = 0, problems = 0;
  size_t next = 0;

  static uint64_t addr(const std::vector<uint32_t>& d, size_t at) { return (uint64_t{d[at + 1]} << 32) | d[at]; }
  void problem(const char* what) {
    ++problems;
    printf("  fake engine: %s\n", what);
  }

  struct Slice { uint32_t size, offset, h, v, count, next_v, next_h, q; bool last; uint32_t bit; };

  void run(FakeGpu& gpu, const std::vector<uint32_t>& d) {
    uint64_t out_pre = 0, out_post = 0, refs[16] = {}, bitstream = 0, bsd_mpc = 0;
    uint32_t format = 99, pic1 = 0, pic2 = 0, pic3 = 0, surf_w = 0, surf_h = 0, surf_pitch = 0, surf_chroma = 0;
    uint8_t qm[2][64];
    bool have_qm[2] = {false, false};
    std::vector<Slice> slices;
    bool pic_state = false;
    size_t i = 0;
    while (i < d.size()) {
      const uint32_t h = d[i];
      if (h == 0) { ++i; continue; }
      if ((h >> 23) == 0x0A) break;
      if ((h >> 29) == 0) { i += (h & 0x3F) + 2; continue; }
      const size_t len = (h & 0xFF) + 2;
      if (i + len > d.size()) { problem("command runs past the batch"); return; }
      switch (h & 0xFFFF0000) {
        case kMfxPipeModeSelect:
          format = d[i + 1] & 0xF;
          if (!(d[i + 1] & (1u << 8)) || (d[i + 1] & (1u << 9))) problem("MPEG-2 output must be the pre-deblocking one");
          break;
        case kMfxSurfaceState:
          surf_w = ((d[i + 2] >> 4) & 0x3FFF) + 1;
          surf_h = ((d[i + 2] >> 18) & 0x3FFF) + 1;
          surf_pitch = ((d[i + 3] >> 3) & 0x1FFFF) + 1;
          surf_chroma = d[i + 4] & 0xFFFF;
          break;
        case kMfxPipeBufAddrState:
          out_pre = addr(d, i + 1);
          out_post = addr(d, i + 4);
          for (int k = 0; k < 16; ++k) refs[k] = addr(d, i + 19 + 2 * k);
          break;
        case kMfxBspBufBaseAddrState: bsd_mpc = addr(d, i + 1); break;
        case kMfxIndObjBaseAddrState: bitstream = addr(d, i + 1); break;
        case kMfxMpeg2PicState: pic1 = d[i + 1]; pic2 = d[i + 2]; pic3 = d[i + 3]; pic_state = true; break;
        case kMfxQmState: {
          const uint32_t type = d[i + 1];
          if (type > 1) { problem("quantiser matrix type"); break; }
          for (int k = 0; k < 64; ++k) qm[type][k] = uint8_t(d[i + 2 + k / 4] >> (8 * (k % 4)));
          have_qm[type] = true;
          break;
        }
        case kMfdMpeg2BsdObject: {
          Slice s;
          s.size = d[i + 1];
          s.offset = d[i + 2];
          s.h = d[i + 3] >> 24;
          s.v = (d[i + 3] >> 16) & 0xFF;
          s.count = (d[i + 3] >> 8) & 0xFF;
          s.last = (d[i + 3] >> 5) & 1;
          if (((d[i + 3] >> 3) & 1) != uint32_t(s.last)) problem("last slice flags differ");
          s.bit = d[i + 3] & 7;
          s.q = d[i + 4] >> 24;
          s.next_v = (d[i + 4] >> 8) & 0xFFFF;
          s.next_h = d[i + 4] & 0xFF;
          slices.push_back(s);
          break;
        }
        default: break;
      }
      i += len;
    }
    if (format != kFormatMpeg2 || !pic_state || !have_qm[0] || !have_qm[1] || slices.empty()) { problem("incomplete MPEG-2 batch"); return; }
    if (out_post != 0 || out_pre == 0 || !bitstream || !bsd_mpc) problem("buffer addresses");
    const uint32_t width_mbs = (pic3 & 0xFFFF) + 1, height_mbs = ((pic3 >> 16) & 0x3FFF) + 1;
    if (surf_w != width_mbs * 16 || surf_h != height_mbs * 16 || !(pic3 >> 31)) problem("surface or picture size");
    const uint32_t structure = (pic1 >> 12) & 3, coding_type = (pic2 >> 9) & 7;
    // Every address the commands use must be mapped in the context's address space.
    for (uint64_t a : {out_pre, bitstream, bsd_mpc}) {
      bool ok;
      if (!gpu.ppgtt_bytes(a, 1, "command address", &ok) || !ok) problem("an address in the commands is not mapped");
    }
    const size_t k = next++;
    const RefPicture* pic = reference && check_content && k < reference->observer.pictures.size() ? &reference->observer.pictures[k] : nullptr;
    if (pic) {
      if (pic->type != int(coding_type) || pic->structure != int(structure) || pic->mb_width != width_mbs || pic->mb_height != height_mbs) problem("picture type, structure or size differ from the stream's");
      const bool fields_ok = int((pic1 >> 16) & 15) == pic->f_code[0][0] && int((pic1 >> 20) & 15) == pic->f_code[0][1] && int((pic1 >> 24) & 15) == pic->f_code[1][0] && int((pic1 >> 28) & 15) == pic->f_code[1][1];
      if (!fields_ok) problem("f_code differs");
      if (int((pic1 >> 14) & 3) != pic->intra_dc_precision || bool((pic1 >> 11) & 1) != pic->top_field_first || bool((pic1 >> 10) & 1) != pic->frame_pred_frame_dct ||
          bool((pic1 >> 9) & 1) != pic->concealment_motion_vectors || bool((pic1 >> 8) & 1) != pic->q_scale_type || bool((pic1 >> 7) & 1) != pic->intra_vlc_format ||
          bool((pic1 >> 6) & 1) != pic->alternate_scan)
        problem("picture coding extension differs");
      if (memcmp(qm[0], pic->intra_matrix, 64) || memcmp(qm[1], pic->non_intra_matrix, 64)) problem("quantiser matrix differs");
      // The references: the pictures painted for the pictures this one predicts from.
      auto painted_as = [&](uint64_t a) { auto it = painted.find(a); return it == painted.end() ? -2 : it->second; };
      if (coding_type == 2 && structure == 3 && (painted_as(refs[0]) != pic->forward || painted_as(refs[1]) != pic->forward)) problem("forward reference is not the picture it should be");
      if (coding_type == 3 && (painted_as(refs[0]) != pic->forward || painted_as(refs[1]) != pic->backward)) problem("B references are not the pictures they should be");
      // The slices: where they are in the bitstream, where in the picture.
      uint64_t base = 0;
      const size_t n = pic->slices.size();
      if (slices.size() != n) { printf("  picture %zu: %zu slices in the batch, %zu in the stream (type %d structure %d)\n", k, slices.size(), n, pic->type, pic->structure); problem("slice count differs"); return; }
      for (size_t s = 0; s < n; ++s) {
        const std::vector<uint8_t>& bits = pic->slices[s];
        const mpeg2::SliceInfo& info = pic->info[s];
        const Slice& hw = slices[s];
        const uint64_t skip = info.mb_bit_offset >> 3;
        if (hw.offset != base + skip || hw.size != bits.size() - skip || hw.bit != (info.mb_bit_offset & 7) || int(hw.h) != info.mb_col || int(hw.v) != info.mb_row ||
            int(hw.q) != info.quantiser_scale_code || hw.last != (s + 1 == n))
          problem("slice position differs");
        const uint32_t next_h = s + 1 < n ? uint32_t(pic->info[s + 1].mb_col) : 0, next_v = s + 1 < n ? uint32_t(pic->info[s + 1].mb_row) : (structure == 3 ? height_mbs : height_mbs / 2);
        if (hw.next_h != next_h || hw.next_v != next_v || hw.count != (next_v * width_mbs + next_h) - (uint32_t(info.mb_row) * width_mbs + uint32_t(info.mb_col))) problem("next slice or macroblock count differs");
        // The bytes in the bitstream buffer.
        for (uint64_t off = 0; off < bits.size(); off += 1) {
          bool ok;
          uint8_t* p = gpu.ppgtt_bytes(bitstream + base + off, 1, "bitstream", &ok);
          if (!ok) { problem("bitstream not mapped"); return; }
          if (*p != bits[off]) { problem("bitstream bytes differ"); break; }
        }
        base += bits.size();
      }
    }
    // "Decode": paint the output picture with the CPU decoder's, tiled. A field picture's lines only.
    Nv12TiledLayout layout;
    if (!nv12_tiled_layout(width_mbs * 16, height_mbs * 16, &layout) || layout.pitch != surf_pitch || layout.chroma_offset_rows != surf_chroma) { problem("surface layout"); return; }
    uint8_t* page = nullptr;
    uint64_t page_index = ~0ull;
    auto put = [&](uint64_t offset, uint8_t value) {
      if (offset / 4096 != page_index) {
        bool ok;
        page = gpu.ppgtt_bytes(out_pre + (offset / 4096) * 4096, 4096, "output surface", &ok);
        page_index = offset / 4096;
        if (!ok) page = nullptr;
      }
      if (page) page[offset % 4096] = value;
    };
    const bool field = structure != 3;
    const bool second_field = pic && pic->second_field;
    if (!field || !second_field)
      for (uint64_t off = 0; off < layout.size; ++off) put(off, 0xEE);
    if (pic) {
      const uint32_t first = field && structure == 2 ? 1 : 0, step = field ? 2 : 1;
      for (uint32_t y = first; y < pic->height; y += step)
        for (uint32_t x = 0; x < pic->width; ++x) put(y_tile_offset(layout.pitch, x, y), pic->plane[0][uint64_t{y} * pic->stride[0] + x]);
      for (uint32_t y = first; y < pic->height / 2; y += step)
        for (uint32_t x = 0; x < pic->width / 2; ++x) {
          put(y_tile_offset(layout.pitch, 2 * x, layout.chroma_offset_rows + y), pic->plane[1][uint64_t{y} * pic->stride[1] + x]);
          put(y_tile_offset(layout.pitch, 2 * x + 1, layout.chroma_offset_rows + y), pic->plane[2][uint64_t{y} * pic->stride[2] + x]);
        }
    }
    painted[out_pre] = int(k);
    ++pictures;
  }
};

struct Rig2 : Rig {
  FakeDecoder engine;
  Rig2() { gpu.on_batch = [this](FakeGpu& g, const std::vector<uint32_t>& d) { engine.run(g, d); }; }
};

static DecoderConfig i420_config() {
  DecoderConfig c;
  c.codec = VideoCodec::Mpeg2;
  c.output = PixelFormat::I420;
  return c;
}

static void compare_stream(const char* name) {
  printf("  %s\n", name);
  TestAllocator a;
  {
    Reference ref;
    CHECK(make_reference(a, name, &ref));
    if (ref.units.empty()) return;
    printf("    %zu units in, %zu pictures decoded, %zu out\n", ref.units.size(), ref.observer.pictures.size(), ref.out.pts.size());
    Rig2 rig;
    rig.engine.reference = &ref;
    {
      VcsDriver driver(rig.mem);
      CHECK(driver.initialize(rig.config()));
      {
        IntelMpeg2Decoder session(a, driver, i420_config());
        CHECK(session.prepare());
        Output out;
        for (size_t k = 0; k < ref.units.size(); ++k) CHECK(feed(session, ref.unit(k), int64_t(k) * 1000, &out));
        session.flush();
        drain(session, &out);
        CHECK(session.statistics().errors == 0 && session.statistics().frames == ref.out.pts.size());
        CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
        CHECK(rig.engine.pictures == int(ref.observer.pictures.size()) && session.pictures_decoded() == ref.observer.pictures.size());
        // The same pictures, in the same order, byte for byte.
        CHECK(out.pts == ref.out.pts);
        for (size_t i = 0; i < out.bytes.size() && i < ref.out.pts.size(); ++i) {
          CHECK(out.format[i] == ref.out.format[i]);
          CHECK(out.bytes[i] == ref.out.bytes[i]);
        }
        CHECK(!out.format.empty() && session.output_format().width == out.format.front().width && session.output_format().height == out.format.front().height);
      }
    }
    CHECK(rig.gpu.violations.empty());
    CHECK(rig.mem.live() == 1);   // only the scratch page that is never freed
  }
  CHECK(a.live == 0);
}

static void test_lease_and_config() {
  TestAllocator a;
  Rig2 rig;
  {
    VcsDriver driver(rig.mem);
    CHECK(driver.initialize(rig.config()));
    {
      IntelMpeg2Decoder first(a, driver, i420_config());
      CHECK(first.prepare());
      IntelMpeg2Decoder second(a, driver, i420_config());
      CHECK(!second.prepare());
    }
    DecoderConfig junk = i420_config();
    const uint8_t bytes[4] = {'j', 'u', 'n', 'k'};
    junk.codec_config = ConstByteView(bytes, 4);
    IntelMpeg2Decoder bad(a, driver, junk);
    CHECK(!bad.prepare());   // MPEG-2 has no configuration record
    IntelMpeg2Decoder third(a, driver, i420_config());
    CHECK(third.prepare());   // the lease came back
  }
  CHECK(a.live == 0);
}

// The engine stops answering in the middle of a stream.
static void test_hang() {
  TestAllocator a;
  Reference ref;
  CHECK(make_reference(a, "ipp", &ref));
  Rig2 rig;
  rig.engine.reference = &ref;
  int live_with_session = 0;
  {
    VcsDriver driver(rig.mem);
    CHECK(driver.initialize(rig.config()));
    {
      IntelMpeg2Decoder session(a, driver, i420_config());
      CHECK(session.prepare());
      Output out;
      CHECK(feed(session, ref.unit(0), 0, &out));
      CHECK(rig.engine.pictures == 1);
      rig.gpu.hang = true;
      CHECK(feed(session, ref.unit(1), 1000, &out));
      CHECK(driver.state() == VcsDriver::State::Wedged && session.statistics().errors >= 1);
      const int pictures = rig.engine.pictures;
      CHECK(feed(session, ref.unit(2), 2000, &out));
      CHECK(rig.engine.pictures == pictures);
      session.flush();
      drain(session, &out);
      CHECK(out.bytes.size() >= 3 && out.bytes[0] == ref.out.bytes[0]);
      if (out.bytes.size() >= 3) CHECK(out.bytes[1][0] == 128 && out.bytes[2][0] == 128);
      live_with_session = rig.mem.live();
    }
    CHECK(rig.mem.live() == live_with_session);   // the engine may still be writing: nothing was given back
  }
  CHECK(rig.gpu.violations.empty());
  CHECK(a.live == 0);
}

// A picture size beyond the engine's limit is decoded on the CPU.
static void test_cpu_fallback_when_busy() {
  TestAllocator a;
  Reference ref;
  CHECK(make_reference(a, "ibp", &ref));
  Rig2 rig;
  rig.engine.reference = &ref;
  {
    VcsDriver driver(rig.mem);
    CHECK(driver.initialize(rig.config()));
    IntelMpeg2Decoder session(a, driver, i420_config());
    CHECK(session.prepare());
    rig.gpu.hang = true;   // (wedge it before the stream starts: the session must not touch the engine then)
    CHECK(driver.run_batch(ArrayView<const uint32_t>()) != VcsDriver::Result::Done);
    Output out;
    for (size_t k = 0; k < ref.units.size(); ++k) feed(session, ref.unit(k), int64_t(k) * 1000, &out);
    session.flush();
    drain(session, &out);
    CHECK(out.pts == ref.out.pts);   // the stream still came out (grey pictures, but all of them)
  }
  CHECK(a.live == 0);
}

// Damaged streams: units cut short and bits flipped. The engine is never pointed at anything unmapped or freed, nothing leaks.
static void test_damaged_streams() {
  TestAllocator a;
  for (const char* name : {"ibp", "syn_fields"}) {
    Reference ref;
    CHECK(make_reference(a, name, &ref));
    int runs = 0;
    for (size_t variant = 0; variant < 24; ++variant) {
      Rig2 rig;
      rig.engine.reference = &ref;
      rig.engine.check_content = false;
      {
        VcsDriver driver(rig.mem);
        CHECK(driver.initialize(rig.config()));
        {
          IntelMpeg2Decoder session(a, driver, i420_config());
          CHECK(session.prepare());
          Output out;
          for (size_t k = 0; k < ref.units.size(); ++k) {
            std::vector<uint8_t> au(ref.unit(k).data(), ref.unit(k).data() + ref.unit(k).size());
            if (k > 0 && variant % 2 == 1) au.resize(au.size() * (variant + 1) / 25 + 1);
            else if (k > 0) au[(variant * 7919 + k * 104729) % au.size()] ^= uint8_t(1u << (variant % 8));
            VideoPacket p;
            p.data = ConstByteView(au.data(), au.size());
            p.pts_us = int64_t(k) * 1000;
            if (session.submit(p) == SubmitResult::Full) { drain(session, &out); session.submit(p); }
            drain(session, &out);
            ++runs;
          }
          session.flush();
          drain(session, &out);
        }
      }
      CHECK(rig.gpu.violations.empty());
    }
    CHECK(runs > 100);
  }
  CHECK(a.live == 0);
}

// The device: MPEG-2 sessions go to the engine, one at a time, everything else stays on the CPU.
static void test_device() {
  TestAllocator a;
  Reference ref;
  CHECK(make_reference(a, "ibp", &ref));
  Rig2 rig;
  rig.engine.reference = &ref;
  std::vector<uint32_t> regs(0x20000 / 4, 0);
  regs[0x0D88 / 4] = 1;   // the video box answers the power request
  const MmioRegion region(reinterpret_cast<uint64_t>(regs.data()), regs.size() * 4);
  IntelMediaVideoEngine::HardwareSetup setup;
  setup.registers = &rig.gpu;
  setup.ggtt_table = rig.config().ggtt_table;
  setup.ggtt_first_page = 100;
  setup.memory = &rig.mem;
  {
    IntelMediaVideoEngine device(a, 0x3E92, region);   // Coffee Lake
    device.set_hardware(setup);
    CHECK(device.initialize());
    CHECK(device.hardware_decode() && (device.info().accelerated_decode_codecs & codec_mask(VideoCodec::Mpeg2)));
    CHECK(device.engine_decode_codecs() & codec_mask(VideoCodec::Mpeg2) && !(device.engine_encode_codecs() & codec_mask(VideoCodec::Mpeg2)));
    {
      UniquePointer<IVideoDecoder> hw = device.create_decoder(i420_config());
      CHECK(hw != nullptr);
      Output out;
      for (size_t k = 0; k < ref.units.size(); ++k) CHECK(feed(*hw, ref.unit(k), int64_t(k) * 1000, &out));
      hw->flush();
      drain(*hw, &out);
      CHECK(out.pts == ref.out.pts && out.bytes == ref.out.bytes);
      CHECK(rig.engine.pictures == int(ref.observer.pictures.size()));
      // The engine is taken: the next session runs on the CPU and sends nothing to it.
      UniquePointer<IVideoDecoder> sw = device.create_decoder(i420_config());
      CHECK(sw != nullptr);
      Output out2;
      for (size_t k = 0; k < ref.units.size(); ++k) CHECK(feed(*sw, ref.unit(k), int64_t(k) * 1000, &out2));
      sw->flush();
      drain(*sw, &out2);
      CHECK(out2.bytes == ref.out.bytes && rig.engine.pictures == int(ref.observer.pictures.size()));
    }
  }
  CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
  CHECK(a.live == 0);
}

int main() {
  static const char* const streams[] = {"intra", "ipp", "ibp", "interlaced", "interlaced_bff", "alt", "dc10", "highrate", "lowrate", "matrix", "oddsize", "syn_frames", "syn_fields", "syn_fields_bff"};
  for (const char* name : streams)
    if (!getenv("ONLY") || !strcmp(getenv("ONLY"), name)) compare_stream(name);
  test_lease_and_config();
  test_hang();
  test_cpu_fallback_when_busy();
  test_damaged_streams();
  test_device();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("IntelMpeg2SessionTest passed\n");
  return 0;
}
