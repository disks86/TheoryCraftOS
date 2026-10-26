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

// Host-side tests for the UVD 6 H.264 decode session (AmdUvd6AvcDecoder) against a simulated engine: the ring controller of FakeAmdUvd6.hpp and a stand-in for the
// engine's firmware. The stand-in reads the message the way the firmware does (offsets written out here by hand from Mesa's structure), checks that the buffers it is given
// are inside the work memory and do not overlap, that the bitstream holds the picture's slices behind start codes, and that the references it is told about include every
// picture the CPU decoder's reference lists use; then it "decodes" by writing the picture the CPU decoder made for the same access unit into the target surface, as
// linear NV12 with the pitch and chroma offset the message names. The session must hand back exactly the CPU decoder's pictures, in the same order.
//
// This does not show that the real engine decodes anything: the message contents the stand-in checks are the contents Mesa sends, as far as they are known.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <memory>
#include <vector>
#include "FakeAmdUvd6.hpp"
#include "FakeAmdVcn.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6AvcDecoder.hpp"
#include "Hardware/VideoCodec/Software/H264.hpp"
#include "Hardware/VideoCodec/Software/SoftwareVideoCodec.hpp"

using namespace hardware;
using fakevcn::FakeClock;

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

// Splits an Annex B stream into access units: a slice with first_mb_in_slice 0, or parameter sets / SEI / a delimiter after a slice, starts one.
static bool next_access_unit(ConstByteView d, uint64_t* pos, ConstByteView* au) {
  auto start_at = [&](uint64_t k) { return k + 3 <= d.size() && d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1; };
  uint64_t i = *pos;
  while (i < d.size() && !start_at(i)) ++i;
  if (i >= d.size()) return false;
  const uint64_t begin = i;
  bool has_slice = false;
  while (i < d.size()) {
    const uint64_t hdr = i + 3;
    if (hdr >= d.size()) { i = d.size(); break; }
    const int t = d[hdr] & 31;
    const bool slice = t == 1 || t == 5;
    const bool first = slice && hdr + 1 < d.size() && (d[hdr + 1] & 0x80);
    if (i != begin && ((has_slice && (t == 6 || t == 7 || t == 8 || t == 9)) || (slice && first && has_slice))) break;
    has_slice = has_slice || slice;
    uint64_t j = hdr + 1;
    while (j < d.size() && !start_at(j)) ++j;
    i = j;
  }
  *au = d.subview(begin, i - begin);
  *pos = i;
  return true;
}

// The slice NAL units of an access unit (header byte on, trailing zero bytes off): what the engine's bitstream must hold behind start codes.
static std::vector<std::vector<uint8_t>> slice_nals(ConstByteView au) {
  std::vector<std::vector<uint8_t>> nals;
  for (uint64_t i = 0; i + 3 <= au.size();) {
    if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
      uint64_t j = i + 3;
      while (j + 3 <= au.size() && !(au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 1)) ++j;
      uint64_t end = j + 3 <= au.size() ? j : au.size();
      while (end > i + 3 && au[end - 1] == 0) --end;
      const int type = au[i + 3] & 31;
      if (type == 1 || type == 5) nals.emplace_back(au.data() + i + 3, au.data() + end);
      i = j;
    } else {
      ++i;
    }
  }
  return nals;
}

// The CPU decoder's view of a stream: its pictures in display order, per decode-order picture the reference pictures its slices used, and its parameter sets' crop.
struct Reference {
  std::map<int64_t, std::vector<uint8_t>> i420;     // by pts: Y, then U, then V
  std::map<int64_t, VideoFormat> format;
  std::vector<int64_t> output_pts;
  std::vector<std::vector<H264Decoder::SliceRefs>> slices;   // per access unit
  std::vector<ConstByteView> aus;
};

static void observe(void* context, const H264Decoder::SliceRefs& refs) {
  auto* r = static_cast<Reference*>(context);
  r->slices.back().push_back(refs);
}

