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


// Host-side tests for the VCE H.264 encode session (AmdVce3Encoder) against a simulated engine: a ring controller that reads the commands the driver writes and runs the
// indirect buffer, and a stand-in for the firmware that parses the task blocks the way the firmware does (sizes, ids, the fields at the dwords they must be at), checks them
// (one session handle, create before encode, buffers inside the work memory, the reference the picture says it uses, the feedback buffer named), and then "encodes" with the CPU
// H.264 encoder: it reads the limited-range NV12 picture the engine was given, expands it to full range, encodes it as an IDR or P picture and writes the access unit into the
// bitstream buffer and its length into the feedback buffer. The session must hand back packets the CPU decoder turns into pictures close to the ones that went in.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <functional>
#include <memory>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdVce3Encoder.hpp"
#include "Hardware/VideoCodec/Software/H264.hpp"
#include "Hardware/VideoCodec/Software/H264Encoder.hpp"
#include "FakeAmdVcn.hpp"

using namespace hardware;
using namespace hardware::amd_vce3;

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

namespace reg {
constexpr uint32_t kRptr[3] = {0x8063, 0x805e, 0x80d7}, kWptr[3] = {0x8064, 0x805f, 0x80d8};
constexpr uint32_t kGrbmGfxIndex = 0xc200;
}  // namespace reg
constexpr uint32_t kRingDwords = 1024;

struct Window {
  uint64_t gpu = 0;
  uint8_t* host = nullptr;
  uint64_t size = 0;
};

struct FakeMapper final : IRegisterMapper {
  uint8_t* host = nullptr;
  uint64_t physical_base = 0, size = 0;
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    if (physical < physical_base || length > size || physical - physical_base > size - length) return false;
    *region = MmioRegion(reinterpret_cast<uint64_t>(host + (physical - physical_base)), length);
    return true;
  }
};

// The registers of the ring pointers and the controller that runs the commands (as in AmdVce3RingTest), with a hook that gets each indirect buffer.
struct Controller final : IVcnRegisters {
  std::vector<Window> windows;
  uint64_t ring_gpu = 0;
  uint32_t regs[2][4096] = {};
  uint32_t grbm = 0xE0000000;
  bool stall = false;
  uint32_t protocol_errors = 0;
  std::function<void(const std::vector<uint32_t>&)> on_ib;
  uint32_t selected() const { return (grbm >> 4) & 1; }
  uint32_t read(uint32_t r) override {
    if (r == reg::kGrbmGfxIndex) return grbm;
    return r >= 0x8000 && r < 0x9000 ? regs[selected()][r - 0x8000] : 0;
  }
  void write(uint32_t r, uint32_t v) override {
    if (r == reg::kGrbmGfxIndex) { grbm = v; return; }
    if (r >= 0x8000 && r < 0x9000) regs[selected()][r - 0x8000] = v;
    if (r == reg::kWptr[0]) run();
  }
  uint8_t* translate(uint64_t gpu, uint64_t bytes) const {
    for (const Window& w : windows)
      if (gpu >= w.gpu && gpu - w.gpu <= w.size && bytes <= w.size - (gpu - w.gpu)) return w.host + (gpu - w.gpu);
    return nullptr;
  }
  uint32_t get32(uint64_t gpu) const {
    const uint8_t* p = translate(gpu, 4);
    return p ? p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24) : 0xDEADBEEF;
  }
  void put32(uint64_t gpu, uint32_t v) const {
    uint8_t* p = translate(gpu, 4);
    if (!p) return;
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
  }
  void set_pointers(uint32_t position) { for (int i = 0; i < 2; ++i) regs[i][reg::kRptr[0] - 0x8000] = regs[i][reg::kWptr[0] - 0x8000] = position; }

 private:
  void run() {
    uint32_t& rptr = regs[selected()][reg::kRptr[0] - 0x8000];
    const uint32_t wptr = regs[selected()][reg::kWptr[0] - 0x8000];
    auto next = [&]() { const uint32_t v = get32(ring_gpu + uint64_t{rptr} * 4); rptr = (rptr + 1) % kRingDwords; return v; };
    while (rptr != wptr) {
      const uint32_t cmd = next();
      switch (cmd) {
        case 0: case 1: case 4: break;
        case 0x102: {
          const uint32_t vmid = next(), lo = next(), hi = next(), length = next();
          if (vmid != 0) ++protocol_errors;
          std::vector<uint32_t> words;
          const uint64_t address = lo | (uint64_t{hi} << 32);
          if (!translate(address, uint64_t{length} * 4)) { ++protocol_errors; break; }
          for (uint32_t i = 0; i < length; ++i) words.push_back(get32(address + uint64_t{i} * 4));
          if (on_ib) on_ib(words);
          break;
        }
        case 3: {
          const uint32_t lo = next(), hi = next(), seq = next();
          if (!stall) put32(lo | (uint64_t{hi} << 32), seq);
          break;
        }
        default: ++protocol_errors; rptr = wptr; return;
      }
    }
  }
};

