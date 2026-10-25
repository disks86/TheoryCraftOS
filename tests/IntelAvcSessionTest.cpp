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

// Host-side tests for the Intel hardware H.264 decode session (IntelAvcDecoder) against a simulated GPU. The simulated engine plays the decode hardware: for every batch it reads
// the commands the way the engine would, checks that every address in them is mapped in the context's address space, resolves each reference list entry through the frame
// stores to the picture it names, and "decodes" by painting the output surface with a signature of the picture's order count. The CPU decoder runs on the same streams and
// reports the reference lists it builds; the two must agree slice by slice, produce the pictures in the same order, and the pictures the session hands out must be the ones
// painted for them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

#include "FakeIntelGpu.hpp"
#include "Hardware/VideoCodec/Intel/IntelAvcSession.hpp"
#include "Hardware/VideoCodec/Intel/IntelMediaVideoEngine.hpp"
#include "Hardware/VideoCodec/Software/H264.hpp"
#include "Hardware/VideoCodec/SystemVideoCodecs.hpp"

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

struct File {
  uint8_t* data = nullptr;
  long size = 0;
  explicit File(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = static_cast<uint8_t*>(malloc(size));
    if (fread(data, 1, size, f) != static_cast<size_t>(size)) { free(data); data = nullptr; }
    fclose(f);
  }
  ~File() { free(data); }
  ConstByteView view() const { return ConstByteView(data, static_cast<uint64_t>(size)); }
};

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

// What the picture's order count looks like painted on its surface, and back.
static uint8_t signature(int poc) { return static_cast<uint8_t>(poc * 3 + 60); }
static int poc_of_signature(uint8_t y) { return static_cast<int8_t>(static_cast<uint8_t>((y - 60) * 171)); }  // 3 * 171 = 1 mod 256

struct SliceRecord {
  int type = 0, first_mb = 0, current_poc = 0;
  int count[2] = {0, 0};
  int poc[2][32] = {};
  bool long_term[2][32] = {};
};

// The decode engine of the simulated GPU.
struct FakeDecoder {
  std::vector<SliceRecord> slices;
  std::map<uint64_t, int> painted_poc;
  std::map<uint64_t, uint64_t> painted_dmv;
  int pictures = 0, problems = 0;

  static uint64_t addr(const std::vector<uint32_t>& d, size_t at) { return (uint64_t{d[at + 1]} << 32) | d[at]; }

  void problem(const char* what) {
    ++problems;
    printf("  fake engine: %s\n", what);
  }

