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


// Host-side tests for the VCN AV1 decode session (AmdVcnAv1Decoder) against a simulated engine: the ring controller of FakeAmdVcn.hpp and a stand-in for the engine's
// firmware. The stand-in reads the messages the way the firmware does (offsets written out here by hand from Mesa's structures), checks that the buffers it is given are
// inside the work memory and do not overlap, that the context buffer starts with the initial entropy contexts, that the bitstream holds the frame's tiles back to back
// and the message names where each starts, and that the reference slots the message names hold the frames the stream says (by order hint); then it "decodes" by writing
// the picture a separate CPU decode made of the same frame into the target surface, as linear NV12. The session must hand back exactly the CPU decoder's pictures, in
// the same order. Streams the engine is not given (10-bit, 4:4:4, film grain, super-resolution, ...) must come out of the CPU, identical, without the engine being touched.
//
// This does not show that the real engine decodes anything: the message contents the stand-in checks are the contents Mesa sends, as far as they are known.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <memory>
#include <vector>
#include "FakeAmdVcn.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnAv1Decoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnVideoEngine.hpp"
#include "Hardware/VideoCodec/Software/Av1.hpp"

using namespace hardware;
using namespace fakevcn;

// The stream the engine's self-test decodes (kAv1SelfTestStream, kAv1SelfTestEnds, ...).
namespace {
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestAv1Stream.inc"
}

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

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

// The temporal units of an IVF file, back to back in `stream`, with where each one ends.
static bool read_ivf(const std::vector<uint8_t>& d, std::vector<uint8_t>* stream, std::vector<size_t>* ends) {
  if (d.size() < 32 || memcmp(d.data(), "DKIF", 4) != 0) return false;
  size_t pos = d[6] | (d[7] << 8);
  while (pos + 12 <= d.size()) {
    const size_t size = d[pos] | (d[pos + 1] << 8) | (d[pos + 2] << 16) | (size_t(d[pos + 3]) << 24);
    if (size > d.size() - pos - 12) return false;
    stream->insert(stream->end(), d.begin() + pos + 12, d.begin() + pos + 12 + size);
    ends->push_back(stream->size());
    pos += 12 + size;
  }
  return !ends->empty();
}

struct Output {
  std::vector<int64_t> pts;
  std::vector<std::vector<uint8_t>> bytes;
  std::vector<VideoFormat> format;
};

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

// The CPU decoder's view of a stream: its temporal units, its pictures, and for every frame it decodes (hidden ones too) what the stand-in firmware needs to know.
struct FrameRecord {
  int width = 0, height = 0, frame_type = 0, frame_offset = 0, base_qindex = 0, tile_cols = 0, tile_rows = 0, refresh = 0;
  bool inter = false;
  int expected_ref_offset[7] = {};
  std::vector<uint8_t> tiles;                 // the tile data back to back
  std::vector<uint32_t> tile_sizes;
  std::vector<uint8_t> nv12_luma, nv12_chroma;   // the decoded picture, 8-bit: luma rows then interleaved chroma rows
};

struct Reference final : av1::IFrameObserver {
  std::vector<uint8_t> stream;
  std::vector<size_t> ends;
  std::vector<FrameRecord> frames;
  Output out;
  int held_offset[8] = {};

  ConstByteView unit(size_t k) const {
    const size_t start = k == 0 ? 0 : ends[k - 1];
    return ConstByteView(stream.data() + start, ends[k] - start);
  }

