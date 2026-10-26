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
  std::map<uint32_t, uint64_t> buffer;
  uint64_t block_gpu = 0;
  const uint8_t* block_host = nullptr;
  uint32_t expected_first_word = 0;
  bool boots = true, write_pictures = true;
  int boot_attempts = 0, creates = 0, destroys = 0, decodes = 0, problems = 0;

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
    printf("  fake engine: %s\n", what);
  }
  void on_command(uint32_t command, uint64_t address) {
    buffer[command] = address;
    if (command != 0) return;
    const uint8_t* m = ring.translate(address, kMsgBytes);
    if (!m) { problem("message outside the card's memory"); return; }
    if (word(m, 4) == 0) ++creates;
    else if (word(m, 4) == 2) ++destroys;
  }
  void on_engine() {
    uint8_t m[kMsgBytes];
    const uint8_t* source = ring.translate(buffer[0], sizeof m);
    if (!source) { problem("decode message outside the card's memory"); return; }
    memcpy(m, source, sizeof m);
    const uint32_t k = static_cast<uint32_t>(decodes++);
    const uint32_t height = word(m, dec(3)), pitch = word(m, dec(24)), chroma_offset = word(m, dec(30));
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

enum class Fault { None, NoFirmware, NoRegisters, SmallAperture, MapperRefuses, NoBoot, NoGbConfig, DeadRing, WrongPictures, Hang };

static void run_case(Fault fault) {
  TestAllocator a;
  {
    Expected expected;
    CHECK(make_expected(a, &expected));
    Card card;
    card.expected = &expected;
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
    CHECK(started == (fault == Fault::None));
    CHECK(card.problems == 0 || fault == Fault::WrongPictures);
    if (fault == Fault::None) {
      CHECK(engine.engine_running() && engine.decode_ready() && engine.info().accelerated_decode_codecs == codec_mask(VideoCodec::H264));
      CHECK(strstr(engine.engine_note(), "self-test passed") != nullptr && strstr(engine.engine_summary(), "UVD 6.3") != nullptr);
      CHECK(card.creates == 1 && card.destroys == 1 && card.decodes == int(AmdVcnSelfTest::kPictures));   // the test opened and closed its own session
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
                                  fault == Fault::Hang ? "engine stopped" : "self-test failed";
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
  for (Fault f : {Fault::None, Fault::NoFirmware, Fault::NoRegisters, Fault::SmallAperture, Fault::MapperRefuses, Fault::NoBoot, Fault::NoGbConfig, Fault::DeadRing, Fault::WrongPictures, Fault::Hang}) {
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
