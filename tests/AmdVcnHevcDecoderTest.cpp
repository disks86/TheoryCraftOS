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


// Host-side tests for the VCN H.265 decode session (AmdVcnHevcDecoder) against a simulated engine: the ring controller of FakeAmdVcn.hpp and a stand-in for the engine's
// firmware. The stand-in reads the messages the way the firmware does (offsets written out here by hand from Mesa's structures), checks that the buffers it is given are
// inside the work memory and do not overlap, that the bitstream holds the picture's slices behind start codes, and that the pictures the message names as references
// are the ones it holds in the slots the message names; then it "decodes" by writing the picture a separate CPU decoder made for the same access unit into the target
// surface, as linear NV12. The session must hand back exactly that decoder's pictures, in the same order, and the stream's own picture hashes must confirm them.
//
// This does not show that the real engine decodes anything: the message contents the stand-in checks are the contents Mesa sends, as far as they are known.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <memory>
#include <vector>
#include "Crypto/Crypto.hpp"
#include "FakeAmdVcn.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnHevcDecoder.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcnVideoEngine.hpp"
#include "Hardware/VideoCodec/Software/H265.hpp"

using namespace hardware;
using namespace fakevcn;

// The stream the engine's self-test decodes (kHevcSelfTestStream, kHevcSelfTestEnds, ...).
namespace {
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestHevcStream.inc"
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

struct Md5Hasher final : IPlaneHasher {
  void md5(const uint8_t* plane, uint32_t width, uint32_t height, uint32_t stride, uint8_t out[16]) override {
    crypto::Hasher h(crypto::HashKind::Md5);
    for (uint32_t y = 0; y < height; ++y) h.update(ConstByteView(plane + size_t(y) * stride, width));
    h.finish(out);
  }
};
static Md5Hasher g_md5;

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

// Splits an Annex B stream into units that each end before the next picture's first slice (the parameter sets in front of a picture go with the picture before it).
static std::vector<std::pair<size_t, size_t>> split_pictures(const std::vector<uint8_t>& d) {
  std::vector<size_t> starts;
  for (size_t i = 0; i + 3 <= d.size(); ++i)
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) { starts.push_back(i); i += 2; }
  std::vector<std::pair<size_t, size_t>> units;
  size_t unit_begin = 0;
  bool have_slice = false;
  for (size_t k = 0; k < starts.size(); ++k) {
    const size_t at = starts[k];
    if (at + 5 > d.size()) break;
    const int type = (d[at + 3] >> 1) & 0x3F;
    const bool slice = type < 32;
    const bool first = slice && (d[at + 5] & 0x80);
    if (first && have_slice) {
      size_t cut = at;
      size_t j = k;
      while (j > 0) {
        const int pt = (d[starts[j - 1] + 3] >> 1) & 0x3F;
        if (pt >= 32 && pt <= 39) { cut = starts[j - 1]; --j; } else break;
      }
      if (cut > unit_begin) units.push_back({unit_begin, cut});
      unit_begin = cut;
      have_slice = false;
    }
    if (slice) have_slice = true;
  }
  units.push_back({unit_begin, d.size()});
  return units;
}

// The slice NAL units of an access unit (header bytes on, trailing zero bytes off): what the engine's bitstream must hold behind start codes.
static std::vector<std::vector<uint8_t>> slice_nals(ConstByteView au) {
  std::vector<std::vector<uint8_t>> nals;
  for (uint64_t i = 0; i + 3 <= au.size();) {
    if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
      uint64_t j = i + 3;
      while (j + 3 <= au.size() && !(au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 1)) ++j;
      uint64_t end = j + 3 <= au.size() ? j : au.size();
      while (end > i + 3 && au[end - 1] == 0) --end;
      const int type = (au[i + 3] >> 1) & 0x3F;
      if (type < 32) nals.emplace_back(au.data() + i + 3, au.data() + end);
      i = j;
    } else {
      ++i;
    }
  }
  return nals;
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

