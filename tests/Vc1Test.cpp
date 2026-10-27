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


// Host-side tests for the VC-1 decoder. Every stream in tests/data/vc1_* (made by scripts/gen_vc1_test_streams.sh: random but valid syntax from
// scripts/gen_vc1_synthetic.py) is decoded and compared with the pictures ffmpeg's C decoder made of it: the same pictures in the same order, every sample equal.
// Damaged streams (truncated, bits flipped) must not fault, hang or leak.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/VideoCodec/Software/Vc1.hpp"
#include "Hardware/VideoCodec/Software/Vc1Core.hpp"
#include "Vc1InterlacedFrames.hpp"

using namespace hardware;
using namespace vc1test;

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

static uint32_t le32(const std::vector<uint8_t>& d, size_t at) { return d[at] | d[at + 1] << 8 | d[at + 2] << 16 | uint32_t(d[at + 3]) << 24; }

// A test stream split into what the decoder takes: the configuration (Simple and Main profile) and one unit per frame.
struct Stream {
  bool advanced = false;
  uint8_t struct_c[4] = {};
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> bytes;
  std::vector<std::pair<size_t, size_t>> units;   // offset, size
};

// The RCV container: a header (frame count, sequence header, size) then, per frame, a 24 bit size, a flag byte, a 32 bit timestamp and the data.
static bool parse_rcv(Stream* s) {
  const std::vector<uint8_t>& d = s->bytes;
  if (d.size() < 36 || (le32(d, 0) >> 24) != 0xC5 || le32(d, 4) != 4) return false;
  memcpy(s->struct_c, &d[8], 4);
  s->height = le32(d, 12);
  s->width = le32(d, 16);
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
static void split_advanced(Stream* s) {
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

static bool load(const char* name, Stream* s) {
  char path[160];
  snprintf(path, sizeof path, "tests/data/vc1_%s.rcv", name);
  s->bytes = read_file(path);
  if (!s->bytes.empty()) return parse_rcv(s);
  snprintf(path, sizeof path, "tests/data/vc1_%s.vc1", name);
  s->bytes = read_file(path);
  s->advanced = true;
  split_advanced(s);
  return !s->units.empty();
}

struct Result {
  int frames = 0;
  int max_diff = 0;
  uint32_t width = 0, height = 0;
};

static void collect(Vc1Decoder& dec, const std::vector<uint8_t>& ref, Result* r) {
  VideoFrame f;
  while (dec.receive(&f)) {
    r->width = f.format.width;
    r->height = f.format.height;
    const size_t bytes = size_t(f.format.width) * f.format.height + 2 * size_t(f.format.chroma_width()) * f.format.chroma_height();
    const size_t offset = size_t(r->frames) * bytes;
    if (!ref.empty() && offset + bytes <= ref.size()) {
      size_t p = offset;
      for (uint32_t c = 0; c < 3; ++c)
        for (uint32_t y = 0; y < f.format.plane_height(c); ++y)
          for (uint32_t x = 0; x < f.format.plane_width(c); ++x) {
            const int d = abs(int(f.plane[c][size_t(y) * f.stride[c] + x]) - int(ref[p++]));
            if (d > r->max_diff) r->max_diff = d;
          }
    } else {
      r->max_diff = 255;   // more pictures than the reference has
    }
    ++r->frames;
  }
}

static bool open_decoder(Vc1Decoder& dec, const Stream& s) { return s.advanced || dec.configure(ConstByteView(s.struct_c, 4), s.width, s.height); }

static Result decode_stream(allocators::IAllocator& a, const Stream& s, const std::vector<uint8_t>& ref) {
  Result r;
  Vc1Decoder dec(a);
  CHECK(open_decoder(dec, s));
  int64_t k = 0;
  for (const auto& u : s.units) {
    CHECK(dec.decode(ConstByteView(s.bytes.data() + u.first, u.second), k++ * 1000));
    collect(dec, ref, &r);
  }
  dec.flush();
  collect(dec, ref, &r);
  return r;
}

static void test_stream(const char* name) {
  Stream s;
  CHECK(load(name, &s));
  char path[160];
  snprintf(path, sizeof path, "tests/data/vc1_%s.yuv", name);
  const std::vector<uint8_t> ref = read_file(path);
  CHECK(!s.units.empty() && !ref.empty());
  if (s.units.empty() || ref.empty()) return;
  TestAllocator a;
  {
    const Result r = decode_stream(a, s, ref);
    const size_t frame_bytes = size_t(r.width) * r.height + 2 * size_t((r.width + 1) / 2) * ((r.height + 1) / 2);
    printf("  %-12s %ux%u, %d pictures, largest difference %d\n", name, r.width, r.height, r.frames, r.max_diff);
    CHECK(frame_bytes && size_t(r.frames) * frame_bytes == ref.size() && size_t(r.frames) == s.units.size());
    CHECK(r.max_diff == 0);
  }
  CHECK(a.live == 0);
}

// Damaged streams: every frame cut short at many points, and single bits flipped all over the stream. Whatever the decoder makes of them, it must stay inside its
// buffers (the sanitizers say so), not hang, and not leak.
static void test_damaged(const char* name) {
  Stream s;
  CHECK(load(name, &s));
  if (s.units.empty()) return;
  TestAllocator a;
  int runs = 0;
  {
    for (int variant = 0; variant < 40; ++variant) {
      Vc1Decoder dec(a);
      VideoFrame f;
      if (!open_decoder(dec, s)) continue;
      for (size_t u = 0; u < s.units.size(); ++u) {
        std::vector<uint8_t> au(s.bytes.begin() + long(s.units[u].first), s.bytes.begin() + long(s.units[u].first + s.units[u].second));
        if (u > 0 && !au.empty()) {
          if (variant % 2 == 1) au.resize(au.size() * size_t(variant + 1) / 41 + 1);                                                    // cut short
          else au[(size_t(variant) * 7919 + u * 104729) % au.size()] ^= uint8_t(1u << (variant % 8));                                   // one bit flipped
        }
        dec.decode(ConstByteView(au.data(), au.size()), int64_t(u) * 1000);
        while (dec.receive(&f)) {}
        ++runs;
      }
      dec.flush();
      while (dec.receive(&f)) {}
    }
  }
  CHECK(runs > 100 && a.live == 0);
}

static void test_config() {
  TestAllocator a;
  {
    Stream s;
    CHECK(load("sm_ipp", &s));
    Vc1Decoder dec(a);
    CHECK(!dec.configure(ConstByteView(s.struct_c, 3), s.width, s.height));      // too short
    CHECK(!dec.configure(ConstByteView(s.struct_c, 4), 0, s.height));            // no size
    uint8_t bad[4];
    memcpy(bad, s.struct_c, 4);
    bad[0] = uint8_t((bad[0] & 0x3F) | 0xC0);                                    // the Complex profile
    CHECK(!dec.configure(ConstByteView(bad, 4), s.width, s.height));
    CHECK(dec.configure(ConstByteView(s.struct_c, 4), s.width, s.height));
    // A frame before an intra picture is refused or ignored; nothing comes out of it.
    Vc1Decoder fresh(a);
    CHECK(fresh.configure(ConstByteView(s.struct_c, 4), s.width, s.height));
    VideoFrame f;
    fresh.decode(ConstByteView(s.bytes.data() + s.units[1].first, s.units[1].second), 0);
    CHECK(!fresh.receive(&f));
  }
  CHECK(a.live == 0);
}

// ---- interlaced pictures (frame and field coding): no macroblock data is needed, since only an engine decodes them; the headers are built bit by bit.
struct InterlacedRecorder final : vc1::IOffload {
  struct Seen { int fcm, type, fptype, units; bool forward, backward; };
  std::vector<Seen> seen;
  bool accept = true;
  bool accepts(const vc1::SequenceHeader& seq) override { return accept && seq.interlace; }
  bool decode_picture(const vc1::OffloadPicture& p) override {
    seen.push_back({p.pic->fcm, p.pic->type, p.pic->fptype, int(p.units.size()), p.forward_slot >= 0, p.backward_slot >= 0});
    return true;
  }
};

static void test_interlaced_pictures() {
  struct Step { int fcm, value; bool field_unit; int type; };
  // Decode order: I (frame), P (field PP), B (frame), B (field BB), P (field PI), B (field B,BI), B (field BI,BI), I (field I,P).
  const Step steps[] = {{1, 2, false, vc1::kPictureI}, {2, 3, true, vc1::kPictureP}, {1, 1, false, vc1::kPictureB}, {2, 4, true, vc1::kPictureB},
                        {2, 2, true, vc1::kPictureP}, {2, 5, true, vc1::kPictureB}, {2, 7, true, vc1::kPictureBI}, {2, 1, true, vc1::kPictureI}};
  const uint32_t count = sizeof steps / sizeof steps[0];
  {
    TestAllocator a;
    {
      InterlacedRecorder recorder;
      Vc1Decoder cpu(a);
      cpu.set_offload(&recorder);
      uint32_t frames = 0;
      VideoFrame f;
      for (uint32_t i = 0; i < count; ++i) {
        std::vector<uint8_t> unit;
        if (i == 0) unit = interlaced_headers(true);
        const std::vector<uint8_t> frame = interlaced_frame(steps[i].fcm, steps[i].value, steps[i].field_unit);
        unit.insert(unit.end(), frame.begin(), frame.end());
        CHECK(cpu.decode(ConstByteView(unit.data(), unit.size()), int64_t{i} * 1000));
        while (cpu.receive(&f)) ++frames;
      }
      cpu.flush();
      while (cpu.receive(&f)) ++frames;
      CHECK(frames == count && recorder.seen.size() == count);
      for (uint32_t i = 0; i < recorder.seen.size() && i < count; ++i) {
        const auto& s = recorder.seen[i];
        CHECK(s.fcm == steps[i].fcm && s.type == steps[i].type && s.units == (steps[i].field_unit ? 2 : 1));
        CHECK(steps[i].fcm == 2 ? s.fptype == steps[i].value : true);
        // References: an intra frame has none, a predicted one the older anchor, a B frame both.
        if (steps[i].type == vc1::kPictureB) CHECK(s.forward && s.backward);
        if (steps[i].type == vc1::kPictureP) CHECK(s.forward && !s.backward);
        if (steps[i].type == vc1::kPictureI) CHECK(!s.forward && !s.backward);
      }
    }
    CHECK(a.live == 0);
  }
  {
    // Display order: an anchor waits for the next anchor, a B frame goes out at once. Decode order I P B B P B B I gives I B B P B B ... the pts of what comes out tell.
    TestAllocator a;
    InterlacedRecorder recorder;
    Vc1Decoder cpu(a);
    cpu.set_offload(&recorder);
    std::vector<int64_t> order;
    VideoFrame f;
    for (uint32_t i = 0; i < count; ++i) {
      std::vector<uint8_t> unit;
      if (i == 0) unit = interlaced_headers(true);
      const std::vector<uint8_t> frame = interlaced_frame(steps[i].fcm, steps[i].value, steps[i].field_unit);
      unit.insert(unit.end(), frame.begin(), frame.end());
      cpu.decode(ConstByteView(unit.data(), unit.size()), int64_t{i} * 1000);
      while (cpu.receive(&f)) order.push_back(f.pts_us / 1000);
    }
    cpu.flush();
    while (cpu.receive(&f)) order.push_back(f.pts_us / 1000);
    const int64_t want[] = {0, 2, 3, 1, 5, 6, 4, 7};
    CHECK(order.size() == count);
    for (uint32_t i = 0; i < count && i < order.size(); ++i) CHECK(order[i] == want[i]);
  }
  {
    // Without an engine the CPU has no pixel path for these pictures: they are refused, nothing comes out. An engine that refuses the sequence is no engine.
    TestAllocator a;
    InterlacedRecorder refusing;
    refusing.accept = false;
    for (int mode = 0; mode < 2; ++mode) {
      Vc1Decoder cpu(a);
      if (mode == 1) cpu.set_offload(&refusing);
      std::vector<uint8_t> unit = interlaced_headers(true);
      const std::vector<uint8_t> frame = interlaced_frame(1, 2, false);
      unit.insert(unit.end(), frame.begin(), frame.end());
      CHECK(!cpu.decode(ConstByteView(unit.data(), unit.size()), 0));
      cpu.flush();
      VideoFrame f;
      CHECK(!cpu.receive(&f));
    }
    CHECK(refusing.seen.empty());
  }
  {
    // Progressive segmented frames are refused (nothing to check them against).
    InterlacedRecorder recorder;
    TestAllocator a;
    Vc1Decoder cpu(a);
    cpu.set_offload(&recorder);
    std::vector<uint8_t> unit = interlaced_headers(true, /*psf=*/true);
    const std::vector<uint8_t> frame = interlaced_frame(1, 2, false);
    unit.insert(unit.end(), frame.begin(), frame.end());
    CHECK(!cpu.decode(ConstByteView(unit.data(), unit.size()), 0) && recorder.seen.empty());
  }
  {
    // A sequence that is not interlaced has no frame coding mode: the engine is asked, and a frame is read as before (no mode bit).
    InterlacedRecorder recorder;
    TestAllocator a;
    Vc1Decoder cpu(a);
    cpu.set_offload(&recorder);
    std::vector<uint8_t> unit = interlaced_headers(false);
    const std::vector<uint8_t> frame = interlaced_frame(1, 2, false);   // (a mode bit that a progressive sequence's frame does not have)
    unit.insert(unit.end(), frame.begin(), frame.end());
    cpu.decode(ConstByteView(unit.data(), unit.size()), 0);
    CHECK(recorder.seen.empty());   // (the recorder wants an interlaced sequence)
  }
}

int main() {
  test_interlaced_pictures();
  static const char* const streams[] = {"simple_intra", "sm_intra", "sm_intra_lf", "sm_intra_q", "sm_intra_nu", "adv_intra", "adv_intra_lf", "adv_intra_odd", "sp_ipp", "sm_ipp", "sm_ipp_q", "sm_ipp_nu",
                                        "sm_ipb", "sm_ipb_f", "adv_ipp", "adv_ipb", "adv_ipp_odd", "adv_skip",
                                        "adv_il_ipb", "adv_il_ipp", "adv_il_skip"};   // (the last three: an interlaced sequence whose frames are progressive)
  for (const char* name : streams) test_stream(name);
  for (const char* name : {"sm_ipb_f", "sm_ipp_q", "adv_ipb", "adv_skip", "adv_ipp_odd", "sp_ipp", "adv_il_ipb"}) test_damaged(name);
  test_config();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("Vc1Test passed\n");
  return 0;
}