  void run(FakeGpu& gpu, const std::vector<uint32_t>& d) {
    uint64_t ref_pic[16] = {}, ref_dmv[16] = {}, out_pre = 0, out_post = 0, cur_dmv = 0;
    int32_t ref_poc[16] = {}, cur_poc[2] = {};
    uint8_t entries[2][32];
    memset(entries, 0xFF, sizeof entries);
    uint32_t width_mbs = 0, height_mbs = 0;
    SliceRecord pending;
    int list_seen = 0;
    size_t i = 0;
    while (i < d.size()) {
      const uint32_t h = d[i];
      if (h == 0) { ++i; continue; }
      if ((h >> 23) == 0x0A) break;
      if ((h >> 29) == 0) { i += (h & 0x3F) + 2; continue; }
      const size_t len = (h & 0xFF) + 2;
      if (i + len > d.size()) { problem("command runs past the batch"); return; }
      switch (h & 0xFFFF0000) {
        case kMfxPipeBufAddrState:
          out_pre = addr(d, i + 1);
          out_post = addr(d, i + 4);
          for (int k = 0; k < 16; ++k) ref_pic[k] = addr(d, i + 19 + 2 * k);
          break;
        case kMfxAvcImgState:
          width_mbs = (d[i + 2] & 0xFFFF) + 1;
          height_mbs = (d[i + 2] >> 16) + 1;
          break;
        case kMfxAvcDirectmodeState:
          for (int k = 0; k < 16; ++k) { ref_dmv[k] = addr(d, i + 1 + 2 * k); ref_poc[k] = static_cast<int32_t>(d[i + 37 + 2 * k]); }
          cur_dmv = addr(d, i + 34);
          cur_poc[0] = static_cast<int32_t>(d[i + 69]);
          cur_poc[1] = static_cast<int32_t>(d[i + 70]);
          break;
        case kMfxAvcRefIdxState: {
          const uint32_t list = d[i + 1];
          if (list > 1) { problem("reference list number"); return; }
          for (int k = 0; k < 32; ++k) entries[list][k] = static_cast<uint8_t>(d[i + 2 + k / 4] >> (8 * (k % 4)));
          ++list_seen;
          break;
        }
        case kMfxAvcSliceState:
          pending = SliceRecord();
          pending.type = static_cast<int>(d[i + 1]);
          pending.count[0] = static_cast<int>((d[i + 2] >> 16) & 0xFF);
          pending.count[1] = static_cast<int>((d[i + 2] >> 24) & 0xFF);
          pending.first_mb = static_cast<int>(d[i + 4] & 0xFFFF);
          pending.current_poc = cur_poc[0];
          break;
        case kMfdAvcBsdObject:
          if (d[i + 1] != 0) {  // a real slice (the phantom slice has no data)
            for (int l = 0; l < 2; ++l) {
              for (int k = 0; k < pending.count[l]; ++k) {
                const uint8_t e = entries[l][k];
                if (e == 0xFF) { problem("empty entry in a used reference list"); continue; }
                const int fs = (e >> 1) & 0xF;
                const uint64_t a = ref_pic[fs];
                auto it = painted_poc.find(a);
                if (a == 0 || it == painted_poc.end()) { problem("reference names a picture that was never decoded"); pending.poc[l][k] = -100000; continue; }
                pending.poc[l][k] = it->second;
                pending.long_term[l][k] = (e >> 6) & 1;
                if (ref_poc[fs] != it->second) problem("frame store order count differs from the picture's");
                if (ref_dmv[fs] != painted_dmv[a]) problem("frame store motion vector buffer differs from the picture's");
              }
            }
            slices.push_back(pending);
          }
          memset(entries, 0xFF, sizeof entries);
          break;
        default: break;
      }
      i += len;
    }
    (void)list_seen;
    // Every address the commands use must be mapped in the context's address space.
    for (uint64_t a : {out_pre, out_post, cur_dmv}) {
      if (!a) continue;
      bool ok;
      if (!gpu.ppgtt_bytes(a, 1, "command address", &ok) || !ok) problem("an address in the commands is not mapped");
    }
    // "Decode": paint the output picture.
    const uint64_t out = out_pre ? out_pre : out_post;
    Nv12TiledLayout layout;
    if (!out || !nv12_tiled_layout(width_mbs * 16, height_mbs * 16, &layout)) { problem("no output picture"); return; }
    const uint64_t y_bytes = uint64_t{layout.pitch} * layout.aligned_height;
    for (uint64_t off = 0; off < layout.size; off += 4096) {
      bool ok;
      uint8_t* p = gpu.ppgtt_bytes(out + off, 4096, "output surface", &ok);
      if (!ok) { problem("output surface not fully mapped"); return; }
      memset(p, off < y_bytes ? signature(cur_poc[0]) : 128, 4096);
    }
    painted_poc[out] = cur_poc[0];
    painted_dmv[out] = cur_dmv;
    ++pictures;
  }
};

struct Rig2 : Rig {
  FakeDecoder engine;
  AllocatorGpuMemory* unused = nullptr;
  Rig2() { gpu.on_batch = [this](FakeGpu& g, const std::vector<uint32_t>& d) { engine.run(g, d); }; }
};