// The stand-in for the firmware.
struct Firmware {
  Controller* ctrl = nullptr;
  allocators::IAllocator* alloc = nullptr;
  std::unique_ptr<H264Encoder> encoder;
  uint32_t handle = 0;
  bool created = false, destroyed = false, opened_session = false;
  uint32_t width = 0, height = 0, ref_pitch = 0;
  uint32_t rc_method = 99, qp_i = 0, qp_p = 0, gop = 0, poc_lsb_minus4 = 99;
  int sessions = 0, creates = 0, configs = 0, encodes = 0, destroys = 0, problems = 0;
  bool no_bitstream = false;
  struct Encoded { uint32_t type, frame_num, poc, ref_type, ref_frame, ref_luma, cur_luma, bytes; bool headers; uint32_t cpb_hi, cpb_lo; };
  std::vector<Encoded> pictures;
  std::vector<uint32_t> blocks_seen;   // the command ids in order

  void problem(const char* what) { ++problems; printf("  fake firmware: %s\n", what); }

  void on_ib(const std::vector<uint32_t>& w) {
    size_t at = 0;
    uint64_t feedback_address = 0;
    uint32_t bs_size = 0, cpb_hi = 0, cpb_lo = 0;
    uint64_t bs_address = 0;
    bool encode_pending = false;
    std::vector<uint32_t> encode;
    uint32_t ring_index = 0;
    while (at < w.size()) {
      const uint32_t bytes = w[at];
      if (bytes == 0) { ++at; continue; }   // padding
      if (bytes % 4 != 0 || bytes < 8 || at + bytes / 4 > w.size()) { problem("block size is not a whole number of dwords inside the buffer"); return; }
      const uint32_t id = w[at + 1];
      const uint32_t n = bytes / 4 - 2;   // payload dwords
      const uint32_t* p = &w[at + 2];
      blocks_seen.push_back(id);
      switch (id) {
        case 0x00000001: ++sessions; if (n != 1) problem("session block length"); else handle = p[0]; opened_session = true; break;
        case 0x00000002: if (n != 6) problem("task info length"); else { ring_index = p[5]; } break;
        case 0x01000001:
          ++creates;
          if (!opened_session) problem("create before session");
          if (n != 14) { problem("create length"); break; }
          created = true;
          width = p[4]; height = p[5]; ref_pitch = p[6];
          if (p[1] != 66 || p[7] != p[6]) problem("create profile or pitches");
          break;
        case 0x02000001: ++destroys; destroyed = true; break;
        case 0x04000005: ++configs; if (n != 26) problem("rate control length"); else { rc_method = p[0]; gop = p[4]; qp_i = p[5]; qp_p = p[6]; } break;
        case 0x04000001: if (n != 1) problem("config extension length"); break;
        case 0x04000007: if (n != 24) problem("motion estimation length"); break;
        case 0x04000008: if (n != 17) problem("rdo length"); break;
        case 0x04000002: if (n != 27) problem("pic control length"); else poc_lsb_minus4 = p[15]; break;
        case 0x05000001: if (n != 2) problem("context buffer length"); else { cpb_hi = p[0]; cpb_lo = p[1]; } break;
        case 0x05000004: if (n != 3) problem("bitstream buffer length"); else { bs_address = (uint64_t{p[0]} << 32) | p[1]; bs_size = p[2]; } break;
        case 0x05000005: if (n != 3) problem("feedback length"); else feedback_address = (uint64_t{p[0]} << 32) | p[1]; break;
        case 0x03000001: if (n != 96) problem("encode length"); else { encode_pending = true; encode.assign(p, p + n); } break;
        default: problem("unknown command id"); return;
      }
      at += bytes / 4;
    }
    if (!encode_pending) return;
    if (!created || p_handle_mismatch()) { problem("encode before create, or another session"); return; }
    run_encode(encode, bs_address - uint64_t{ring_index} * 0 + uint64_t{ring_index} * bs_size, bs_size, feedback_address, cpb_hi, cpb_lo);
  }
  bool p_handle_mismatch() const { return handle == 0; }