  void frame_decoded(const av1::SequenceHeader& seq, const av1::FrameHeader& hdr, ArrayView<const av1::TileData> tiles, const av1::Picture& pic) override {
    FrameRecord r;
    r.width = hdr.width[1];
    r.height = hdr.height;
    r.frame_type = hdr.frame_type;
    r.frame_offset = hdr.frame_offset;
    r.base_qindex = hdr.quant.yac;
    r.tile_cols = hdr.tiling.cols;
    r.tile_rows = hdr.tiling.rows;
    r.refresh = hdr.refresh_frame_flags;
    r.inter = (hdr.frame_type & 1) != 0;
    if (r.inter)
      for (int i = 0; i < 7; ++i) r.expected_ref_offset[i] = held_offset[hdr.refidx[i]];
    for (const av1::TileData& t : tiles) {
      r.tiles.insert(r.tiles.end(), t.data.data(), t.data.data() + t.data.size());
      r.tile_sizes.push_back(static_cast<uint32_t>(t.data.size()));
    }
    for (int i = 0; i < 8; ++i)
      if (hdr.refresh_frame_flags & (1 << i)) held_offset[i] = hdr.frame_offset;
    if (seq.hbd == 0 && seq.layout == av1::PIXEL_LAYOUT_I420) {
      for (int y = 0; y < pic.height(); ++y)
        for (int x = 0; x < pic.width(); ++x) r.nv12_luma.push_back(static_cast<uint8_t>(pic.plane(0).at(x, y)));
      for (int y = 0; y < pic.plane_height(1); ++y)
        for (int x = 0; x < pic.plane_width(1); ++x) {
          r.nv12_chroma.push_back(static_cast<uint8_t>(pic.plane(1).at(x, y)));
          r.nv12_chroma.push_back(static_cast<uint8_t>(pic.plane(2).at(x, y)));
        }
    }
    frames.push_back(static_cast<FrameRecord&&>(r));
  }
};

static bool make_reference(allocators::IAllocator& a, Reference* ref) {
  Av1Decoder dec(a);
  dec.set_frame_observer(ref);
  VideoFrame f;
  for (size_t k = 0; k < ref->ends.size(); ++k) {
    dec.decode(ref->unit(k), int64_t(k) * 1000);
    while (dec.receive(&f)) {
      std::vector<uint8_t> bytes;
      for (uint32_t p = 0; p < 3; ++p)
        for (uint32_t y = 0; y < f.format.plane_height(p); ++y) bytes.insert(bytes.end(), f.plane[p] + uint64_t{y} * f.stride[p], f.plane[p] + uint64_t{y} * f.stride[p] + f.format.plane_width(p));
      ref->out.pts.push_back(f.pts_us);
      ref->out.bytes.push_back(bytes);
      ref->out.format.push_back(f.format);
    }
  }
  return !ref->out.pts.empty();
}

static bool make_reference_file(allocators::IAllocator& a, const char* name, Reference* ref) {
  char path[160];
  snprintf(path, sizeof path, "tests/data/av1_%s.ivf", name);
  return read_ivf(read_file(path), &ref->stream, &ref->ends) && make_reference(a, ref);
}

static uint32_t word(const uint8_t* p, uint32_t offset) { return p[offset] | (p[offset + 1] << 8) | (p[offset + 2] << 16) | (uint32_t{p[offset + 3]} << 24); }

struct FakeMapper final : IRegisterMapper {
  uint8_t* host = nullptr;
  uint64_t physical_base = 0, size = 0;
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    if (physical < physical_base || length > size || physical - physical_base > size - length) return false;
    *region = MmioRegion(reinterpret_cast<uint64_t>(host + (physical - physical_base)), length);
    return true;
  }
};

struct Rig {
  static constexpr uint64_t kArena = 160ull << 20;
  static constexpr uint64_t kGpuBase = 0x4'0000'0000ull, kPhysicalBase = 0xE000'0000ull;
  std::vector<uint8_t> arena, ring_page;
  FakeMapper mapper;
  AmdVcnWorkMemory memory;
  Vcn vcn;
  FakeClock clock;
  std::unique_ptr<AmdVcn1Ring> ring;
  AmdVcnWorkMemory::Piece ib, fence;

  const Reference* reference = nullptr;
  bool write_pictures = true;      // the engine really decodes (off: it leaves the target as it was)
  bool check_content = true;       // compare with the CPU decoder's records (off for damaged streams)
  std::map<uint32_t, uint64_t> buffer;
  int creates = 0, destroys = 0, decodes = 0, problems = 0;
  uint32_t session_decodes = 0;
  std::vector<uint32_t> stream_handles;
  struct Slot { bool valid = false; int frame_offset = 0; };
  Slot slots[16];