// The CPU decoder's view of a stream: its units, and its pictures by presentation time.
struct Reference {
  std::vector<uint8_t> file;
  std::vector<std::pair<size_t, size_t>> units;
  Output out;
  std::map<int64_t, size_t> index;   // pts -> position in out
  ConstByteView unit(size_t k) const { return ConstByteView(file.data() + units[k].first, units[k].second - units[k].first); }
};

static bool make_reference_from(allocators::IAllocator& a, Reference* ref) {
  if (ref->file.empty()) return false;
  ref->units = split_pictures(ref->file);
  H265Decoder dec(a);
  dec.set_plane_hasher(&g_md5);
  VideoFrame f;
  auto take = [&] {
    while (dec.receive(&f)) {
      std::vector<uint8_t> bytes;
      for (uint32_t p = 0; p < 3; ++p)
        for (uint32_t y = 0; y < f.format.plane_height(p); ++y) bytes.insert(bytes.end(), f.plane[p] + uint64_t{y} * f.stride[p], f.plane[p] + uint64_t{y} * f.stride[p] + f.format.plane_width(p));
      ref->index[f.pts_us] = ref->out.pts.size();
      ref->out.pts.push_back(f.pts_us);
      ref->out.bytes.push_back(bytes);
      ref->out.format.push_back(f.format);
    }
  };
  for (size_t k = 0; k < ref->units.size(); ++k) {
    dec.decode(ref->unit(k), int64_t(k) * 1000);
    take();
  }
  dec.flush();
  take();
  const auto stats = dec.hash_statistics();
  return !ref->out.pts.empty() && stats.checked == ref->out.pts.size() && stats.mismatched == 0;
}

static bool make_reference(allocators::IAllocator& a, const char* name, Reference* ref) {
  char path[160];
  snprintf(path, sizeof path, "tests/data/hevc_%s.h265", name);
  ref->file = read_file(path);
  return make_reference_from(a, ref);
}