static void take(H264Decoder& dec, Reference* r) {
  VideoFrame f;
  while (dec.receive(&f)) {
    std::vector<uint8_t> bytes;
    for (uint32_t p = 0; p < 3; ++p)
      for (uint32_t y = 0; y < f.format.plane_height(p); ++y) bytes.insert(bytes.end(), f.plane[p] + uint64_t{y} * f.stride[p], f.plane[p] + uint64_t{y} * f.stride[p] + f.format.plane_width(p));
    r->i420[f.pts_us] = bytes;
    r->format[f.pts_us] = f.format;
    r->output_pts.push_back(f.pts_us);
  }
}

static void run_software(allocators::IAllocator& a, ConstByteView stream, Reference* r) {
  H264Decoder dec(a);
  dec.set_slice_observer(observe, r);
  uint64_t pos = 0;
  ConstByteView au;
  int64_t pts = 0;
  while (next_access_unit(stream, &pos, &au)) {
    r->aus.push_back(au);
    r->slices.emplace_back();
    take(dec, r);
    dec.decode(au, pts);
    pts += 1000;
    take(dec, r);
  }
  dec.flush();
  take(dec, r);
}

// Plain memory standing in for the card: the work memory the session allocates from, the ring page, and the engine around them.
static uint32_t word(const uint8_t* p, uint32_t offset) { return p[offset] | (p[offset + 1] << 8) | (p[offset + 2] << 16) | (uint32_t{p[offset + 3]} << 24); }

struct FakeMapper final : IRegisterMapper {
  uint8_t* host = nullptr;
  uint64_t physical_base = 0, size = 0;
  int maps = 0;
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    if (physical < physical_base || length > size || physical - physical_base > size - length) return false;
    ++maps;
    *region = MmioRegion(reinterpret_cast<uint64_t>(host + (physical - physical_base)), length);
    return true;
  }
};

// struct ruvd_msg (Mesa's ac_uvd_dec.h), written out here by hand as dword counts: four header dwords, then the decode message, 36 dwords and 16 reserved, then the H.264 message.
constexpr uint32_t kMsgBytes = 3556;
constexpr uint32_t dec(uint32_t dword) { return 16 + 4 * dword; }
constexpr uint32_t kCodec = dec(52);   // = 224

struct Rig {
  static constexpr uint64_t kArena = 40ull << 20;
  static constexpr uint64_t kGpuBase = 0x4'0000'0000ull, kPhysicalBase = 0xE000'0000ull;
  std::vector<uint8_t> arena, ring_page;
  FakeMapper mapper;
  AmdVcnWorkMemory memory;
  fakeuvd::Ring vcn;
  FakeClock clock;
  std::unique_ptr<AmdUvd6Ring> ring;
  AmdVcnWorkMemory::Piece ib, fence;

  // What the stand-in firmware is checked against / does.
  const Reference* reference = nullptr;
  int crop_x = 0, crop_y = 0;
  bool check_content = true;       // compare the bitstream and the references with the CPU decoder's (off for damaged streams)
  bool write_pictures = true;
  std::map<uint32_t, uint64_t> buffer;
  int creates = 0, destroys = 0, decodes = 0, problems = 0;
  std::vector<uint32_t> stream_handles;
  uint32_t last_dt_pitch = 0;