// The CPU decoder's view of a stream.
struct SoftwareRun {
  std::vector<H264Decoder::SliceRefs> slices;
  std::vector<int64_t> output_pts;
  uint32_t width = 0, height = 0;
};

static void observe(void* context, const H264Decoder::SliceRefs& refs) { static_cast<SoftwareRun*>(context)->slices.push_back(refs); }

static void drain(IVideoDecoder& dec, std::vector<int64_t>* pts, std::vector<uint8_t>* first_y, uint32_t* w, uint32_t* h) {
  VideoFrame f;
  while (dec.receive(&f)) {
    pts->push_back(f.pts_us);
    if (first_y) first_y->push_back(f.plane[0][0]);
    if (w) *w = f.format.width;
    if (h) *h = f.format.height;
  }
}

static bool feed(IVideoDecoder& dec, ConstByteView au, int64_t pts, std::vector<int64_t>* out, std::vector<uint8_t>* ys, uint32_t* w, uint32_t* h) {
  VideoPacket p;
  p.data = au;
  p.pts_us = pts;
  SubmitResult r = dec.submit(p);
  if (r == SubmitResult::Full) {
    drain(dec, out, ys, w, h);
    r = dec.submit(p);
  }
  drain(dec, out, ys, w, h);
  return r == SubmitResult::Accepted;
}

static SoftwareRun run_software(allocators::IAllocator& a, ConstByteView stream, std::vector<ConstByteView>* aus) {
  SoftwareRun run;
  // The CPU decoder through its own interface, with the observer.
  H264Decoder dec(a);
  dec.set_slice_observer(observe, &run);
  uint64_t pos = 0;
  ConstByteView au;
  int64_t pts = 0;
  VideoFrame f;
  while (next_access_unit(stream, &pos, &au)) {
    aus->push_back(au);
    auto take = [&] {
      while (dec.receive(&f)) { run.output_pts.push_back(f.pts_us); run.width = f.format.width; run.height = f.format.height; }
    };
    take();
    dec.decode(au, pts);
    pts += 1000;
    take();
  }
  dec.flush();
  while (dec.receive(&f)) { run.output_pts.push_back(f.pts_us); run.width = f.format.width; run.height = f.format.height; }
  return run;
}