// Plain memory standing in for the card: the work memory the session allocates from, the ring page, and the engine around them.
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
  static constexpr uint64_t kArena = 40ull << 20;
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
  bool check_content = true;       // compare the bitstream with the units' slices (off for damaged streams)
  std::map<uint32_t, uint64_t> buffer;
  int creates = 0, destroys = 0, decodes = 0, problems = 0;
  uint32_t session_decodes = 0;    // pictures decoded since the last create: the stream's own count
  std::vector<uint32_t> stream_handles;
  // The engine's decoded picture buffer, as far as the message lets one tell: which order count lives in which slot.
  struct Slot { bool valid = false; int32_t poc = 0; };
  Slot slots[32];

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
      if (!c || word(c, 0) != 0x10) problem("create: stream type is not H265");
    } else if (type == 2) {
      ++destroys;
      if (word(m, 0) != 40 || word(m, 8) != 0) problem("bad destroy header");
    }
  }

  struct Range { const char* name; uint64_t start, size; };

  void on_engine() {
    const uint64_t msg_addr = buffer[0];
    constexpr uint32_t kMessageBytes = 244 + 276;
    uint8_t m[kMessageBytes];
    if (!vcn.translate(msg_addr, sizeof m)) { problem("decode message outside the work memory"); return; }
    memcpy(m, vcn.translate(msg_addr, sizeof m), sizeof m);
    if (word(m, 12) != 1) { problem("engine started on a message that is not a decode"); return; }
    stream_handles.push_back(word(m, 16));
    ++decodes;
    const uint32_t k = session_decodes++;
    // Header and indexes: the decode message at 56 (188 bytes), the HEVC message (id 13) at 244 (276 bytes).
    if (word(m, 0) != 40 || word(m, 4) != 244 || word(m, 8) != 2 || word(m, 24) != 2 || word(m, 28) != 56 || word(m, 32) != 188 || word(m, 40) != 13 || word(m, 44) != 244 ||
        word(m, 48) != 276)
      problem("bad decode header");
    if (word(m, 20) != k + 1) problem("feedback number is not the picture count");
    const uint32_t dec = 56, hevc = 244;
    const uint32_t width = word(m, dec + 8), height = word(m, dec + 12), bsd = word(m, dec + 16), dpb = word(m, dec + 20), dt_size = word(m, dec + 24), ctx = word(m, dec + 36);
    if (word(m, dec) != 0x10 || width % 8 || height % 8 || !width || !height) problem("bad decode message");
    if (word(m, dec + 40) != 128 * 1024) problem("session context size");
    // Every buffer is inside the work memory, and none overlaps another.
    const uint64_t fb = buffer[3], it = buffer[0x204];
    if (fb != msg_addr + 0x1000 || it != msg_addr + 0x1800) problem("feedback / scaling table not where Mesa puts them");
    const Range ranges[] = {{"message", msg_addr, 0x1800 + 992}, {"session", buffer[5], 128 * 1024}, {"dpb", buffer[1], dpb}, {"context", buffer[0x206], ctx},
                            {"bitstream", buffer[0x100], bsd}, {"target", buffer[2], dt_size}};
    for (size_t i = 0; i < 6; ++i) {
      if (!ranges[i].size || !vcn.translate(ranges[i].start, ranges[i].size)) { char t[80]; snprintf(t, sizeof t, "%s buffer outside the work memory", ranges[i].name); problem(t); }
      for (size_t j = i + 1; j < 6; ++j)
        if (ranges[i].start < ranges[j].start + ranges[j].size && ranges[j].start < ranges[i].start + ranges[i].size) { char t[80]; snprintf(t, sizeof t, "%s and %s overlap", ranges[i].name, ranges[j].name); problem(t); }
    }
    // The picture buffer: as many pictures as the buffers were sized for, 17.
    uint64_t image = ((uint64_t(width + 15) / 16 * 16 + 31) / 32 * 32) * ((height + 15) / 16 * 16) * 3 / 2;
    image = (image + 255) / 256 * 256;
    if (dpb < image * 17 || dpb >= image * 17 + 4096) problem("dpb size is not 17 pictures (to the page)");
    // The reference pictures: each one the message names must be in the slot it says, with the order count it says.
    const uint32_t cur_idx = m[hevc + 118];
    const int32_t cur_poc = int32_t(word(m, hevc + 120));
    if (cur_idx >= 17) problem("current slot out of range");
    const uint32_t sets[3] = {204, 212, 220};
    for (uint32_t set = 0; set < 3; ++set)
      for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t e = m[hevc + sets[set] + i];
        if (e == 0xFF) continue;
        if (e >= 16) { problem("reference set entry out of range"); continue; }
        const uint32_t slot = m[hevc + 124 + e];
        const int32_t poc = int32_t(word(m, hevc + 140 + e * 4));
        if (check_content && (slot >= 17 || !slots[slot].valid || slots[slot].poc != poc)) problem("a reference picture is not in the slot the message names");
      }
    for (uint32_t l = 0; l < 2; ++l)
      for (uint32_t i = 0; i < 15; ++i) {
        const uint32_t e = m[hevc + 244 + l * 15 + i];
        if (e != 0xFF && (e >= 16 || m[hevc + 124 + e] == 0x7F)) problem("reference list entry names no picture");
      }
    if (cur_idx < 32) slots[cur_idx] = {true, cur_poc};
    // The bitstream: every slice behind a start code, then zeros up to bsd_size.
    if (reference && check_content && k < reference->units.size()) {
      std::vector<uint8_t> expected;
      for (const auto& nal : slice_nals(reference->unit(k))) { expected.push_back(0); expected.push_back(0); expected.push_back(1); expected.insert(expected.end(), nal.begin(), nal.end()); }
      const uint8_t* bits = vcn.translate(buffer[0x100], bsd);
      if (bits && (expected.size() > bsd || bsd % 128 != 0 || memcmp(bits, expected.data(), expected.size()) != 0)) problem("bitstream differs from the access unit's slices");
      for (uint64_t i = expected.size(); bits && i < bsd; ++i) if (bits[i] != 0) { problem("bitstream padding is not zero"); break; }
    }
    const uint8_t* f = vcn.translate(fb, 44);
    if (!f || word(f, 0) != 44 || word(f, 4) != 44 || word(f, 8) != 0) problem("feedback header not initialised");
    // "Decode": the CPU decoder's picture of the same decode-order position, NV12, linear, with the pitch and chroma offset the message names. (The CPU's pictures are
    // cropped to the conformance window, which x265 puts at the right and bottom, so the picture goes into the corner of the surface.)
    const uint32_t pitch = word(m, dec + 100), uv_pitch_texels = word(m, dec + 104), chroma_offset = word(m, dec + 144);
    if (word(m, dec + 116) != 0 || word(m, dec + 112) != 0) problem("target is not linear");
    if (reference && write_pictures && k < reference->units.size()) {
      auto it = reference->index.find(int64_t{k} * 1000);
      uint8_t* target = vcn.translate(buffer[2], dt_size);
      if (it == reference->index.end() || !target) { problem("no CPU picture to stand in for the decode"); return; }
      const std::vector<uint8_t>& yuv = reference->out.bytes[it->second];
      const VideoFormat& fmt = reference->out.format[it->second];
      const uint32_t cw = fmt.chroma_width(), ch = fmt.chroma_height();
      if (fmt.width > width || fmt.height > height || uint64_t{pitch} * height > chroma_offset || chroma_offset + uint64_t{pitch} * (height / 2) > dt_size || uv_pitch_texels * 2 != pitch) {
        problem("target layout does not fit");
        return;
      }
      memset(target, 0, dt_size);
      for (uint32_t y = 0; y < fmt.height; ++y) memcpy(target + uint64_t{pitch} * y, yuv.data() + uint64_t{y} * fmt.width, fmt.width);
      const uint8_t* u = yuv.data() + uint64_t{fmt.width} * fmt.height;
      const uint8_t* v = u + uint64_t{cw} * ch;
      for (uint32_t y = 0; y < ch; ++y)
        for (uint32_t x = 0; x < cw; ++x) {
          target[chroma_offset + uint64_t{pitch} * y + 2 * x] = u[uint64_t{y} * cw + x];
          target[chroma_offset + uint64_t{pitch} * y + 2 * x + 1] = v[uint64_t{y} * cw + x];
        }
    }
  }
};