  explicit Rig(uint64_t arena_bytes = kArena) : arena(arena_bytes), ring_page(AmdVcn1::kRingBytes) {
    mapper.host = arena.data();
    mapper.physical_base = kPhysicalBase;
    mapper.size = arena.size();
    memory.configure(&mapper, kGpuBase, kPhysicalBase, arena.size());
    memory.allocate(4096, 4096, &ib);
    memory.allocate(4096, 4096, &fence);
    memory.keep();
    vcn.windows = {{kGpuBase, arena.data(), arena.size()}};
    vcn.windows.push_back({0x5'0000'0000ull, ring_page.data(), ring_page.size()});
    vcn.ring_gpu = 0x5'0000'0000ull;
    AmdVcn1Ring::Memory m;
    m.ib_gpu_address = ib.gpu_address;
    m.ib = ib.cpu;
    m.fence_gpu_address = fence.gpu_address;
    m.fence = fence.cpu;
    m.ring = MmioRegion(reinterpret_cast<uint64_t>(ring_page.data()), ring_page.size());
    ring.reset(new AmdVcn1Ring(vcn, clock, m));
    vcn.on_command = [this](uint32_t c, uint64_t a) { on_command(c, a); };
    vcn.on_engine = [this] { on_engine(); };
  }

  void problem(const char* what) {
    ++problems;
    printf("  fake engine: %s\n", what);
  }

  void on_command(uint32_t command, uint64_t address) {
    buffer[command] = address;
    if (command != 0) return;
    const uint8_t* m = vcn.translate(address, 40);
    if (!m) { problem("message outside the work memory"); return; }
    const uint32_t type = word(m, 12);
    if (type == 0) {
      ++creates;
      session_decodes = 0;
      for (Slot& slot : slots) slot = Slot();
      stream_handles.push_back(word(m, 16));
      if (word(m, 0) != 40 || word(m, 4) != 56 || word(m, 8) != 1 || word(m, 24) != 1 || word(m, 28) != 40 || word(m, 32) != 16) problem("bad create header");
      const uint8_t* c = vcn.translate(address + 40, 16);
      if (!c || word(c, 0) != 0x13) problem("create: stream type is not AV1");
    } else if (type == 2) {
      ++destroys;
      if (word(m, 0) != 40 || word(m, 8) != 0) problem("bad destroy header");
    }
  }

  struct Range { const char* name; uint64_t start, size; };