  // The encode block's fields by their place (counted from Mesa's radeon_vce_52.c): 1-based `n` is the n-th payload dword.
  void run_encode(const std::vector<uint32_t>& e, uint64_t bs_address, uint32_t bs_size, uint64_t feedback_address, uint32_t cpb_hi, uint32_t cpb_lo) {
    auto f = [&](uint32_t n) { return e[n - 1]; };
    ++encodes;
    const uint64_t luma = (uint64_t{f(8)} << 32) | f(9), chroma = (uint64_t{f(10)} << 32) | f(11);
    const uint32_t frame_y_pitch = f(12), luma_pitch = f(13), chroma_pitch = f(14);
    const uint32_t type = f(17), idr = f(18);
    if (frame_y_pitch < height || luma_pitch != ref_pitch || chroma_pitch != ref_pitch || f(15) != 0x00010000) problem("encode pitches");
    if ((type == 3) != (idr == 1) || f(1) != (f(80) == 0 ? 0x11u : 0u)) problem("encode picture type, idr flag or headers");
    Encoded rec{type, f(80), f(81), f(55), f(56), f(58), f(72), 0, f(1) == 0x11, cpb_hi, cpb_lo};
    if (type == 0) {
      // A P picture: its first L0 entry names the previous picture, and the picture goes to another place in the buffer than its reference.
      if (rec.ref_luma == 0xffffffff || rec.ref_luma == rec.cur_luma) problem("P picture reference");
      // The reference is the previous picture, so the reference list needs no modification (dwords 26 and 27).
      if (f(26) != 0 || f(27) != 0) problem("P picture asks for a reference list modification");
    } else if (f(58) != 0xffffffff) {
      problem("IDR picture with a reference");
    }
    // The input: limited-range NV12 in the engine's memory, expanded to a full-range I420 picture for the CPU encoder.
    VideoFrameBuffer i420(*alloc);
    if (!i420.allocate({width, height, PixelFormat::I420}) || !ctrl->translate(luma, uint64_t{luma_pitch} * height) || !ctrl->translate(chroma, uint64_t{chroma_pitch} * height / 2)) {
      problem("input pictures outside the engine's memory");
      return;
    }
    const uint8_t* ly = ctrl->translate(luma, uint64_t{luma_pitch} * height);
    const uint8_t* lc = ctrl->translate(chroma, uint64_t{chroma_pitch} * height / 2);
    ByteView y = i420.plane(0), u = i420.plane(1), v = i420.plane(2);
    auto clamp = [](int x) { return static_cast<uint8_t>(x < 0 ? 0 : x > 255 ? 255 : x); };
    for (uint32_t r = 0; r < height; ++r)
      for (uint32_t c = 0; c < width; ++c) y[uint64_t{r} * width + c] = clamp(((ly[uint64_t{r} * luma_pitch + c] - 16) * 255 + 109) / 219);
    for (uint32_t r = 0; r < height / 2; ++r)
      for (uint32_t c = 0; c < width / 2; ++c) {
        u[uint64_t{r} * (width / 2) + c] = clamp(128 + ((lc[uint64_t{r} * chroma_pitch + c * 2] - 128) * 255) / 224);
        v[uint64_t{r} * (width / 2) + c] = clamp(128 + ((lc[uint64_t{r} * chroma_pitch + c * 2 + 1] - 128) * 255) / 224);
      }
    if (!encoder) {
      encoder.reset(new H264Encoder(*alloc));
      if (!encoder->configure(width, height, 30, 1, 90, 1000000)) { problem("CPU encoder"); return; }
    }
    ByteBuffer out(*alloc);
    bool key = false;
    if (!encoder->encode(i420.frame(0, true), type == 3, &out, &key)) { problem("CPU encode"); return; }
    uint8_t* bs = ctrl->translate(bs_address, out.size());
    uint8_t* fb = ctrl->translate(feedback_address, 40);
    if (!bs || !fb || out.size() > bs_size) { problem("bitstream or feedback outside the memory, or too small"); return; }
    memcpy(bs, out.view().data(), out.size());
    rec.bytes = static_cast<uint32_t>(out.size());
    pictures.push_back(rec);
    if (no_bitstream) return;
    ctrl->put32(feedback_address + 0, 1);                              // encStatus
    ctrl->put32(feedback_address + 4, 1);                              // encHasBitstream
    ctrl->put32(feedback_address + 12, 0);                             // encBitstreamOffset
    ctrl->put32(feedback_address + 16, rec.bytes);                     // encBitstreamSize
    ctrl->put32(feedback_address + 36, 0);                             // (the entry the size is reduced by)
  }
};