static DecoderConfig i420_config() {
  DecoderConfig c;
  c.codec = VideoCodec::Hevc;
  c.output = PixelFormat::I420;
  return c;
}

static AmdVcnHevcDecoder::Engine engine_of(Rig& rig, uint32_t handle = 7) {
  AmdVcnHevcDecoder::Engine e;
  e.ring = rig.ring.get();
  e.memory = &rig.memory;
  e.stream_handle = handle;
  e.job_timeout_ms = 20;
  return e;
}

static void compare_stream(const char* name) {
  printf("  %s\n", name);
  TestAllocator a;
  {
    Reference ref;
    CHECK(make_reference(a, name, &ref));
    if (ref.units.empty()) return;
    printf("    %zu pictures in, %zu out\n", ref.units.size(), ref.out.pts.size());
    Rig rig;
    rig.reference = &ref;
    {
      AmdVcnHevcDecoder session(a, engine_of(rig), i420_config());
      session.set_plane_hasher(&g_md5);
      CHECK(session.prepare());
      Output out;
      for (size_t k = 0; k < ref.units.size(); ++k) CHECK(feed(session, ref.unit(k), int64_t(k) * 1000, &out));
      session.flush();
      drain(session, &out);
      CHECK(session.statistics().errors == 0 && session.statistics().frames == ref.out.pts.size());
      CHECK(rig.problems == 0 && rig.vcn.protocol_errors == 0);
      CHECK(rig.creates == 1 && rig.decodes == int(ref.units.size()) && rig.destroys == 0 && session.pictures_decoded() == ref.units.size());
      // The same pictures, in the same order, byte for byte; and the stream's own hashes agree with every one of the engine's pictures.
      CHECK(out.pts == ref.out.pts);
      for (size_t i = 0; i < out.bytes.size() && i < ref.out.pts.size(); ++i) {
        CHECK(out.format[i] == ref.out.format[i]);
        CHECK(out.bytes[i] == ref.out.bytes[i]);
      }
      CHECK(session.hash_statistics().checked == ref.units.size() && session.hash_statistics().mismatched == 0);
      CHECK(!out.format.empty() && session.output_format().width == out.format.front().width && session.output_format().height == out.format.front().height);
    }
    CHECK(rig.destroys == 1 && rig.problems == 0);
    CHECK(rig.stream_handles.front() == 7 && rig.stream_handles.back() == 7);
    CHECK(rig.memory.free_bytes() == Rig::kArena - 2 * 4096);
  }
  CHECK(a.live == 0);
}