  explicit Rig(uint64_t arena_bytes = kArena) : arena(arena_bytes), ring_page(AmdUvd6::kRingBytes) {
    mapper.host = arena.data();
    mapper.physical_base = kPhysicalBase;
    mapper.size = arena.size();
    memory.configure(&mapper, kGpuBase, kPhysicalBase, arena.size());
    memory.allocate(4096, 4096, &ib);
    memory.allocate(4096, 4096, &fence);
    memory.keep();
    vcn.windows = {{kGpuBase, arena.data(), arena.size()}};
    // The ring page has its own made-up GPU address.
    vcn.windows.push_back({0x5'0000'0000ull, ring_page.data(), ring_page.size()});
    vcn.ring_gpu = 0x5'0000'0000ull;
    AmdUvd6Ring::Memory m;
    m.ib_gpu_address = ib.gpu_address;
    m.ib = ib.cpu;
    m.fence_gpu_address = fence.gpu_address;
    m.fence = fence.cpu;
    m.ring = MmioRegion(reinterpret_cast<uint64_t>(ring_page.data()), ring_page.size());
    ring.reset(new AmdUvd6Ring(vcn, clock, m));
    vcn.on_command = [this](uint32_t c, uint64_t a, bool) { on_command(c, a); };
    vcn.on_engine = [this] { on_engine(); };
  }

  void problem(const char* what) {
    ++problems;
    printf("  fake engine: %s\n", what);
  }

  void on_command(uint32_t command, uint64_t address) {
    buffer[command] = address;
    if (command != 0) return;
    // The message buffer: a create or destroy is acted on here (they run no picture); a decode message waits for the engine start.
    const uint8_t* m = vcn.translate(address, kMsgBytes);
    if (!m) { problem("message outside the work memory"); return; }
    const uint32_t type = word(m, 4);
    if (word(m, 0) != kMsgBytes) problem("message size is not the structure's");
    if (type == 0) {
      ++creates;
      stream_handles.push_back(word(m, 8));
      if (word(m, 16) != 7) problem("create: stream type is not H264_PERF");
      if (word(m, 20) != 0 || word(m, 28) == 0 || word(m, 32) == 0 || word(m, 40) == 0) problem("create: no size or no picture buffer size");
    } else if (type == 2) {
      ++destroys;
      stream_handles.push_back(word(m, 8));
      if (word(m, 12) != 0) problem("bad destroy message");
    }
  }

  struct Range { const char* name; uint64_t start, size; };