  void on_engine() {
    const uint64_t msg_addr = buffer[0];
    constexpr uint32_t kMessageBytes = 244 + 3576;
    uint8_t m[kMessageBytes];
    if (!vcn.translate(msg_addr, sizeof m)) { problem("decode message outside the work memory"); return; }
    memcpy(m, vcn.translate(msg_addr, sizeof m), sizeof m);
    if (word(m, 12) != 1) { problem("engine started on a message that is not a decode"); return; }
    stream_handles.push_back(word(m, 16));
    ++decodes;
    const uint32_t k = session_decodes++;
    if (word(m, 0) != 40 || word(m, 4) != 244 || word(m, 8) != 2 || word(m, 24) != 2 || word(m, 28) != 56 || word(m, 32) != 188 || word(m, 40) != 0x11 || word(m, 44) != 244 ||
        word(m, 48) != 3576)
      problem("bad decode header");
    if (word(m, 20) != k + 1) problem("feedback number is not the picture count");
    const uint32_t dec = 56, av1 = 244;
    const uint32_t width = word(m, dec + 8), height = word(m, dec + 12), bsd = word(m, dec + 16), dpb = word(m, dec + 20), dt_size = word(m, dec + 24), ctx = word(m, dec + 36);
    if (word(m, dec) != 0x13 || !width || !height) problem("bad decode message");
    if (word(m, dec + 40) != 128 * 1024) problem("session context size");
    const uint64_t fb = buffer[3], prob = buffer[4];
    if (fb != msg_addr + 0x1000 || prob != msg_addr + 0x1800) problem("feedback / segmentation table not where Mesa puts them");
    const Range ranges[] = {{"message", msg_addr, 0x1800 + 20736}, {"session", buffer[5], 128 * 1024}, {"dpb", buffer[1], dpb}, {"context", buffer[0x206], ctx},
                            {"bitstream", buffer[0x100], bsd}, {"target", buffer[2], dt_size}};
    for (size_t i = 0; i < 6; ++i) {
      if (!ranges[i].size || !vcn.translate(ranges[i].start, ranges[i].size)) { char t[80]; snprintf(t, sizeof t, "%s buffer outside the work memory", ranges[i].name); problem(t); }
      for (size_t j = i + 1; j < 6; ++j)
        if (ranges[i].start < ranges[j].start + ranges[j].size && ranges[j].start < ranges[i].start + ranges[i].size) { char t[80]; snprintf(t, sizeof t, "%s and %s overlap", ranges[i].name, ranges[j].name); problem(t); }
    }
    // The context buffer starts with four initial contexts of 22528 bytes each: the first bytes are not zero, the padding after a context's 22240 bytes is.
    const uint8_t* c = vcn.translate(buffer[0x206], 4 * 22528);
    if (c) {
      bool nonzero = false;
      for (uint32_t i = 0; i < 64; ++i) nonzero = nonzero || c[i] != 0;
      if (!nonzero) problem("context buffer does not start with the initial contexts");
      for (uint32_t ctx_i = 0; ctx_i < 4; ++ctx_i)
        for (uint32_t i = 22240; i < 22528; ++i) if (c[ctx_i * 22528 + i] != 0) { problem("padding of an initial context is not zero"); ctx_i = 4; break; }
    }
    // Slots: the frame goes into one that no reference slot of the message holds; the references it names must hold the frames the stream says.
    const uint32_t cur_idx = m[av1 + 16];
    if (cur_idx >= 9) problem("current picture slot out of range");
    for (uint32_t i = 0; i < 8; ++i) {
      const uint32_t s = m[av1 + 602 + i];
      if (s != 0x7F && s == cur_idx) problem("the current picture's slot is still referenced");
      if (s != 0x7F && s >= 9) problem("reference slot out of range");
    }
    if (reference && check_content && k < reference->frames.size()) {
      const FrameRecord& r = reference->frames[k];
      if (word(m, av1 + 8) != uint32_t(r.frame_offset) || m[av1 + 14] != r.frame_type || m[av1 + 34] != r.base_qindex || m[av1 + 45] != r.tile_cols || m[av1 + 46] != r.tile_rows ||
          word(m, av1 + 588) != uint32_t(r.width) || word(m, av1 + 592) != uint32_t(r.height) || word(m, av1 + 596) != uint32_t(r.width) || word(m, dec + 8) != uint32_t(r.width))
        problem("frame fields differ from the CPU decoder's");
      if (m[av1 + 601] == 0 || m[av1 + 600] != 8) problem("order hint bits / superres denominator");
      if (word(m, av1 + 52) != 0xFF) problem("tile size bytes");
      if (r.inter)
        for (uint32_t i = 0; i < 7; ++i) {
          const uint32_t s = m[av1 + 644 + i];
          if (s >= 9 || !slots[s].valid || slots[s].frame_offset != r.expected_ref_offset[i]) problem("a reference picture is not in the slot the message names");
        }
      // The bitstream: every tile back to back, then zeros up to bsd_size; the message says where each tile starts and how long it is.
      const uint8_t* bits = vcn.translate(buffer[0x100], bsd);
      if (bits && (r.tiles.size() > bsd || bsd % 128 != 0 || memcmp(bits, r.tiles.data(), r.tiles.size()) != 0)) problem("bitstream differs from the frame's tiles");
      for (uint64_t i = r.tiles.size(); bits && i < bsd; ++i) if (bits[i] != 0) { problem("bitstream padding is not zero"); break; }
      uint32_t offset = 0;
      for (size_t t = 0; t < r.tile_sizes.size() && t < 256; ++t) {
        if (word(m, av1 + 1528 + t * 8) != offset || word(m, av1 + 1528 + t * 8 + 4) != r.tile_sizes[t]) { problem("tile table"); break; }
        offset += r.tile_sizes[t];
      }
      if (cur_idx < 16) slots[cur_idx] = {true, r.frame_offset};
    }
    const uint8_t* f = vcn.translate(fb, 44);
    if (!f || word(f, 0) != 44 || word(f, 4) != 44 || word(f, 8) != 0) problem("feedback header not initialised");
    // "Decode": the CPU decoder's picture of the same frame, NV12, linear, with the pitch and chroma offset the message names.
    const uint32_t pitch = word(m, dec + 100), uv_pitch_texels = word(m, dec + 104), chroma_offset = word(m, dec + 144);
    if (word(m, dec + 116) != 0 || word(m, dec + 112) != 0) problem("target is not linear");
    if (reference && write_pictures && k < reference->frames.size()) {
      const FrameRecord& r = reference->frames[k];
      uint8_t* target = vcn.translate(buffer[2], dt_size);
      const uint32_t w = r.width, h = r.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
      if (!target || r.nv12_luma.size() != size_t(w) * h || uint64_t{pitch} * h > chroma_offset || chroma_offset + uint64_t{pitch} * ch > dt_size || uv_pitch_texels * 2 != pitch || cw * 2 > pitch) {
        problem("target layout does not fit");
        return;
      }
      memset(target, 0, dt_size);
      for (uint32_t y = 0; y < h; ++y) memcpy(target + uint64_t{pitch} * y, r.nv12_luma.data() + uint64_t{y} * w, w);
      for (uint32_t y = 0; y < ch; ++y) memcpy(target + chroma_offset + uint64_t{pitch} * y, r.nv12_chroma.data() + uint64_t{y} * cw * 2, cw * 2);
    }
  }
};