// A work memory too small for the sequence: the CPU decodes it whole, and the engine is never touched.
static void test_cpu_fallback() {
  TestAllocator a;
  {
    Reference ref;
    CHECK(make_reference(a, "bframes", &ref));
    Rig rig(256 * 1024);
    AmdVcnHevcDecoder session(a, engine_of(rig), i420_config());
    CHECK(session.prepare());
    Output out;
    for (size_t k = 0; k < ref.units.size(); ++k) CHECK(feed(session, ref.unit(k), int64_t(k) * 1000, &out));
    session.flush();
    drain(session, &out);
    CHECK(out.pts == ref.out.pts && out.bytes == ref.out.bytes);
    CHECK(rig.creates == 0 && rig.decodes == 0 && rig.vcn.writes.empty() && !session.session_open() && session.statistics().errors == 0);
  }
  CHECK(a.live == 0);
}

static void test_one_session_at_a_time() {
  TestAllocator a;
  {
    Rig rig;
    AmdVcnHevcDecoder first(a, engine_of(rig), i420_config());
    CHECK(first.prepare());
    AmdVcnHevcDecoder second(a, engine_of(rig, 8), i420_config());
    CHECK(!second.prepare());
  }
  {
    Rig rig;
    DecoderConfig junk = i420_config();
    const uint8_t bytes[4] = {'j', 'u', 'n', 'k'};
    junk.codec_config = ConstByteView(bytes, 4);
    AmdVcnHevcDecoder bad(a, engine_of(rig), junk);
    CHECK(!bad.prepare());   // not an hvcC record
  }
  CHECK(a.live == 0);
}

static void test_wedged_engine() {
  TestAllocator a;
  {
    Reference ref;
    CHECK(make_reference(a, "ipp", &ref));
    Rig rig;
    rig.reference = &ref;
    AmdVcnHevcDecoder session(a, engine_of(rig), i420_config());
    CHECK(session.prepare());
    Output out;
    CHECK(feed(session, ref.unit(0), 0, &out));
    CHECK(rig.decodes == 1 && !rig.ring->wedged());
    // The engine stops answering: the next picture times out and is shown grey; the pictures after it are grey too, without the engine being touched again.
    rig.vcn.stall_fence = true;
    CHECK(feed(session, ref.unit(1), 1000, &out));
    CHECK(rig.ring->wedged() && session.statistics().errors >= 1);
    const size_t writes = rig.vcn.writes.size();
    CHECK(feed(session, ref.unit(2), 2000, &out));
    CHECK(rig.vcn.writes.size() == writes);
    session.flush();
    drain(session, &out);
    CHECK(out.bytes.size() >= 3 && out.bytes[0] == ref.out.bytes[0]);
    if (out.bytes.size() >= 3) {
      CHECK(out.bytes[1][0] == 128 && out.bytes[1].back() == 128 && out.bytes[2][0] == 128);
    }
  }
  CHECK(a.live == 0);
}