static void compare_stream(const char* name) {
  char path[160];
  snprintf(path, sizeof path, "tests/data/%s", name);
  File file(path);
  CHECK(file.data != nullptr);
  if (!file.data) return;
  printf("  %s\n", name);

  TestAllocator a;
  {
    std::vector<ConstByteView> aus;
    SoftwareRun sw = run_software(a, file.view(), &aus);
    CHECK(!sw.slices.empty() && !sw.output_pts.empty());
    int kinds[3] = {0, 0, 0};
    for (const auto& sr : sw.slices) ++kinds[sr.slice_type];
    printf("    %zu slices (P %d, B %d, I %d), %zu pictures out\n", sw.slices.size(), kinds[0], kinds[1], kinds[2], sw.output_pts.size());

    Rig2 rig;
    {
      VcsDriver driver(rig.mem);
      CHECK(driver.initialize(rig.config()));
      {
        DecoderConfig config;
        config.codec = VideoCodec::H264;
        config.output = PixelFormat::I420;
        IntelAvcDecoder session(a, driver, config);
        CHECK(session.prepare());
        std::vector<int64_t> pts_out;
        std::vector<uint8_t> ys;
        uint32_t w = 0, h = 0;
        int64_t pts = 0;
        for (ConstByteView au : aus) {
          CHECK(feed(session, au, pts, &pts_out, &ys, &w, &h));
          pts += 1000;
        }
        session.flush();
        drain(session, &pts_out, &ys, &w, &h);

        CHECK(session.statistics().errors == 0);
        CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
        // The same pictures, in the same order, of the same size.
        CHECK(pts_out == sw.output_pts);
        CHECK(w == sw.width && h == sw.height && session.output_format().width == sw.width);
        // Slice by slice: the same kind, the same place in the picture, the same references.
        CHECK(rig.engine.slices.size() == sw.slices.size());
        for (size_t s = 0; s < rig.engine.slices.size() && s < sw.slices.size(); ++s) {
          const SliceRecord& hw = rig.engine.slices[s];
          const H264Decoder::SliceRefs& sr = sw.slices[s];
          bool same = hw.type == sr.slice_type && hw.first_mb == sr.first_mb && hw.current_poc == sr.current_poc && hw.count[0] == sr.count[0] && hw.count[1] == sr.count[1];
          for (int l = 0; l < 2 && same; ++l)
            for (int k = 0; k < hw.count[l]; ++k) same = same && hw.poc[l][k] == sr.poc[l][k] && hw.long_term[l][k] == sr.long_term[l][k];
          if (!same) { printf("  slice %zu differs (type %d/%d, mb %d/%d, poc %d/%d)\n", s, hw.type, sr.slice_type, hw.first_mb, sr.first_mb, hw.current_poc, sr.current_poc); }
          CHECK(same);
        }
        // The pictures handed out are the ones painted for them: the signature of the picture decoded with that time stamp.
        std::map<int64_t, int> poc_by_pts;  // pts of a picture -> its order count, from the CPU decoder's slices in decode order
        {
          // A slice with first_mb 0 starts a picture; the n-th picture has pts n * 1000.
          int64_t n = -1;
          for (const auto& sr : sw.slices) if (sr.first_mb == 0) { ++n; poc_by_pts[n * 1000] = sr.current_poc; }
        }
        CHECK(ys.size() == pts_out.size());
        for (size_t k = 0; k < ys.size() && k < pts_out.size(); ++k) {
          // The pictures' time stamps are the index of their access unit, which is the n-th picture only when every access unit holds one.
          auto it = poc_by_pts.find(pts_out[k]);
          CHECK(it != poc_by_pts.end() && poc_of_signature(ys[k]) == it->second);
        }
      }
      // The session gave its buffers back; the driver has only its own left.
      const int baseline = rig.mem.live();
      (void)baseline;
    }
    CHECK(rig.gpu.violations.empty());
    CHECK(rig.mem.live() == 1);  // only the scratch page that is never freed
  }
  CHECK(a.live == 0);
}

static DecoderConfig i420_config() {
  DecoderConfig c;
  c.codec = VideoCodec::H264;
  c.output = PixelFormat::I420;
  return c;
}

static std::vector<ConstByteView> split(ConstByteView stream) {
  std::vector<ConstByteView> aus;
  uint64_t pos = 0;
  ConstByteView au;
  while (next_access_unit(stream, &pos, &au)) aus.push_back(au);
  return aus;
}