static DecoderConfig i420_config() {
  DecoderConfig c;
  c.codec = VideoCodec::Av1;
  c.output = PixelFormat::I420;
  return c;
}

static AmdVcnAv1Decoder::Engine engine_of(Rig& rig, uint32_t handle = 7) {
  AmdVcnAv1Decoder::Engine e;
  e.ring = rig.ring.get();
  e.memory = &rig.memory;
  e.stream_handle = handle;
  e.job_timeout_ms = 20;
  return e;
}

// A stream through the engine's session. `on_engine`: the engine takes it (else the CPU decodes it whole and the engine is never touched).
static void compare_stream(const char* name, bool on_engine) {
  printf("  %s\n", name);
  TestAllocator a;
  {
    Reference ref;
    CHECK(make_reference_file(a, name, &ref));
    if (ref.ends.empty()) return;
    printf("    %zu temporal units, %zu frames decoded, %zu pictures out\n", ref.ends.size(), ref.frames.size(), ref.out.pts.size());
    Rig rig;
    rig.reference = &ref;
    {
      AmdVcnAv1Decoder session(a, engine_of(rig), i420_config());
      CHECK(session.prepare());
      Output out;
      for (size_t k = 0; k < ref.ends.size(); ++k) CHECK(feed(session, ref.unit(k), int64_t(k) * 1000, &out));
      session.flush();
      drain(session, &out);
      CHECK(session.statistics().errors == 0 && session.statistics().frames == ref.out.pts.size());
      CHECK(rig.problems == 0 && rig.vcn.protocol_errors == 0);
      if (on_engine) {
        CHECK(rig.creates == 1 && rig.decodes == int(ref.frames.size()) && session.frames_decoded() == ref.frames.size());
      } else {
        if (rig.creates || rig.decodes) printf("    engine used: %d creates, %d decodes\n", rig.creates, rig.decodes);
        CHECK(rig.creates == 0 && rig.decodes == 0 && rig.vcn.writes.empty() && !session.session_open());
      }
      // The same pictures, in the same order, byte for byte.
      CHECK(out.pts == ref.out.pts);
      for (size_t i = 0; i < out.bytes.size() && i < ref.out.pts.size(); ++i) {
        CHECK(out.format[i] == ref.out.format[i]);
        CHECK(out.bytes[i] == ref.out.bytes[i]);
      }
    }
    CHECK(rig.destroys == (on_engine ? 1 : 0) && rig.problems == 0);
    CHECK(rig.memory.free_bytes() == Rig::kArena - 2 * 4096);
  }
  CHECK(a.live == 0);
}

