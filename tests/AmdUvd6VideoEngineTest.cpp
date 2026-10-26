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


// Host-side tests for the Vega M's video engine (AmdUvd6VideoEngine) against a simulated card: the register file of one card (memory controller, graphics block, the
// UVD start behaviour of AmdUvd6Test, the decode ring controller of FakeAmdUvd6.hpp), card memory that is plain host memory behind a BAR, and a stand-in for the engine's
// firmware that "decodes" with the CPU decoder. The whole path runs: firmware from a file system, planning, start, ring test, the H.264 self-test, sessions served by
// the engine, and every way it can be refused (no firmware, no registers, an engine that does not answer, a graphics block that reports nothing, a ring that does not run,
// an engine that writes the wrong pictures or never finishes).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6VideoEngine.hpp"
#include "Hardware/VideoCodec/Software/H264.hpp"
#include "Hardware/VideoCodec/Software/H265.hpp"
#include "FakeAmdUvd6.hpp"
#include "FakeAmdVcn.hpp"

using namespace hardware;

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

static std::vector<uint8_t> read_host_file(const char* path) {
  std::vector<uint8_t> bytes;
  FILE* f = fopen(path, "rb");
  if (!f) return bytes;
  uint8_t chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
  fclose(f);
  return bytes;
}

static bool put_file(filesystem::IFileSystem& fs, const char* path, const std::vector<uint8_t>& bytes) {
  UniquePointer<filesystem::IFile> file;
  if (fs.open_file(path, filesystem::OpenMode::CreateTruncate, &file) != filesystem::Status::Ok) return false;
  uint64_t put = 0;
  return file->write(0, ConstByteView(bytes.data(), bytes.size()), &put) == filesystem::Status::Ok && put == bytes.size();
}

static uint32_t word(const uint8_t* p, uint32_t offset) { return p[offset] | (p[offset + 1] << 8) | (p[offset + 2] << 16) | (uint32_t{p[offset + 3]} << 24); }
constexpr uint32_t kMsgBytes = 3556;
constexpr uint32_t dec(uint32_t dword) { return 16 + 4 * dword; }   // a field of the decode message, by dword (struct ruvd_msg)

namespace reg {
constexpr uint32_t kStatus = 0x3daf, kSoftReset = 0x3da0, kVcpuCntl = 0x3d98, kLmiCtrl2 = 0x3d3d, kCacheBarLow = 0x3c5f, kCacheBarHigh = 0x3c5e;
constexpr uint32_t kOffset0 = 0x3d82, kSize0 = 0x3d83, kSize1 = 0x3d85, kSize2 = 0x3d87;
constexpr uint32_t kConfigMemsize = 0x150a, kFbLocation = 0x809, kGbAddrConfig = 0x263e;
}  // namespace reg

#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestHevcStream.inc"   // the H.265 stream the engine's self-test decodes

constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kAperturePhysical = 0xD000'0000ull, kApertureBytes = 256 * kMiB;

// The CPU's pictures of the self-test stream, ready to be written as NV12.
struct Expected {
  std::vector<std::vector<uint8_t>> luma, chroma;   // per picture: 64 x 64, and 32 x 32 interleaved U, V
};

static bool make_expected(allocators::IAllocator& a, Expected* out) {
  AmdVcnSelfTest::Stream stream(a);
  if (!AmdVcnSelfTest::build_stream(a, &stream)) return false;
  H264Decoder cpu(a);
  VideoFrame f;
  auto take = [&] {
    while (cpu.receive(&f)) {
      std::vector<uint8_t> y, c;
      for (uint32_t row = 0; row < f.format.plane_height(0); ++row) y.insert(y.end(), f.plane[0] + uint64_t{row} * f.stride[0], f.plane[0] + uint64_t{row} * f.stride[0] + f.format.plane_width(0));
      for (uint32_t row = 0; row < f.format.plane_height(1); ++row)
        for (uint32_t col = 0; col < f.format.plane_width(1); ++col) {
          c.push_back(f.plane[1][uint64_t{row} * f.stride[1] + col]);
          c.push_back(f.plane[2][uint64_t{row} * f.stride[2] + col]);
        }
      out->luma.push_back(y);
      out->chroma.push_back(c);
    }
  };
  for (uint32_t i = 0; i < AmdVcnSelfTest::kPictures; ++i) {
    if (!cpu.decode(stream.packet(i), int64_t{i} * 1000)) return false;
    take();
  }
  cpu.flush();
  take();
  return out->luma.size() == AmdVcnSelfTest::kPictures;
}