struct Rig {
  static constexpr uint64_t kArena = 16ull << 20;
  static constexpr uint64_t kGpuBase = 0x4'0000'0000ull, kPhysicalBase = 0xE000'0000ull;
  TestAllocator alloc;
  std::vector<uint8_t> arena;
  FakeMapper mapper;
  AmdVcnWorkMemory memory;
  Controller ctrl;
  Firmware fw;
  fakevcn::FakeClock clock;
  std::unique_ptr<AmdVce3Ring> ring;
  uint8_t *ring_page, *ib_page, *fence_page;
  explicit Rig(uint64_t arena_bytes = kArena) : arena(arena_bytes) {
    mapper.host = arena.data();
    mapper.physical_base = kPhysicalBase;
    mapper.size = arena.size();
    memory.configure(&mapper, kGpuBase, kPhysicalBase, arena.size());
    ctrl.windows.push_back({kGpuBase, arena.data(), arena.size()});
    ring_page = static_cast<uint8_t*>(aligned_alloc(4096, 4096));
    ib_page = static_cast<uint8_t*>(aligned_alloc(4096, 4096));
    fence_page = static_cast<uint8_t*>(aligned_alloc(4096, 4096));
    memset(ring_page, 0, 4096); memset(ib_page, 0, 4096); memset(fence_page, 0, 4096);
    ctrl.ring_gpu = 0x6'0000'0000ull;
    ctrl.windows.push_back({ctrl.ring_gpu, ring_page, 4096});
    ctrl.windows.push_back({0x6'1000'0000ull, ib_page, 4096});
    ctrl.windows.push_back({0x6'1001'0000ull, fence_page, 4096});
    ctrl.set_pointers(0);
    AmdVce3Ring::Memory m;
    m.ib_gpu_address = 0x6'1000'0000ull;
    m.ib = MmioRegion(reinterpret_cast<uint64_t>(ib_page), 4096);
    m.fence_gpu_address = 0x6'1001'0000ull;
    m.fence = MmioRegion(reinterpret_cast<uint64_t>(fence_page), 4096);
    m.ring = MmioRegion(reinterpret_cast<uint64_t>(ring_page), 4096);
    ring.reset(new AmdVce3Ring(ctrl, clock, m, 0, AmdVce3::kHarvestVce1));
    fw.ctrl = &ctrl;
    fw.alloc = &alloc;
    ctrl.on_ib = [this](const std::vector<uint32_t>& w) { fw.on_ib(w); };
  }
  ~Rig() { free(ring_page); free(ib_page); free(fence_page); }
  AmdVce3Encoder::Engine engine(uint32_t handle = 5) {
    AmdVce3Encoder::Engine e;
    e.ring = ring.get();
    e.memory = &memory;
    e.stream_handle = handle;
    e.job_timeout_ms = 20;
    return e;
  }
};

// The pictures that go in: a gradient and a moving square, full-range I420.
static void paint(VideoFrameBuffer* frame, uint32_t width, uint32_t height, uint32_t picture) {
  ByteView y = frame->plane(0), u = frame->plane(1), v = frame->plane(2);
  for (uint32_t r = 0; r < height; ++r)
    for (uint32_t c = 0; c < width; ++c) {
      uint32_t value = 40 + c * 2 + r;
      const uint32_t left = 8 + picture * 2, top = 12 + picture;
      if (c >= left && c < left + 20 && r >= top && r < top + 20) value = 200 - (c - left) * 3;
      y[uint64_t{r} * width + c] = static_cast<uint8_t>(value);
    }
  for (uint32_t r = 0; r < height / 2; ++r)
    for (uint32_t c = 0; c < width / 2; ++c) {
      u[uint64_t{r} * (width / 2) + c] = static_cast<uint8_t>(96 + c * 2 + picture);
      v[uint64_t{r} * (width / 2) + c] = static_cast<uint8_t>(160 - r * 2);
    }
}