// The engine's own AV1 set-up and self-test, against the same simulated card: the stand-in firmware is given the CPU's frames for the self-test stream.
static void test_engine_self_test(bool engine_works, bool engine_hangs) {
  TestAllocator a;
  {
    Reference ref;
    ref.stream.assign(kAv1SelfTestStream, kAv1SelfTestStream + sizeof kAv1SelfTestStream);
    ref.ends.assign(kAv1SelfTestEnds, kAv1SelfTestEnds + kAv1SelfTestPictures);
    CHECK(make_reference(a, &ref) && ref.out.pts.size() == kAv1SelfTestPictures);

    Rig rig;
    rig.reference = &ref;
    rig.write_pictures = engine_works;
    rig.vcn.stall_fence = engine_hangs;
    AmdVcnVideoEngine engine(a, 0x1681);   // a Rembrandt: it has an AV1 decoder
    const MmioRegion ring_window(reinterpret_cast<uint64_t>(rig.ring_page.data()), rig.ring_page.size());
    CHECK(engine.set_up_decode(rig.vcn, rig.clock, ring_window, rig.mapper, Rig::kGpuBase, Rig::kPhysicalBase, rig.arena.size()) == nullptr);
    const bool enabled = engine.enable_av1_decode();
    CHECK(enabled == (engine_works && !engine_hangs) && engine.av1_ready() == enabled);
    CHECK(engine.info().accelerated_decode_codecs == (enabled ? codec_mask(VideoCodec::Av1) : 0u));
    CHECK(rig.problems == 0 || !engine_works);
    if (enabled) {
      CHECK(rig.creates == 1 && rig.destroys == 1 && rig.decodes == int(ref.frames.size()));   // the test opened and closed its own session
      UniquePointer<IVideoDecoder> session = engine.create_decoder(i420_config());
      CHECK(static_cast<bool>(session));
      DecoderConfig vp9;
      vp9.codec = VideoCodec::Vp9;
      UniquePointer<IVideoDecoder> software = engine.create_decoder(vp9);
      CHECK(static_cast<bool>(software));
      Output out;
      CHECK(feed(*session, ref.unit(0), 0, &out));
      CHECK(rig.creates == 2 && rig.decodes == int(ref.frames.size()) + 1 && rig.problems == 0);   // it ran on the engine
      // The engine serves one session at a time: while the first is open, a second one is the CPU's.
      UniquePointer<IVideoDecoder> second = engine.create_decoder(i420_config());
      CHECK(static_cast<bool>(second));
      CHECK(feed(*second, ref.unit(0), 0, &out));
      CHECK(rig.creates == 2 && rig.decodes == int(ref.frames.size()) + 1);
      session.reset();
      second.reset();
      software.reset();
      engine.shutdown();
      CHECK(!engine.av1_ready() && engine.info().accelerated_decode_codecs == 0);
    } else {
      UniquePointer<IVideoDecoder> session = engine.create_decoder(i420_config());
      CHECK(static_cast<bool>(session));
      CHECK(strstr(engine.engine_note(), engine_hangs ? "engine stopped" : "AV1 self-test failed") != nullptr);
    }
  }
  CHECK(a.live == 0);
}