// One session at a time: the engine's address space is shared.
static void test_lease() {
  TestAllocator a;
  Rig2 rig;
  {
    VcsDriver driver(rig.mem);
    CHECK(driver.initialize(rig.config()));
    {
      IntelAvcDecoder first(a, driver, i420_config());
      CHECK(first.prepare());
      IntelAvcDecoder second(a, driver, i420_config());
      CHECK(!second.prepare());
    }
    IntelAvcDecoder third(a, driver, i420_config());
    CHECK(third.prepare());  // the lease came back
    // A bad avcC record is refused and gives the lease back.
    DecoderConfig bad = i420_config();
    const uint8_t junk[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    bad.codec_config = ConstByteView(junk, sizeof junk);
    IntelAvcDecoder fourth(a, driver, bad);
    CHECK(!fourth.prepare());
  }
  CHECK(a.live == 0);
}

// The engine stops answering in the middle of a stream.
static void test_hang_and_reset() {
  File file("tests/data/h264_bframes.h264");
  CHECK(file.data != nullptr);
  if (!file.data) return;
  const std::vector<ConstByteView> aus = split(file.view());
  TestAllocator a;
  Rig2 rig;
  int live_with_session = 0;
  {
    VcsDriver driver(rig.mem);
    CHECK(driver.initialize(rig.config()));
    {
      IntelAvcDecoder session(a, driver, i420_config());
      CHECK(session.prepare());
      std::vector<int64_t> out;
      for (size_t i = 0; i < 4 && i < aus.size(); ++i) CHECK(feed(session, aus[i], static_cast<int64_t>(i) * 1000, &out, nullptr, nullptr, nullptr));
      rig.gpu.hang = true;
      bool refused = false;
      for (size_t i = 4; i < aus.size(); ++i) {
        VideoPacket p;
        p.data = aus[i];
        p.pts_us = static_cast<int64_t>(i) * 1000;
        const SubmitResult r = session.submit(p);
        if (r == SubmitResult::Full) drain(session, &out, nullptr, nullptr, nullptr);
        refused |= r == SubmitResult::Invalid;
      }
      CHECK(refused || driver.state() == VcsDriver::State::Wedged);
      CHECK(driver.state() == VcsDriver::State::Wedged);
      CHECK(session.statistics().errors > 0);
      VideoPacket p;
      p.data = aus[0];
      CHECK(session.submit(p) == SubmitResult::Invalid);  // and stays so
      live_with_session = rig.mem.live();
    }
    // The session is gone and so is the driver; the engine may still be writing, so nothing was given back.
    CHECK(rig.mem.live() == live_with_session);
  }
  CHECK(rig.mem.live() == live_with_session);
  CHECK(rig.gpu.violations.empty());
  CHECK(a.live == 0);

  // After a seek (reset) the session waits for an IDR picture again.
  Rig2 rig2;
  {
    VcsDriver driver(rig2.mem);
    CHECK(driver.initialize(rig2.config()));
    IntelAvcDecoder session(a, driver, i420_config());
    CHECK(session.prepare());
    std::vector<int64_t> out;
    for (size_t i = 0; i < aus.size(); ++i) feed(session, aus[i], static_cast<int64_t>(i) * 1000, &out, nullptr, nullptr, nullptr);
    session.flush();
    drain(session, &out, nullptr, nullptr, nullptr);
    const size_t first_run = out.size();
    CHECK(first_run > 5);
    session.reset();
    VideoPacket p;
    p.data = aus[3];  // not a picture to start with
    session.submit(p);
    session.flush();
    std::vector<int64_t> none;
    drain(session, &none, nullptr, nullptr, nullptr);
    CHECK(none.empty());
    for (size_t i = 0; i < aus.size(); ++i) feed(session, aus[i], static_cast<int64_t>(i) * 1000, &none, nullptr, nullptr, nullptr);
    session.flush();
    drain(session, &none, nullptr, nullptr, nullptr);
    CHECK(none.size() == first_run);
  }
  CHECK(rig2.gpu.violations.empty());
  CHECK(a.live == 0);
}

// Two streams of different sizes one after the other: buffers are replaced, and the pictures of the old size come out first.
static void test_resize() {
  File small("tests/data/h264_cavlc.h264"), other("tests/data/h264_bframes.h264");
  CHECK(small.data && other.data);
  if (!small.data || !other.data) return;
  std::vector<ConstByteView> aus = split(small.view());
  const size_t first_count = aus.size();
  for (ConstByteView au : split(other.view())) aus.push_back(au);
  TestAllocator a;
  Rig2 rig;
  {
    VcsDriver driver(rig.mem);
    CHECK(driver.initialize(rig.config()));
    {
      IntelAvcDecoder session(a, driver, i420_config());
      CHECK(session.prepare());
      std::vector<int64_t> pts;
      std::vector<std::pair<uint32_t, uint32_t>> sizes;
      VideoFrame f;
      for (size_t i = 0; i < aus.size(); ++i) {
        VideoPacket p;
        p.data = aus[i];
        p.pts_us = static_cast<int64_t>(i) * 1000;
        SubmitResult r = session.submit(p);
        while (r == SubmitResult::Full) {
          while (session.receive(&f)) { pts.push_back(f.pts_us); sizes.push_back({f.format.width, f.format.height}); }
          r = session.submit(p);
        }
        CHECK(r == SubmitResult::Accepted);
        while (session.receive(&f)) { pts.push_back(f.pts_us); sizes.push_back({f.format.width, f.format.height}); }
      }
      session.flush();
      while (session.receive(&f)) { pts.push_back(f.pts_us); sizes.push_back({f.format.width, f.format.height}); }
      // All the pictures of both streams, none lost at the change, the 90x50 ones first.
      CHECK(pts.size() == 10 + 14);
      size_t changes = 0;
      for (size_t i = 1; i < sizes.size(); ++i) changes += sizes[i] != sizes[i - 1];
      CHECK(changes == 1 && sizes.front() == std::make_pair(90u, 50u) && sizes.back() == std::make_pair(80u, 48u));
      for (size_t i = 0; i < pts.size(); ++i) CHECK((pts[i] < static_cast<int64_t>(first_count) * 1000) == (sizes[i].first == 90));
      CHECK(session.statistics().errors == 0 && rig.engine.problems == 0);
    }
    CHECK(rig.gpu.violations.empty());
  }
  CHECK(rig.mem.live() >= 1);
  CHECK(a.live == 0);
}

// Damaged streams: whatever comes out, the engine is never pointed at anything unmapped or freed, and nothing leaks.
static void test_damaged_streams() {
  const char* names[] = {"h264_cavlc.h264", "h264_bframes.h264", "h264_ltr.h264", "h264_cqm.h264"};
  uint32_t seed = 12345;
  auto rnd = [&seed]() { seed = seed * 1664525 + 1013904223; return seed >> 8; };
  for (const char* name : names) {
    char path[160];
    snprintf(path, sizeof path, "tests/data/%s", name);
    File file(path);
    CHECK(file.data != nullptr);
    if (!file.data) continue;
    for (int round = 0; round < 12; ++round) {
      std::vector<uint8_t> copy(file.data, file.data + file.size);
      uint64_t len = copy.size();
      switch (round % 3) {
        case 0: len = rnd() % copy.size(); break;
        case 1: for (int k = 0; k < 4; ++k) copy[rnd() % copy.size()] ^= static_cast<uint8_t>(1u << (rnd() % 8)); break;
        default: for (int k = 0; k < 10; ++k) copy[rnd() % copy.size()] = static_cast<uint8_t>(rnd()); break;
      }
      TestAllocator a;
      Rig2 rig;
      {
        VcsDriver driver(rig.mem);
        CHECK(driver.initialize(rig.config()));
        {
          IntelAvcDecoder session(a, driver, i420_config());
          CHECK(session.prepare());
          std::vector<int64_t> out;
          int64_t pts = 0;
          for (ConstByteView au : split(ConstByteView(copy.data(), len))) {
            feed(session, au, pts, &out, nullptr, nullptr, nullptr);
            pts += 1000;
          }
          session.flush();
          drain(session, &out, nullptr, nullptr, nullptr);
        }
      }
      CHECK(rig.gpu.violations.empty());
      CHECK(a.live == 0);
    }
  }
}

// The device: opt-in hardware decoding, one hardware session at a time, everything else on the CPU.
static void test_device() {
  File file("tests/data/h264_bframes.h264");
  CHECK(file.data != nullptr);
  if (!file.data) return;
  const std::vector<ConstByteView> aus = split(file.view());
  TestAllocator a;
  auto decode_all = [&](IVideoDecoder& dec, size_t count) {
    std::vector<int64_t> out;
    for (size_t i = 0; i < aus.size(); ++i) feed(dec, aus[i], static_cast<int64_t>(i) * 1000, &out, nullptr, nullptr, nullptr);
    dec.flush();
    drain(dec, &out, nullptr, nullptr, nullptr);
    CHECK(out.size() == count);
    return out;
  };
  {
    Rig2 rig;
    std::vector<uint32_t> regs(0x20000 / 4, 0);
    regs[0x0D88 / 4] = 1;  // the video box answers the power request
    const MmioRegion region(reinterpret_cast<uint64_t>(regs.data()), regs.size() * 4);
    IntelMediaVideoEngine::HardwareSetup setup;
    setup.registers = &rig.gpu;
    setup.ggtt_table = rig.config().ggtt_table;
    setup.ggtt_first_page = 100;
    setup.memory = &rig.mem;
    {
      IntelMediaVideoEngine device(a, 0x3E92, region);  // Coffee Lake
      device.set_hardware(setup);
      CHECK(device.initialize());
      CHECK(device.hardware_decode() && device.info().accelerated_decode_codecs == (codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Mpeg2) | codec_mask(VideoCodec::Vc1) | codec_mask(VideoCodec::Hevc)) && !device.info().accelerated);
      std::vector<int64_t> hw_pts, sw_pts;
      {
        UniquePointer<IVideoDecoder> hw = device.create_decoder(i420_config());
        CHECK(hw != nullptr);
        hw_pts = decode_all(*hw, 14);
        CHECK(rig.engine.pictures == 14);
        // The engine is taken: the next session runs on the CPU, and sends nothing to the engine.
        UniquePointer<IVideoDecoder> sw = device.create_decoder(i420_config());
        CHECK(sw != nullptr);
        sw_pts = decode_all(*sw, 14);
        CHECK(rig.engine.pictures == 14);
        CHECK(sw_pts == hw_pts);
        // Other codecs are the CPU's too.
        DecoderConfig jpeg;
        jpeg.codec = VideoCodec::Mjpeg;
        CHECK(device.create_decoder(jpeg) != nullptr);
      }
      UniquePointer<IVideoDecoder> again = device.create_decoder(i420_config());  // the engine is free again
      CHECK(again != nullptr);
      decode_all(*again, 14);
      CHECK(rig.engine.pictures == 28);
    }
    CHECK(rig.gpu.violations.empty() && rig.engine.problems == 0);
    CHECK(rig.mem.live() == 1);  // the device took its driver down with it
  }
  CHECK(a.live == 0);
  {
    // Not opted in, an unknown generation, or a video box that does not answer: all on the CPU.
    Rig2 rig;
    std::vector<uint32_t> regs(0x20000 / 4, 0);
    regs[0x0D88 / 4] = 1;
    const MmioRegion region(reinterpret_cast<uint64_t>(regs.data()), regs.size() * 4);
    IntelMediaVideoEngine::HardwareSetup setup;
    setup.registers = &rig.gpu;
    setup.ggtt_table = rig.config().ggtt_table;
    setup.ggtt_first_page = 100;
    setup.memory = &rig.mem;
    IntelMediaVideoEngine plain(a, 0x3E92, region);
    CHECK(plain.initialize() && !plain.hardware_decode());
    IntelMediaVideoEngine xelp(a, 0x9A49, region);
    xelp.set_hardware(setup);
    CHECK(xelp.initialize() && !xelp.hardware_decode());
    std::vector<uint32_t> silent(0x20000 / 4, 0);
    IntelMediaVideoEngine deaf(a, 0x3E92, MmioRegion(reinterpret_cast<uint64_t>(silent.data()), silent.size() * 4));
    deaf.set_hardware(setup);
    CHECK(deaf.initialize() && !deaf.hardware_decode());
    UniquePointer<IVideoDecoder> sw = plain.create_decoder(i420_config());
    CHECK(sw != nullptr);
    decode_all(*sw, 14);
    CHECK(rig.engine.pictures == 0 && rig.mem.blocks.empty() && rig.ggtt_untouched());
  }
  CHECK(a.live == 0);
}