// The same for the H.265 stream, per picture in decode order (the picture sent k-th has presentation time k * 1000), with the order count the message must carry.
struct HevcExpected : Expected {
  std::vector<int32_t> poc;
};

struct PocRecorder final : h265::IOffload {
  std::vector<int32_t>* pocs;
  bool accepts(const h265::Sps&, const h265::Pps&) override { return true; }
  bool decode_picture(const h265::OffloadPicture& p) override { pocs->push_back(p.poc); return true; }
};

static bool make_hevc_expected(allocators::IAllocator& a, HevcExpected* out) {
  const ConstByteView stream(kHevcSelfTestStream);
  auto packet = [&](uint32_t i) {
    const uint64_t start = i == 0 ? 0 : kHevcSelfTestEnds[i - 1];
    return stream.subview(start, kHevcSelfTestEnds[i] - start);
  };
  {
    H265Decoder recorder_cpu(a);
    PocRecorder recorder;
    recorder.pocs = &out->poc;
    recorder_cpu.set_offload(&recorder);
    for (uint32_t i = 0; i < kHevcSelfTestPictures; ++i)
      if (!recorder_cpu.decode(packet(i), int64_t{i} * 1000)) return false;
    recorder_cpu.flush();
  }
  out->luma.assign(kHevcSelfTestPictures, {});
  out->chroma.assign(kHevcSelfTestPictures, {});
  H265Decoder cpu(a);
  VideoFrame f;
  uint32_t got = 0;
  auto take = [&] {
    while (cpu.receive(&f)) {
      const uint32_t k = static_cast<uint32_t>(f.pts_us / 1000);
      if (k >= kHevcSelfTestPictures) return false;
      std::vector<uint8_t>& y = out->luma[k];
      std::vector<uint8_t>& c = out->chroma[k];
      for (uint32_t row = 0; row < f.format.plane_height(0); ++row) y.insert(y.end(), f.plane[0] + uint64_t{row} * f.stride[0], f.plane[0] + uint64_t{row} * f.stride[0] + f.format.plane_width(0));
      for (uint32_t row = 0; row < f.format.plane_height(1); ++row)
        for (uint32_t col = 0; col < f.format.plane_width(1); ++col) {
          c.push_back(f.plane[1][uint64_t{row} * f.stride[1] + col]);
          c.push_back(f.plane[2][uint64_t{row} * f.stride[2] + col]);
        }
      ++got;
    }
    return true;
  };
  for (uint32_t i = 0; i < kHevcSelfTestPictures; ++i) {
    if (!cpu.decode(packet(i), int64_t{i} * 1000) || !take()) return false;
  }
  cpu.flush();
  return take() && got == kHevcSelfTestPictures && out->poc.size() == kHevcSelfTestPictures;
}

struct FakeMapper final : IRegisterMapper {
  uint8_t* host = nullptr;
  uint64_t physical_base = 0, size = 0;
  int maps = 0;
  bool refuse = false;
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    if (refuse || physical < physical_base || length > size || physical - physical_base > size - length) return false;
    ++maps;
    *region = MmioRegion(reinterpret_cast<uint64_t>(host + (physical - physical_base)), length);
    return true;
  }
};

// One card: the registers the engine reads and writes, its memory, and the stand-in firmware.
struct Card final : IVcnRegisters {
  std::unique_ptr<uint8_t[], decltype(&free)> arena{static_cast<uint8_t*>(calloc(kApertureBytes, 1)), &free};
  FakeMapper mapper;
  fakeuvd::Ring ring;
  fakevcn::FakeClock clock;
  const Expected* expected = nullptr;
  const HevcExpected* hevc = nullptr;
  std::map<uint32_t, uint64_t> buffer;
  uint64_t block_gpu = 0;
  const uint8_t* block_host = nullptr;
  uint32_t expected_first_word = 0;
  bool quiet = false;
  bool boots = true, write_pictures = true, write_hevc = true, hang_hevc = false;
  int boot_attempts = 0, creates = 0, hevc_creates = 0, destroys = 0, decodes = 0, hevc_decodes = 0, problems = 0;