  void on_engine() {
    const uint64_t msg_addr = buffer[0];
    uint8_t m[kMsgBytes];
    if (!vcn.translate(msg_addr, sizeof m)) { problem("decode message outside the work memory"); return; }
    memcpy(m, vcn.translate(msg_addr, sizeof m), sizeof m);
    if (word(m, 4) != 1 || word(m, 0) != kMsgBytes) { problem("engine started on a message that is not a decode"); return; }
    stream_handles.push_back(word(m, 8));
    const uint32_t k = static_cast<uint32_t>(decodes++);
    if (word(m, 12) != k + 1) problem("feedback number is not the picture count");
    const uint32_t width = word(m, dec(2)), height = word(m, dec(3)), dpb = word(m, dec(5)), ctx = word(m, dec(7)), bsd = word(m, dec(18)), pitch = word(m, dec(24));
    const uint32_t chroma_offset = word(m, dec(30));
    if (word(m, dec(0)) != 7 || word(m, dec(1)) != 1 || width % 16 || height % 16 || !width || !height) problem("bad decode message");
    if (m[dec(52) + 4 * 768] != 1) problem("extension support not set");
    last_dt_pitch = pitch;
    if (word(m, dec(25)) != 0 || word(m, dec(26)) != 0 || word(m, dec(27)) != 0 || word(m, dec(32)) != 0) problem("target is not linear");
    if (word(m, dec(28)) != 0 || word(m, dec(29)) != 0 || word(m, dec(31)) != chroma_offset || word(m, dec(34)) != pitch / 2) problem("target offsets are not Mesa's");
    if (word(m, dec(9)) != width) problem("db_pitch is not the aligned width");
    // Every buffer is inside the work memory, and none overlaps another. (The target's size is not in the message; it is what the surface needs.)
    const uint64_t dt_size = uint64_t{chroma_offset} + uint64_t{pitch} * (height / 2);
    const uint64_t fb = buffer[3], it = buffer[0x204];
    if (fb != msg_addr + 0x1000 || it != msg_addr + 0x1800) problem("feedback / scaling table not where Mesa puts them");
    const Range ranges[] = {{"message", msg_addr, 0x1800 + 992}, {"session", buffer[5], 128 * 1024}, {"dpb", buffer[1], dpb}, {"context", buffer[0x206], ctx},
                            {"bitstream", buffer[0x100], bsd}, {"target", buffer[2], dt_size}};
    for (size_t i = 0; i < 6; ++i) {
      if (!ranges[i].size || !vcn.translate(ranges[i].start, ranges[i].size)) { char t[80]; snprintf(t, sizeof t, "%s buffer outside the work memory", ranges[i].name); problem(t); }
      for (size_t j = i + 1; j < 6; ++j)
        if (ranges[i].start < ranges[j].start + ranges[j].size && ranges[j].start < ranges[i].start + ranges[i].size) { char t[80]; snprintf(t, sizeof t, "%s and %s overlap", ranges[i].name, ranges[j].name); problem(t); }
    }
    // Reference counts and order counts against the CPU decoder.
    if (reference && check_content && k < reference->slices.size()) {
      const auto& slices = reference->slices[k];
      const int32_t cur_top = int32_t(word(m, kCodec + 328)), cur_bottom = int32_t(word(m, kCodec + 332));
      if (!slices.empty() && std::min(cur_top, cur_bottom) != slices[0].current_poc) problem("current order count differs from the CPU decoder's");
      for (const auto& s : slices)
        for (int l = 0; l < 2; ++l)
          for (int e = 0; e < s.count[l]; ++e) {
            bool found = false;
            for (int r = 0; r < 16; ++r) found = found || std::min(int32_t(word(m, kCodec + 336 + r * 8)), int32_t(word(m, kCodec + 340 + r * 8))) == s.poc[l][e];
            if (!found) problem("a reference picture the CPU decoder used is not in the message");
          }
      // The bitstream: every slice behind a start code, then zeros up to bsd_size.
      std::vector<uint8_t> expected;
      for (const auto& nal : slice_nals(reference->aus[k])) { expected.push_back(0); expected.push_back(0); expected.push_back(1); expected.insert(expected.end(), nal.begin(), nal.end()); }
      const uint8_t* bits = vcn.translate(buffer[0x100], bsd);
      if (bits && (expected.size() > bsd || bsd % 128 != 0 || memcmp(bits, expected.data(), expected.size()) != 0)) problem("bitstream differs from the access unit's slices");
      for (uint64_t i = expected.size(); bits && i < bsd; ++i) if (bits[i] != 0) { problem("bitstream padding is not zero"); break; }
    }
    // The feedback area starts with its size, as Mesa leaves it.
    const uint8_t* f = vcn.translate(fb, 4);
    if (!f || word(f, 0) != 2048) problem("feedback size not initialised");
    // "Decode": the CPU decoder's picture for this access unit into the target, NV12, linear, with the pitch and chroma offset the message names.
    if (write_pictures && reference) {
      auto it_frame = reference->i420.find(int64_t{k} * 1000);
      uint8_t* target = vcn.translate(buffer[2], dt_size);
      if (it_frame == reference->i420.end() || !target) { problem("no CPU picture to stand in for the decode"); }
      else {
        const VideoFormat& fmt = reference->format.at(int64_t{k} * 1000);
        const std::vector<uint8_t>& yuv = it_frame->second;
        const uint32_t cw = fmt.chroma_width(), ch = fmt.chroma_height();
        if (uint64_t{pitch} * (crop_y + fmt.height) > chroma_offset) problem("target layout does not fit");
        else {
          for (uint32_t y = 0; y < fmt.height; ++y) memcpy(target + uint64_t{pitch} * (crop_y + y) + crop_x, yuv.data() + uint64_t{y} * fmt.width, fmt.width);
          const uint8_t* u = yuv.data() + uint64_t{fmt.width} * fmt.height;
          const uint8_t* v = u + uint64_t{cw} * ch;
          for (uint32_t y = 0; y < ch; ++y)
            for (uint32_t x = 0; x < cw; ++x) {
              target[chroma_offset + uint64_t{pitch} * (crop_y / 2 + y) + crop_x + 2 * x] = u[uint64_t{y} * cw + x];
              target[chroma_offset + uint64_t{pitch} * (crop_y / 2 + y) + crop_x + 2 * x + 1] = v[uint64_t{y} * cw + x];
            }
        }
      }
    }
  }
};