// A GPU as the PCI bus and the mapper show it, with plain memory behind the BAR: the plumbing finds the registers and the global GTT, picks free entries, and (only when
// asked to) starts the engine driver. Plain memory cannot play the engine, so this checks the setup, not the decoding.
struct PlainGpuPci final : IPciBus {
  const char* name() const override { return "plain gpu pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 1; }
  bool pci_device(uint32_t index, PciDeviceInfo* out) const override {
    if (index != 0) return false;
    *out = {{0, 2, 0}, 0x8086, 0x3E92, kPciClassDisplay, 0, 0};
    return true;
  }
  uint32_t read32(PciAddress, uint16_t offset) const override { return offset == 0x04 ? 2u : offset == 0x50 ? 0x0080u : 0u; }
  void write32(PciAddress, uint16_t, uint32_t) const override {}
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0x1'0000'0000ull; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

struct PlainMapper final : IRegisterMapper {
  std::vector<uint32_t> registers = std::vector<uint32_t>(0x200000 / 4, 0);
  std::vector<uint64_t> table = std::vector<uint64_t>((4u << 20) / 8, 0);
  int calls = 0;
  PlainMapper() {
    registers[0x0D88 / 4] = 1;     // the video box
    registers[0x130044 / 4] = 1;   // the GT well
  }
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    ++calls;
    if (physical == 0x1'0000'0000ull && length <= registers.size() * 4) *region = MmioRegion(reinterpret_cast<uint64_t>(registers.data()), length);
    else if (physical == 0x1'0000'0000ull + (8u << 20) && length == table.size() * 8) *region = MmioRegion(reinterpret_cast<uint64_t>(table.data()), length);
    else return false;
    return true;
  }
};

static void test_system() {
  for (int enable = 0; enable < 2; ++enable) {
    TestAllocator a;
    PlainGpuPci pci;
    PlainMapper mapper;
    FakeMemory memory;
    {
      SystemVideoCodecs system(pci, a);
      system.set_mmio(&mapper);
      system.set_intel_gpu_memory(&memory);
      system.set_intel_hardware_decode(enable != 0);
      system.initialize();
      CHECK(system.count() == 2);
      IntelMediaVideoEngine* intel = static_cast<IntelMediaVideoEngine*>(system.at(0));
      CHECK(strstr(intel->name(), "Intel"));
      CHECK(intel->hardware_decode() == (enable != 0));
      CHECK(intel->engine_status().probed && intel->engine_status().vdbox_count() == 1);
      uint64_t written = 0, first = 0;
      for (uint64_t i = 0; i < mapper.table.size(); ++i)
        if (mapper.table[i]) { if (!written) first = i; ++written; }
      if (enable) {
        CHECK(written == 8 && first == mapper.table.size() - 16 - 8);  // eight entries just below the guard at the top
        CHECK(system.decode_codecs() & codec_mask(VideoCodec::H264));
        CHECK(intel->info().accelerated_decode_codecs == (codec_mask(VideoCodec::H264) | codec_mask(VideoCodec::Mpeg2) | codec_mask(VideoCodec::Vc1) | codec_mask(VideoCodec::Hevc)));
        CHECK(mapper.calls >= 3);
      } else {
        CHECK(written == 0 && intel->info().accelerated_decode_codecs == 0);
      }
      // Whatever the engine did, the setup used plain memory and left no other trace: the registers are written, nothing else.
      system.shutdown();
    }
    CHECK(a.live == 0);
  }
}

int main() {
  const char* names[] = {"h264_intra.h264", "h264_cavlc.h264", "h264_cqm.h264", "h264_noise.h264", "h264_pcm_cavlc.h264", "h264_pcm_cabac.h264", "h264_bframes.h264", "h264_ltr.h264"};
  for (const char* n : names) compare_stream(n);
  test_lease();
  test_hang_and_reset();
  test_resize();
  test_damaged_streams();
  test_device();
  test_system();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("intel avc session tests passed\n");
  return 0;
}