// Damaged streams: units cut short and bits flipped. The session must stay inside its buffers (the sanitizers say so), never hang, and give everything back.
static void test_damaged_streams() {
  TestAllocator a;
  {
    Reference ref;
    CHECK(make_reference(a, "bframes", &ref));
    int runs = 0;
    for (size_t variant = 0; variant < 24; ++variant) {
      Rig rig;
      rig.reference = &ref;
      rig.check_content = false;
      {
        AmdVcnHevcDecoder session(a, engine_of(rig), i420_config());
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
      CHECK(rig.vcn.protocol_errors == 0);
    }
    CHECK(runs > 100);
  }
  CHECK(a.live == 0);
}

// The engine's own H.265 set-up and self-test, against the same simulated card: the stand-in firmware is given the CPU's pictures for the self-test stream.
static void test_engine_self_test(bool engine_works, bool engine_hangs) {
  TestAllocator a;
  {
    Reference ref;
    ref.file.assign(kHevcSelfTestStream, kHevcSelfTestStream + sizeof kHevcSelfTestStream);
    CHECK(make_reference_from(a, &ref));
    CHECK(ref.units.size() == kHevcSelfTestPictures);
    for (size_t k = 0; k < ref.units.size() && k < kHevcSelfTestPictures; ++k) CHECK(ref.units[k].second == kHevcSelfTestEnds[k]);   // the same split the test makes

    Rig rig;
    rig.reference = &ref;
    rig.write_pictures = engine_works;
    rig.vcn.stall_fence = engine_hangs;
    AmdVcnVideoEngine engine(a, 0x15DD);
    const MmioRegion ring_window(reinterpret_cast<uint64_t>(rig.ring_page.data()), rig.ring_page.size());
    CHECK(engine.set_up_decode(rig.vcn, rig.clock, ring_window, rig.mapper, Rig::kGpuBase, Rig::kPhysicalBase, rig.arena.size()) == nullptr);
    const bool enabled = engine.enable_hevc_decode();
    CHECK(enabled == (engine_works && !engine_hangs) && engine.hevc_ready() == enabled);
    CHECK(engine.info().accelerated_decode_codecs == (enabled ? codec_mask(VideoCodec::Hevc) : 0u));
    CHECK(rig.problems == 0 || !engine_works);
    if (enabled) {
      CHECK(rig.creates == 1 && rig.destroys == 1 && rig.decodes == int(kHevcSelfTestPictures));   // the test opened and closed its own session
      // H.265 sessions are the engine's now; other codecs stay with the CPU.
      UniquePointer<IVideoDecoder> session = engine.create_decoder(i420_config());
      CHECK(static_cast<bool>(session));
      DecoderConfig vp9;
      vp9.codec = VideoCodec::Vp9;
      UniquePointer<IVideoDecoder> software = engine.create_decoder(vp9);
      CHECK(static_cast<bool>(software));
      Output out;
      CHECK(feed(*session, ref.unit(0), 0, &out));
      CHECK(rig.creates == 2 && rig.decodes == int(kHevcSelfTestPictures) + 1);   // it ran on the engine
      // The engine serves one session at a time: while the first is open, a second one is the CPU's.
      UniquePointer<IVideoDecoder> second = engine.create_decoder(i420_config());
      CHECK(static_cast<bool>(second));
      CHECK(feed(*second, ref.unit(0), 0, &out));
      CHECK(rig.creates == 2 && rig.decodes == int(kHevcSelfTestPictures) + 1 && rig.problems == 0);
      session.reset();
      second.reset();
      software.reset();
      engine.shutdown();
      CHECK(!engine.hevc_ready() && engine.info().accelerated_decode_codecs == 0);
    } else {
      // Not trusted: nothing accelerated, and H.265 is still served (by the CPU).
      UniquePointer<IVideoDecoder> session = engine.create_decoder(i420_config());
      CHECK(static_cast<bool>(session));
      CHECK(strstr(engine.engine_note(), engine_hangs ? "engine stopped" : "H.265 self-test failed") != nullptr);
    }
  }
  CHECK(a.live == 0);
}

int main() {
  static const char* const streams[] = {"intra", "ipp", "bframes", "ctu64", "scaling", "oddsize", "wpp", "weighted", "slices", "tools", "maxref", "qcif", "cutree_off", "highrate"};
  for (const char* name : streams) compare_stream(name);
  test_engine_self_test(true, false);
  test_engine_self_test(false, false);
  test_engine_self_test(true, true);
  test_cpu_fallback();
  test_one_session_at_a_time();
  test_wedged_engine();
  test_damaged_streams();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("AmdVcnHevcDecoderTest passed\n");
  return 0;
}