// Runs a stream through a session; the pictures come out in `out_pts` / `out_bytes` (Y, U, V planes).
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

static DecoderConfig i420_config(ConstByteView avcc = ConstByteView()) {
  DecoderConfig c;
  c.codec = VideoCodec::H264;
  c.output = PixelFormat::I420;
  c.codec_config = avcc;
  return c;
}

static AmdUvd6AvcDecoder::Engine engine_of(Rig& rig, uint32_t handle = 7) {
  AmdUvd6AvcDecoder::Engine e;
  e.ring = rig.ring.get();
  e.memory = &rig.memory;
  e.stream_handle = handle;
  e.job_timeout_ms = 20;
  return e;
}

static void compare_stream(const char* name, bool length_prefixed) {
  char path[160];
  snprintf(path, sizeof path, "tests/data/%s", name);
  const std::vector<uint8_t> file = read_file(path);
  CHECK(!file.empty());
  if (file.empty()) return;
  printf("  %s%s\n", name, length_prefixed ? " (avcC)" : "");
  TestAllocator a;
  {
    Reference ref;
    run_software(a, ConstByteView(file.data(), file.size()), &ref);
    CHECK(!ref.output_pts.empty() && ref.aus.size() == ref.slices.size());
    printf("    %zu access units, %zu pictures out\n", ref.aus.size(), ref.output_pts.size());

    // For the avcC variant: the record from the first SPS and PPS, and every access unit with length-prefixed NAL units.
    std::vector<uint8_t> avcc;
    std::vector<std::vector<uint8_t>> prefixed;
    if (length_prefixed) {
      std::vector<uint8_t> sps, pps;
      for (ConstByteView au : ref.aus) {
        for (uint64_t i = 0; i + 4 <= au.size(); ++i)
          if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            uint64_t j = i + 3;
            while (j + 3 <= au.size() && !(au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 1)) ++j;
            uint64_t end = j + 3 <= au.size() ? j : au.size();
            while (end > i + 3 && au[end - 1] == 0) --end;
            const int t = au[i + 3] & 31;
            if (t == 7 && sps.empty()) sps.assign(au.data() + i + 3, au.data() + end);
            if (t == 8 && pps.empty()) pps.assign(au.data() + i + 3, au.data() + end);
          }
        if (!sps.empty() && !pps.empty()) break;
      }
      CHECK(sps.size() > 4 && pps.size() > 0);
      avcc = {1, sps[1], sps[2], sps[3], 0xFF, 0xE1, uint8_t(sps.size() >> 8), uint8_t(sps.size())};
      avcc.insert(avcc.end(), sps.begin(), sps.end());
      avcc.push_back(1);
      avcc.push_back(uint8_t(pps.size() >> 8));
      avcc.push_back(uint8_t(pps.size()));
      avcc.insert(avcc.end(), pps.begin(), pps.end());
      for (ConstByteView au : ref.aus) {
        std::vector<uint8_t> out;
        for (uint64_t i = 0; i + 4 <= au.size(); ++i)
          if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
            uint64_t j = i + 3;
            while (j + 3 <= au.size() && !(au[j] == 0 && au[j + 1] == 0 && au[j + 2] == 1)) ++j;
            uint64_t end = j + 3 <= au.size() ? j : au.size();
            while (end > i + 3 && au[end - 1] == 0) --end;
            const uint32_t size = uint32_t(end - (i + 3));
            const int t = au[i + 3] & 31;
            if (t == 7 || t == 8) continue;   // the parameter sets are in the record
            out.insert(out.end(), {uint8_t(size >> 24), uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size)});
            out.insert(out.end(), au.data() + i + 3, au.data() + end);
          }
        prefixed.push_back(out);
      }
    }

    Rig rig;
    rig.reference = &ref;
    {
      AmdUvd6AvcDecoder session(a, engine_of(rig), i420_config(ConstByteView(avcc.data(), avcc.size())));
      CHECK(session.prepare());
      // With an avcC record the session is open before the first picture; an Annex B stream opens it with the first picture.
      CHECK(session.session_open() == length_prefixed);
      Output out;
      int64_t pts = 0;
      for (size_t i = 0; i < ref.aus.size(); ++i) {
        const ConstByteView au = length_prefixed ? ConstByteView(prefixed[i].data(), prefixed[i].size()) : ref.aus[i];
        CHECK(feed(session, au, pts, &out));
        pts += 1000;
      }
      session.flush();
      drain(session, &out);
      CHECK(session.statistics().errors == 0 && session.statistics().frames == ref.output_pts.size());
      CHECK(rig.problems == 0 && rig.vcn.protocol_errors == 0);
      CHECK(rig.creates == 1 && rig.decodes == int(ref.aus.size()) && rig.destroys == 0);
      // The same pictures, in the same order, byte for byte.
      CHECK(out.pts == ref.output_pts);
      for (size_t i = 0; i < out.bytes.size() && i < ref.output_pts.size(); ++i) {
        CHECK(out.format[i] == ref.format.at(ref.output_pts[i]));
        CHECK(out.bytes[i] == ref.i420.at(ref.output_pts[i]));
      }
      CHECK(!out.format.empty() && session.output_format().width == out.format.front().width && session.output_format().height == out.format.front().height);
    }
    // Closing the session sent the destroy message, with the handle of the create, and gave the work memory back.
    CHECK(rig.destroys == 1 && rig.problems == 0);
    CHECK(rig.stream_handles.front() == 7 && rig.stream_handles.back() == 7);
    CHECK(rig.memory.free_bytes() == Rig::kArena - 2 * 4096);
  }
  CHECK(a.live == 0);
}