static double psnr(const VideoFrame& a, const VideoFrame& b) {
  double sum = 0;
  uint64_t n = 0;
  for (uint32_t r = 0; r < a.format.height; ++r)
    for (uint32_t c = 0; c < a.format.width; ++c) {
      const double d = double(a.plane[0][uint64_t{r} * a.stride[0] + c]) - double(b.plane[0][uint64_t{r} * b.stride[0] + c]);
      sum += d * d;
      ++n;
    }
  if (sum == 0) return 99;
  const double mse = sum / double(n);
  double db = 0;
  // 10 * log10(255^2 / mse), by repeated division (no math library in the freestanding tests' way)
  double ratio = 255.0 * 255.0 / mse;
  while (ratio >= 10.0) { ratio /= 10.0; db += 10.0; }
  while (ratio < 1.0) { ratio *= 10.0; db -= 10.0; }
  // ratio in [1, 10): 10*log10(ratio) by a short series of squarings
  double x = ratio, result = 0, step = 5.0;
  for (int i = 0; i < 12; ++i) {   // binary search on 10^(t/10)
    double lo = 0;
    (void)lo;
    break;
  }
  (void)x; (void)step;
  // table of 10*log10 at 1..9
  static const double kTable[10] = {0, 0.0, 3.0103, 4.7712, 6.0206, 6.9897, 7.7815, 8.4510, 9.0309, 9.5424};
  const int k = static_cast<int>(ratio);
  result = kTable[k < 1 ? 1 : k > 9 ? 9 : k];
  return db + result;
}

static void test_session(PixelFormat input, uint32_t interval, uint32_t pictures, const char* name) {
  Rig rig;
  {
    EncoderConfig config;
    config.codec = VideoCodec::H264;
    config.width = 64;
    config.height = 48;
    config.input = input;
    config.quality = 75;
    config.keyframe_interval = interval;
    AmdVce3Encoder encoder(rig.alloc, rig.engine(), config);
    CHECK(encoder.prepare());
    H264Decoder decoder(rig.alloc);
    VideoFrameBuffer source(rig.alloc), converted(rig.alloc), frame_in(rig.alloc);
    CHECK(source.allocate({64, 48, PixelFormat::I420}) && converted.allocate({64, 48, PixelFormat::I420}) && frame_in.allocate({64, 48, input}));
    uint32_t keyframes = 0, decoded = 0;
    for (uint32_t i = 0; i < pictures; ++i) {
      paint(&source, 64, 48, i);
      VideoFrame in = source.frame(int64_t{i} * 1000, true);
      in.limited_range = false;
      if (input != PixelFormat::I420) {
        CHECK(convert_frame(in, frame_in));
        in = frame_in.frame(int64_t{i} * 1000, true);
      }
      CHECK(encoder.submit(in) == SubmitResult::Accepted);
      // One picture is held at a time: the next is refused until it is taken.
      CHECK(encoder.submit(in) == SubmitResult::Full);
      VideoPacket packet;
      VideoPacket again;
      CHECK(encoder.receive(&packet) && !encoder.receive(&again));
      CHECK(packet.pts_us == int64_t{i} * 1000);
      const bool expect_key = interval <= 1 || i % interval == 0;
      CHECK(packet.keyframe == expect_key);
      keyframes += packet.keyframe;
      CHECK(rig.fw.pictures.size() == i + 1 && rig.fw.pictures.back().bytes == packet.data.size());
      CHECK(rig.fw.pictures.back().headers == expect_key);
      CHECK(rig.fw.pictures.back().type == (expect_key ? 3u : 0u));
      // The CPU decoder turns the packet into a picture close to the one that went in.
      CHECK(decoder.decode(packet.data, packet.pts_us));
      VideoFrame out;
      if (decoder.receive(&out)) {
        ++decoded;
        VideoFrame ref = source.frame(0, true);
        const double db = psnr(ref, out);
        if (db < 28.0) printf("  %s: picture %u at %.1f dB\n", name, i, db);
        CHECK(db >= 28.0);
      }
    }
    decoder.flush();
    VideoFrame tail;
    while (decoder.receive(&tail)) ++decoded;
    CHECK(decoded == pictures);
    CHECK(keyframes == (interval <= 1 ? pictures : (pictures + interval - 1) / interval));
    CHECK(encoder.statistics().frames == pictures && encoder.statistics().packets == pictures && encoder.statistics().errors == 0 && encoder.pictures_encoded() == pictures);
    // The firmware saw one session, one create, one configuration; the quantiser from the quality (QP 24 at 75), the group of pictures and the stream's handle.
    CHECK(rig.fw.sessions >= 1 && rig.fw.creates == 1 && rig.fw.configs == 1 && rig.fw.encodes == int(pictures) && rig.fw.problems == 0 && rig.ctrl.protocol_errors == 0);
    CHECK(rig.fw.handle == 5 && rig.fw.width == 64 && rig.fw.height == 48 && rig.fw.rc_method == 0 && rig.fw.qp_i == 24 && rig.fw.qp_p == 24 && rig.fw.gop == (interval < 1 ? 1u : interval));
    CHECK(rig.fw.poc_lsb_minus4 == 4);
    CHECK(rig.fw.destroys == 0);
  }
  // Closing the session destroys the engine's.
  CHECK(rig.fw.destroys == 1);
  CHECK(rig.fw.problems == 0);
  rig.fw.encoder.reset();   // the stand-in's own encoder holds allocations
  CHECK(rig.alloc.live == 0);
  CHECK(rig.ctrl.protocol_errors == 0);
}