static void test_one_session_at_a_time() {
  TestAllocator a;
  {
    Rig rig;
    AmdVcnAv1Decoder first(a, engine_of(rig), i420_config());
    CHECK(first.prepare());
    AmdVcnAv1Decoder second(a, engine_of(rig, 8), i420_config());
    CHECK(!second.prepare());
  }
  {
    Rig rig;
    DecoderConfig junk = i420_config();
    const uint8_t bytes[4] = {'j', 'u', 'n', 'k'};
    junk.codec_config = ConstByteView(bytes, 4);
    AmdVcnAv1Decoder bad(a, engine_of(rig), junk);
    CHECK(!bad.prepare());   // not an av1C record
  }
  CHECK(a.live == 0);
}

static void test_wedged_engine() {
  TestAllocator a;
  {
    Reference ref;
    CHECK(make_reference_file(a, "inter", &ref) && ref.ends.size() > 3);
    Rig rig;
    rig.reference = &ref;
    AmdVcnAv1Decoder session(a, engine_of(rig), i420_config());
    CHECK(session.prepare());
    Output out;
    CHECK(feed(session, ref.unit(0), 0, &out));
    CHECK(rig.decodes == 1 && !rig.ring->wedged());
    // The engine stops answering: the next frame times out and is shown grey; the frames after it are grey too, without the engine being touched again.
    rig.vcn.stall_fence = true;
    CHECK(feed(session, ref.unit(1), 1000, &out));
    CHECK(rig.ring->wedged() && session.statistics().errors >= 1);
    const size_t writes = rig.vcn.writes.size();
    CHECK(feed(session, ref.unit(2), 2000, &out));
    CHECK(rig.vcn.writes.size() == writes);
    drain(session, &out);
    CHECK(out.bytes.size() >= 3 && out.bytes[0] == ref.out.bytes[0]);
    if (out.bytes.size() >= 3) CHECK(out.bytes[1][0] == 128 && out.bytes[1].back() == 128 && out.bytes[2][0] == 128);
  }
  CHECK(a.live == 0);
}

// Damaged streams: units cut short and bits flipped. The session must stay inside its buffers (the sanitizers say so), never hang, and give everything back.
static void test_damaged_streams() {
  TestAllocator a;
  {
    Reference ref;
    ref.stream.assign(kAv1SelfTestStream, kAv1SelfTestStream + sizeof kAv1SelfTestStream);
    ref.ends.assign(kAv1SelfTestEnds, kAv1SelfTestEnds + kAv1SelfTestPictures);
    CHECK(make_reference(a, &ref));
    int runs = 0;
    for (size_t variant = 0; variant < 24; ++variant) {
      Rig rig;
      rig.reference = &ref;
      rig.check_content = false;
      {
        AmdVcnAv1Decoder session(a, engine_of(rig), i420_config());
        CHECK(session.prepare());
        Output out;
        for (size_t k = 0; k < ref.ends.size(); ++k) {
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
      CHECK(rig.vcn.protocol_errors == 0);
    }
    CHECK(runs > 100);
  }
  CHECK(a.live == 0);
}

int main() {
  // Streams the engine takes: 8-bit 4:2:0 without film grain or super-resolution.
  static const char* const taken[] = {"intra", "inter", "lossless", "qm", "rav1e", "sb128", "screen", "segments", "svt", "tiles", "resize"};
  // Streams the CPU keeps: 10- and 12-bit, 4:2:2, 4:4:4, monochrome, film grain, super-resolution.
  static const char* const kept[] = {"10bit", "12bit", "422", "444", "mono", "grain", "superres"};
  for (const char* name : taken) compare_stream(name, true);
  for (const char* name : kept) compare_stream(name, false);
  test_engine_self_test(true, false);
  test_engine_self_test(false, false);
  test_engine_self_test(true, true);
  test_one_session_at_a_time();
  test_wedged_engine();
  test_damaged_streams();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("AmdVcnAv1DecoderTest passed\n");
  return 0;
}