static void test_wedged_engine() {
  const std::vector<uint8_t> file = read_file("tests/data/h264_bframes.h264");
  CHECK(!file.empty());
  TestAllocator a;
  {
    Reference ref;
    run_software(a, ConstByteView(file.data(), file.size()), &ref);
    Rig rig;
    rig.reference = &ref;
    AmdUvd6AvcDecoder session(a, engine_of(rig), i420_config());
    CHECK(session.prepare());
    Output out;
    CHECK(feed(session, ref.aus[0], 0, &out));
    CHECK(rig.decodes == 1 && !rig.ring->wedged());
    // The engine stops answering: the next picture times out, is shown grey, and everything after it is refused without touching the engine again.
    rig.vcn.stall_fence = true;
    CHECK(feed(session, ref.aus[1], 1000, &out));   // accepted (the picture is shown grey), but the ring is now wedged
    CHECK(rig.ring->wedged() && session.statistics().errors == 1);
    const size_t writes = rig.vcn.writes.size();
    VideoPacket p;
    p.data = ref.aus[2];
    p.pts_us = 2000;
    CHECK(session.submit(p) == SubmitResult::Invalid);
    CHECK(rig.vcn.writes.size() == writes);
    session.flush();
    drain(session, &out);
    // The first picture came out intact, the second is grey.
    CHECK(out.bytes.size() >= 2 && out.bytes[0] == ref.i420.at(out.pts[0]));
    if (out.bytes.size() >= 2) {
      const VideoFormat& f = out.format[1];
      CHECK(out.bytes[1].size() == f.frame_bytes() && out.bytes[1][0] == 128 && out.bytes[1][f.frame_bytes() - 1] == 128);
    }
  }
  CHECK(a.live == 0);
}