static void test_group_of_pictures() {
  Rig rig;
  {
    EncoderConfig config;
    config.codec = VideoCodec::H264;
    config.width = 64;
    config.height = 48;
    config.input = PixelFormat::I420;
    config.keyframe_interval = 4;
    AmdVce3Encoder encoder(rig.alloc, rig.engine(), config);
    CHECK(encoder.prepare());
    VideoFrameBuffer source(rig.alloc);
    CHECK(source.allocate({64, 48, PixelFormat::I420}));
    for (uint32_t i = 0; i < 9; ++i) {
      paint(&source, 64, 48, i);
      VideoPacket packet;
      CHECK(encoder.submit(source.frame(i, true)) == SubmitResult::Accepted && encoder.receive(&packet));
    }
    const PictureLayout layout = picture_layout(64, 48);
    // IDR, P, P, P, IDR, P, P, P, IDR: frame numbers and order counts restart at each IDR; a P picture refers to the one before, which is in another slot of the buffer.
    const uint32_t types[9] = {3, 0, 0, 0, 3, 0, 0, 0, 3};
    const uint32_t frame_nums[9] = {0, 1, 2, 3, 0, 1, 2, 3, 0};
    for (uint32_t i = 0; i < 9; ++i) {
      const Firmware::Encoded& e = rig.fw.pictures[i];
      CHECK(e.type == types[i] && e.frame_num == frame_nums[i] && e.poc == 2 * frame_nums[i]);
      if (e.type == 0) {
        CHECK(e.ref_type == (frame_nums[i] == 1 ? 3u : 0u) && e.ref_frame == frame_nums[i] - 1);
        CHECK(e.ref_luma == rig.fw.pictures[i - 1].cur_luma && e.ref_luma != e.cur_luma);
      }
      CHECK(e.cur_luma % layout.frame_bytes == 0);   // a slot of the coded picture buffer
    }
    CHECK(rig.fw.pictures[4].cur_luma == 0 && rig.fw.pictures[0].cur_luma == 0);   // an IDR starts at the first slot again
  }
}