  Card() {
    mapper.host = arena.get();
    mapper.physical_base = kAperturePhysical;
    mapper.size = kApertureBytes;
    ring.on_command = [this](uint32_t c, uint64_t a, bool) { on_command(c, a); };
    ring.on_engine = [this] { on_engine(); };
    // The memory controller: 4 GiB of video memory at GPU 0xF4000000.
    ring.write(reg::kConfigMemsize, 4096);
    ring.write(reg::kFbLocation, ((0xF4u + 255) << 16) | 0xF4u);
    ring.write(reg::kGbAddrConfig, 0x22011003);
  }
  uint32_t read(uint32_t r) override { return ring.read(r); }
  void write(uint32_t r, uint32_t v) override {
    const uint32_t old = ring.read(r);
    ring.write(r, v);
    if (r == reg::kSoftReset && (old & 8) && !(v & 8)) boot();
  }
  void boot() {
    ++boot_attempts;
    const bool clock_on = read(reg::kVcpuCntl) & 0x200;
    const bool not_stalled = !(read(reg::kLmiCtrl2) & 0x100);
    const uint64_t bar = (uint64_t{read(reg::kCacheBarHigh)} << 32) | read(reg::kCacheBarLow);
    uint32_t first = 0;
    if (block_host && bar == block_gpu) memcpy(&first, block_host, 4);
    const bool window = read(reg::kOffset0) == (256u >> 3) && read(reg::kSize0) != 0 && read(reg::kSize1) != 0 && read(reg::kSize2) != 0;
    if (boots && clock_on && not_stalled && window && first == expected_first_word) ring.write(reg::kStatus, read(reg::kStatus) | 2);
  }
  // Called once the plan is known: where the card's memory is for the ring controller, and what the boot looks at.
  void place(const AmdUvd6Plan& plan, ConstByteView ucode) {
    ring.windows = {{0xF400'0000ull, arena.get(), kApertureBytes}};
    ring.ring_gpu = plan.ring_gpu_address;
    block_gpu = plan.block_gpu_address;
    block_host = arena.get() + plan.block_offset;
    memcpy(&expected_first_word, ucode.data(), 4);
  }
  void problem(const char* what) {
    ++problems;
    if (quiet) return;
    printf("  fake engine: %s\n", what);
  }
  void on_command(uint32_t command, uint64_t address) {
    buffer[command] = address;
    if (command != 0) return;
    const uint8_t* m = ring.translate(address, kMsgBytes);
    if (!m) { problem("message outside the card's memory"); return; }
    if (word(m, 4) == 0) {
      if (word(m, 16) == 0x10) ++hevc_creates;
      else ++creates;
    }
    else if (word(m, 4) == 2) ++destroys;
  }
  // The H.265 picture: the message must name an order count of the stream, a slot of the picture buffer, and the slices must be behind a start code; then the CPU's picture
  // for that order count goes into the target.
  void on_hevc(const uint8_t* m, uint32_t height, uint32_t pitch, uint32_t chroma_offset) {
    ++hevc_decodes;
    if (hang_hevc) ring.stall_fence = true;
    constexpr uint32_t kCodec = dec(52), kCurrIdx = 118, kCurrPoc = 120, kRefList = 124;
    if (!hevc) { problem("no H.265 stream to stand in for"); return; }
    const int32_t poc = static_cast<int32_t>(word(m, kCodec + kCurrPoc));
    uint32_t k = 0;
    while (k < hevc->poc.size() && hevc->poc[k] != poc) ++k;
    if (k == hevc->poc.size()) { problem("H.265 message names an order count the stream does not have"); return; }
    if (m[kCodec + kCurrIdx] >= 17) problem("H.265 current slot outside the picture buffer");
    for (uint32_t i = 0; i < 16; ++i)
      if (m[kCodec + kRefList + i] >= 17 && m[kCodec + kRefList + i] != 0x7F) problem("H.265 reference slot outside the picture buffer");
    if (word(m, dec(5)) == 0 || word(m, dec(7)) == 0 || word(m, dec(2)) != 64 || height != 64) problem("H.265 message without buffer sizes");
    const uint8_t* bits = ring.translate(buffer[0x100], 4);
    if (!bits || bits[0] != 0 || bits[1] != 0 || bits[2] != 1) problem("H.265 bitstream does not start with a start code");
    if (!write_hevc) return;
    const uint64_t target_bytes = uint64_t{chroma_offset} + uint64_t{pitch} * (height / 2);
    uint8_t* target = ring.translate(buffer[2], target_bytes);
    if (!target || pitch < 64 || uint64_t{pitch} * 64 > chroma_offset) { problem("target does not hold a 64 x 64 picture"); return; }
    for (uint32_t y = 0; y < 64; ++y) memcpy(target + uint64_t{pitch} * y, hevc->luma[k].data() + y * 64, 64);
    for (uint32_t y = 0; y < 32; ++y) memcpy(target + chroma_offset + uint64_t{pitch} * y, hevc->chroma[k].data() + y * 64, 64);
  }
  void on_engine() {
    uint8_t m[kMsgBytes];
    const uint8_t* source = ring.translate(buffer[0], sizeof m);
    if (!source) { problem("decode message outside the card's memory"); return; }
    memcpy(m, source, sizeof m);
    const uint32_t height = word(m, dec(3)), pitch = word(m, dec(24)), chroma_offset = word(m, dec(30));
    if (word(m, dec(0)) == 0x10 && word(m, 4) == 1 && word(m, 0) == kMsgBytes) {
      on_hevc(m, height, pitch, chroma_offset);
      return;
    }
    const uint32_t k = static_cast<uint32_t>(decodes++);
    if (word(m, 4) != 1 || word(m, 0) != kMsgBytes || word(m, dec(0)) != 7) { problem("engine started on something that is not an H.264 decode message"); return; }
    if (!expected || !write_pictures) return;
    const uint32_t picture = k % static_cast<uint32_t>(expected->luma.size());   // (the self-test, then a stream of the same pictures)
    const uint64_t target_bytes = uint64_t{chroma_offset} + uint64_t{pitch} * (height / 2);
    uint8_t* target = ring.translate(buffer[2], target_bytes);
    if (!target || height != 64 || pitch < 64 || uint64_t{pitch} * 64 > chroma_offset) { problem("target does not hold a 64 x 64 picture"); return; }
    for (uint32_t y = 0; y < 64; ++y) memcpy(target + uint64_t{pitch} * y, expected->luma[picture].data() + y * 64, 64);
    for (uint32_t y = 0; y < 32; ++y) memcpy(target + chroma_offset + uint64_t{pitch} * y, expected->chroma[picture].data() + y * 64, 64);
  }
};

constexpr uint16_t kVegaM = 0x694C;

// The firmware file in a file system (or an empty /firmware).
static bool firmware_fs(filesystem::RamFileSystem& fs, bool with_file) {
  if (fs.create_directory("/firmware") != filesystem::Status::Ok) return false;
  if (!with_file) return true;
  const std::vector<uint8_t> image = read_host_file("firmware/vegam_uvd.bin");
  CHECK(image.size() > 300000);
  return put_file(fs, "/firmware/vegam_uvd.bin", image);
}

static DecoderConfig h264_config() {
  DecoderConfig c;
  c.codec = VideoCodec::H264;
  c.output = PixelFormat::I420;
  return c;
}

static void drain(IVideoDecoder& d, uint32_t* count, const Expected& e, bool* same) {
  VideoFrame f;
  while (d.receive(&f)) {
    if (*count < e.luma.size()) {
      for (uint32_t y = 0; y < 64 && *same; ++y)
        for (uint32_t x = 0; x < 64; ++x)
          if (f.plane[0][uint64_t{y} * f.stride[0] + x] != e.luma[*count][y * 64 + x]) { *same = false; break; }
    } else {
      *same = false;
    }
    ++*count;
  }
}

// Feeds the self-test stream to a session; true when it came out as the CPU's pictures.
static bool run_stream(allocators::IAllocator& a, IVideoDecoder& session, const Expected& expected) {
  AmdVcnSelfTest::Stream stream(a);
  if (!AmdVcnSelfTest::build_stream(a, &stream)) return false;
  uint32_t count = 0;
  bool same = true;
  for (uint32_t i = 0; i < AmdVcnSelfTest::kPictures; ++i) {
    VideoPacket p;
    p.data = stream.packet(i);
    p.pts_us = int64_t{i} * 1000;
    p.keyframe = i == 0;
    if (session.submit(p) != SubmitResult::Accepted) return false;
    drain(session, &count, expected, &same);
  }
  session.flush();
  drain(session, &count, expected, &same);
  return same && count == AmdVcnSelfTest::kPictures;
}

// Feeds the H.265 self-test stream to a session; true when every picture came out as the CPU's (by presentation time) and none was missing.
static bool run_hevc_stream(IVideoDecoder& session, const HevcExpected& e) {
  const ConstByteView stream(kHevcSelfTestStream);
  uint32_t count = 0;
  bool same = true;
  auto drain_hevc = [&] {
    VideoFrame f;
    while (session.receive(&f)) {
      const uint32_t k = static_cast<uint32_t>(f.pts_us / 1000);
      if (k >= e.luma.size()) { same = false; continue; }
      for (uint32_t y = 0; y < 64 && same; ++y)
        for (uint32_t x = 0; x < 64; ++x)
          if (f.plane[0][uint64_t{y} * f.stride[0] + x] != e.luma[k][y * 64 + x]) { same = false; break; }
      ++count;
    }
  };
  for (uint32_t i = 0; i < kHevcSelfTestPictures; ++i) {
    VideoPacket p;
    const uint64_t start = i == 0 ? 0 : kHevcSelfTestEnds[i - 1];
    p.data = stream.subview(start, kHevcSelfTestEnds[i] - start);
    p.pts_us = int64_t{i} * 1000;
    p.keyframe = i == 0;
    if (session.submit(p) != SubmitResult::Accepted) return false;
    drain_hevc();
  }
  session.flush();
  drain_hevc();
  return same && count == kHevcSelfTestPictures;
}

// Damaged H.265 streams through an engine session: every packet truncated at some point, and single bits flipped across the stream. Nothing may crash or hang (the
// sanitizers are watching), the engine must not wedge, and the session must still hand back as many pictures as it was given or fewer.
static void damaged_hevc_streams(AmdUvd6VideoEngine& engine, Card& card) {
  const ConstByteView stream(kHevcSelfTestStream);
  DecoderConfig config = h264_config();
  config.codec = VideoCodec::Hevc;
  card.quiet = true;
  const int problems_before = card.problems;
  for (uint32_t variant = 0; variant < 48; ++variant) {
    std::vector<uint8_t> bytes(stream.data(), stream.data() + stream.size());
    std::vector<uint64_t> ends(kHevcSelfTestEnds, kHevcSelfTestEnds + kHevcSelfTestPictures);
    if (variant < 12) {
      // Packet `variant / 2` ends early (halfway, or all but a few bytes).
      const uint32_t i = variant / 2;
      const uint64_t start = i == 0 ? 0 : ends[i - 1];
      const uint64_t length = ends[i] - start;
      const uint64_t keep = variant % 2 == 0 ? length / 2 : length - 3;
      bytes.erase(bytes.begin() + start + keep, bytes.begin() + ends[i]);
      for (uint32_t j = i; j < ends.size(); ++j) ends[j] -= length - keep;
    } else {
      const uint64_t at = (uint64_t{variant} * 977) % bytes.size();
      bytes[at] ^= static_cast<uint8_t>(1u << (variant % 8));
    }
    UniquePointer<IVideoDecoder> session = engine.create_decoder(config);
    CHECK(static_cast<bool>(session));
    uint32_t frames = 0;
    VideoFrame f;
    for (uint32_t i = 0; i < kHevcSelfTestPictures; ++i) {
      const uint64_t start = i == 0 ? 0 : ends[i - 1];
      VideoPacket p;
      p.data = ConstByteView(bytes.data() + start, ends[i] - start);
      p.pts_us = int64_t{i} * 1000;
      p.keyframe = i == 0;
      const SubmitResult r = session->submit(p);
      if (r == SubmitResult::Full) break;
      while (session->receive(&f)) ++frames;
    }
    session->flush();
    while (session->receive(&f)) ++frames;
    CHECK(frames <= kHevcSelfTestPictures);
  }
  card.quiet = false;
  (void)problems_before;
}

enum class Fault { None, NoFirmware, NoRegisters, SmallAperture, MapperRefuses, NoBoot, NoGbConfig, DeadRing, WrongPictures, Hang, HevcWrong, HevcHang };

static void run_case(Fault fault) {
  TestAllocator a;
  {
    Expected expected;
    CHECK(make_expected(a, &expected));
    HevcExpected hevc;
    CHECK(make_hevc_expected(a, &hevc));
    Card card;
    card.expected = &expected;
    card.hevc = &hevc;
    card.write_hevc = fault != Fault::HevcWrong;
    card.hang_hevc = fault == Fault::HevcHang;
    card.boots = fault != Fault::NoBoot;
    card.write_pictures = fault != Fault::WrongPictures;
    card.ring.stall_fence = fault == Fault::Hang;
    card.ring.dead = fault == Fault::DeadRing;
    if (fault == Fault::NoGbConfig) card.ring.write(reg::kGbAddrConfig, 0);
    card.mapper.refuse = fault == Fault::MapperRefuses;

    filesystem::RamFileSystem fs(a);
    CHECK(firmware_fs(fs, fault != Fault::NoFirmware));
    AmdUvd6VideoEngine engine(a, kVegaM);
    if (fault != Fault::NoRegisters) engine.set_register_file(card);
    engine.set_aperture(kAperturePhysical, fault == Fault::SmallAperture ? 32 * kMiB : kApertureBytes);
    const bool firmware_ok = engine.load_firmware(fs);
    CHECK(firmware_ok == (fault != Fault::NoFirmware));
    CHECK(strstr(engine.firmware_status(), "vegam_uvd.bin") != nullptr);
    VideoEngineEnvironment environment;
    environment.mapper = &card.mapper;
    const bool planned = engine.plan_engine(environment);
    const bool plannable = fault != Fault::NoFirmware && fault != Fault::NoRegisters && fault != Fault::SmallAperture && fault != Fault::MapperRefuses;
    CHECK(planned == plannable);
    if (!planned) {
      CHECK(!engine.start_hardware(card.clock) && !engine.engine_running() && engine.info().accelerated_decode_codecs == 0);
      CHECK(strstr(engine.plan_status(), "not planned") != nullptr);
      if (fault == Fault::NoFirmware) CHECK(strstr(engine.plan_status(), "no firmware") != nullptr);
      if (fault == Fault::NoRegisters) CHECK(strstr(engine.plan_status(), "registers") != nullptr);
      if (fault == Fault::SmallAperture) CHECK(strstr(engine.plan_status(), amd_uvd6_plan_result_name(AmdUvd6Plan::Result::ApertureTooSmall)) != nullptr);
      // The CPU still serves everything.
      UniquePointer<IVideoDecoder> software = engine.create_decoder(h264_config());
      CHECK(static_cast<bool>(software) && card.decodes == 0 && card.ring.writes.size() < 10);
      return;
    }
    CHECK(engine.engine_planned() && strstr(engine.plan_status(), "engine memory: block at video memory") != nullptr);
    CHECK(engine.plan().result == AmdUvd6Plan::Result::Ok && engine.plan().block_gpu_address >= 0xF400'0000ull);
    card.place(engine.plan(), engine.firmware().ucode());
    const size_t writes_before = card.ring.writes.size();
    const bool started = engine.start_hardware(card.clock);
    CHECK(started == (fault == Fault::None || fault == Fault::HevcWrong));
    CHECK(card.problems == 0 || fault == Fault::WrongPictures);
    if (fault == Fault::None || fault == Fault::HevcWrong) {
      const bool hevc_on = fault == Fault::None;
      CHECK(engine.engine_running() && engine.decode_ready() && engine.hevc_ready() == hevc_on);
      CHECK(engine.info().accelerated_decode_codecs == (codec_mask(VideoCodec::H264) | (hevc_on ? codec_mask(VideoCodec::Hevc) : 0u)));
      CHECK(strstr(engine.engine_note(), hevc_on ? "H.264 and H.265 decode run on it" : "H.265 self-test failed") != nullptr && strstr(engine.engine_summary(), "UVD 6.3") != nullptr);
      // The tests opened and closed their own sessions: H.264 (five pictures) and H.265 (six; its session is made even when the test then fails).
      CHECK(card.creates == 1 && card.hevc_creates == 1 && card.destroys == 2 && card.decodes == int(AmdVcnSelfTest::kPictures));
      CHECK(hevc_on ? card.hevc_decodes == int(kHevcSelfTestPictures) : card.hevc_decodes > 0);   // (a failing test stops at the first wrong picture)
      // H.264 sessions are the engine's: the stream decodes on it (decodes go up) and comes out as the CPU's pictures.
      {
        UniquePointer<IVideoDecoder> first = engine.create_decoder(h264_config());
        CHECK(static_cast<bool>(first));
        // The engine serves one session at a time: while the first is open, a second H.264 session is the CPU's.
        const int before = card.decodes;
        UniquePointer<IVideoDecoder> second = engine.create_decoder(h264_config());
        CHECK(static_cast<bool>(second));
        CHECK(run_stream(a, *second, expected) && card.decodes == before);
        CHECK(run_stream(a, *first, expected) && card.decodes == before + int(AmdVcnSelfTest::kPictures));
      }
      // H.265 sessions are the engine's too when its test passed, and the CPU's when it did not.
      {
        DecoderConfig hevc_config = h264_config();
        hevc_config.codec = VideoCodec::Hevc;
        const int before = card.hevc_decodes;
        UniquePointer<IVideoDecoder> first = engine.create_decoder(hevc_config);
        CHECK(static_cast<bool>(first));
        UniquePointer<IVideoDecoder> second = engine.create_decoder(hevc_config);   // (the engine is busy: the CPU's)
        CHECK(static_cast<bool>(second) && run_hevc_stream(*second, hevc) && card.hevc_decodes == before);
        CHECK(run_hevc_stream(*first, hevc) && card.hevc_decodes == before + (hevc_on ? int(kHevcSelfTestPictures) : 0));
      }
      if (hevc_on) {
        damaged_hevc_streams(engine, card);
        // Damage did not hurt the engine: a clean stream still decodes on it afterwards.
        DecoderConfig hevc_config = h264_config();
        hevc_config.codec = VideoCodec::Hevc;
        const int before = card.hevc_decodes;
        UniquePointer<IVideoDecoder> clean = engine.create_decoder(hevc_config);
        CHECK(static_cast<bool>(clean) && run_hevc_stream(*clean, hevc) && card.hevc_decodes == before + int(kHevcSelfTestPictures));
      }
      // Other codecs are the CPU's.
      DecoderConfig vp9;
      vp9.codec = VideoCodec::Vp9;
      CHECK(static_cast<bool>(engine.create_decoder(vp9)));
      engine.shutdown();
      CHECK(!engine.engine_running() && !engine.decode_ready() && engine.info().accelerated_decode_codecs == 0);
      CHECK(card.ring.writes.size() > writes_before);
    } else {
      // Not trusted, or never started: nothing running, nothing accelerated, and H.264 is still served (by the CPU).
      CHECK(!engine.engine_running() && !engine.decode_ready() && engine.info().accelerated_decode_codecs == 0);
      const char* expected_note = fault == Fault::NoBoot ? "did not answer" : fault == Fault::NoGbConfig ? "address configuration" : fault == Fault::DeadRing ? "did not run a register write" :
                                  fault == Fault::Hang || fault == Fault::HevcHang ? "engine stopped" : "self-test failed";
      CHECK(strstr(engine.engine_note(), expected_note) != nullptr);
      if (fault != Fault::NoBoot && fault != Fault::NoGbConfig) CHECK(strstr(engine.firmware_status(), "engine stopped") != nullptr || strstr(engine.firmware_status(), "stopped") != nullptr);
      const int before = card.decodes;
      UniquePointer<IVideoDecoder> software = engine.create_decoder(h264_config());
      CHECK(static_cast<bool>(software) && run_stream(a, *software, expected) && card.decodes == before);
      if (fault == Fault::NoBoot) CHECK(card.boot_attempts == 1 + 3);   // the release that starts the VCPU, then three tries (not the driver's ten)
    }
  }
  CHECK(a.live == 0);
}

int main() {
  for (Fault f : {Fault::None, Fault::NoFirmware, Fault::NoRegisters, Fault::SmallAperture, Fault::MapperRefuses, Fault::NoBoot, Fault::NoGbConfig, Fault::DeadRing, Fault::WrongPictures, Fault::Hang, Fault::HevcWrong, Fault::HevcHang}) {
    printf("case %d\n", static_cast<int>(f));
    run_case(f);
  }
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("AmdUvd6VideoEngineTest passed\n");
  return 0;
}