static void test_too_little_memory() {
  const std::vector<uint8_t> file = read_file("tests/data/h264_bframes.h264");
  TestAllocator a;
  {
    Reference ref;
    run_software(a, ConstByteView(file.data(), file.size()), &ref);
    Rig rig(64 * 1024);   // work memory far too small for any picture
    AmdUvd6AvcDecoder session(a, engine_of(rig), i420_config());
    CHECK(session.prepare());
    Output out;
    VideoPacket p;
    p.data = ref.aus[0];
    CHECK(session.submit(p) == SubmitResult::Invalid);
    CHECK(!session.session_open() && rig.creates == 0 && rig.vcn.writes.empty());
    CHECK(session.statistics().errors >= 1);
  }
  CHECK(a.live == 0);
}

// Damaged streams: every packet of the stream truncated at every few bytes, and a bit flipped here and there. The session must stay inside its buffers (the
// sanitizers say so) and never hang; what it makes of the pictures does not matter.
static void test_damaged_streams() {
  const std::vector<uint8_t> file = read_file("tests/data/h264_bframes.h264");
  TestAllocator a;
  Reference ref;
  run_software(a, ConstByteView(file.data(), file.size()), &ref);
  CHECK(!ref.aus.empty());
  int runs = 0;
  for (size_t variant = 0; variant < 24; ++variant) {
    Rig rig;
    rig.reference = &ref;
    rig.check_content = false;
    AmdUvd6AvcDecoder session(a, engine_of(rig), i420_config());
    CHECK(session.prepare());
    Output out;
    int64_t pts = 0;
    for (size_t i = 0; i < ref.aus.size(); ++i) {
      std::vector<uint8_t> au(ref.aus[i].data(), ref.aus[i].data() + ref.aus[i].size());
      if (variant % 3 == 1 && i > 0) au.resize(au.size() * (variant + 1) / 30 + 1);                      // truncated
      else if (variant % 3 == 2 && i > 0) au[(variant * 37 + i * 11) % au.size()] ^= uint8_t(1u << (variant % 8));   // one bit flipped
      VideoPacket p;
      p.data = ConstByteView(au.data(), au.size());
      p.pts_us = pts;
      SubmitResult r = session.submit(p);
      if (r == SubmitResult::Full) { drain(session, &out); r = session.submit(p); }
      drain(session, &out);
      pts += 1000;
      ++runs;
    }
    session.flush();
    drain(session, &out);
  }
  CHECK(runs > 100);
  CHECK(a.live == 0);
}

static void test_one_session_at_a_time() {
  TestAllocator a;
  {
    Rig rig;
    AmdUvd6AvcDecoder first(a, engine_of(rig), i420_config());
    CHECK(first.prepare());
    {
      AmdUvd6AvcDecoder second(a, engine_of(rig, 8), i420_config());
      CHECK(!second.prepare());   // the engine is busy
    }
    // The refused session did not give up the first one's lease.
    AmdUvd6AvcDecoder third(a, engine_of(rig, 9), i420_config());
    CHECK(!third.prepare());
  }
  {
    Rig rig;
    { AmdUvd6AvcDecoder first(a, engine_of(rig), i420_config()); CHECK(first.prepare()); }
    AmdUvd6AvcDecoder next(a, engine_of(rig, 8), i420_config());
    CHECK(next.prepare());   // closed: free again
  }
  CHECK(a.live == 0);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_one_session_at_a_time();
  compare_stream("h264_bframes.h264", false);
  compare_stream("h264_bframes.h264", true);
  test_wedged_engine();
  test_too_little_memory();
  test_damaged_streams();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("AmdUvd6AvcDecoderTest passed\n");
  return 0;
}