static void test_refusals_and_failures() {
  {  // Configurations the engine does not take.
    Rig rig;
    EncoderConfig c;
    c.codec = VideoCodec::H264;
    c.width = 64;
    c.height = 48;
    c.input = PixelFormat::I420;
    { EncoderConfig bad = c; bad.codec = VideoCodec::Mjpeg; AmdVce3Encoder e(rig.alloc, rig.engine(), bad); CHECK(!e.prepare()); }
    { EncoderConfig bad = c; bad.width = 63; AmdVce3Encoder e(rig.alloc, rig.engine(), bad); CHECK(!e.prepare()); }
    { EncoderConfig bad = c; bad.height = 8; AmdVce3Encoder e(rig.alloc, rig.engine(), bad); CHECK(!e.prepare()); }
    { EncoderConfig bad = c; bad.fps_denominator = 0; AmdVce3Encoder e(rig.alloc, rig.engine(), bad); CHECK(!e.prepare()); }
    { AmdVce3Encoder::Engine none; AmdVce3Encoder e(rig.alloc, none, c); CHECK(!e.prepare()); }
    // The engine serves one session at a time.
    AmdVce3Encoder first(rig.alloc, rig.engine(1), c);
    CHECK(first.prepare());
    AmdVce3Encoder second(rig.alloc, rig.engine(2), c);
    CHECK(!second.prepare());
    // A picture of another size or format, or before prepare, is refused.
    VideoFrameBuffer wrong(rig.alloc);
    CHECK(wrong.allocate({32, 32, PixelFormat::I420}));
    CHECK(first.submit(wrong.frame(0, true)) == SubmitResult::Invalid && first.statistics().errors == 1);
    VideoFrameBuffer other_format(rig.alloc);
    CHECK(other_format.allocate({64, 48, PixelFormat::Nv12}));
    CHECK(first.submit(other_format.frame(0, true)) == SubmitResult::Invalid);
    AmdVce3Encoder unprepared(rig.alloc, rig.engine(3), c);
    VideoFrameBuffer ok(rig.alloc);
    CHECK(ok.allocate({64, 48, PixelFormat::I420}) && unprepared.submit(ok.frame(0, true)) == SubmitResult::Invalid);
  }
  {  // Too little work memory.
    Rig rig(64 * 1024);
    EncoderConfig c;
    c.codec = VideoCodec::H264;
    c.width = 64;
    c.height = 48;
    c.input = PixelFormat::I420;
    AmdVce3Encoder e(rig.alloc, rig.engine(), c);
    CHECK(!e.prepare());
  }
  {  // The engine never finishes: the picture is Invalid, the ring is wedged, everything after is refused without another job.
    Rig rig;
    EncoderConfig c;
    c.codec = VideoCodec::H264;
    c.width = 64;
    c.height = 48;
    c.input = PixelFormat::I420;
    AmdVce3Encoder e(rig.alloc, rig.engine(), c);
    CHECK(e.prepare());
    VideoFrameBuffer src(rig.alloc);
    CHECK(src.allocate({64, 48, PixelFormat::I420}));
    paint(&src, 64, 48, 0);
    rig.ctrl.stall = true;
    CHECK(e.submit(src.frame(0, true)) == SubmitResult::Invalid && rig.ring->wedged());
    const int encodes = rig.fw.encodes;
    CHECK(e.submit(src.frame(1, true)) == SubmitResult::Invalid && rig.fw.encodes == encodes);
    VideoPacket packet;
    CHECK(!e.receive(&packet) && e.statistics().errors == 2 && e.pictures_encoded() == 0);
  }
  {  // The firmware reports no bitstream: the picture is Invalid, but the session goes on.
    Rig rig;
    EncoderConfig c;
    c.codec = VideoCodec::H264;
    c.width = 64;
    c.height = 48;
    c.input = PixelFormat::I420;
    AmdVce3Encoder e(rig.alloc, rig.engine(), c);
    CHECK(e.prepare());
    VideoFrameBuffer src(rig.alloc);
    CHECK(src.allocate({64, 48, PixelFormat::I420}));
    paint(&src, 64, 48, 0);
    rig.fw.no_bitstream = true;
    CHECK(e.submit(src.frame(0, true)) == SubmitResult::Invalid && !rig.ring->wedged());
    rig.fw.no_bitstream = false;
    CHECK(e.submit(src.frame(1, true)) == SubmitResult::Accepted);
  }
}

int main() {
  test_session(PixelFormat::I420, 3, 7, "I420");
  test_session(PixelFormat::Bgra8888, 1, 4, "Bgra");
  test_session(PixelFormat::I420, 0, 3, "every picture a keyframe");
  test_group_of_pictures();
  test_refusals_and_failures();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("amd vce3 encoder tests passed\n");
  return 0;
}
