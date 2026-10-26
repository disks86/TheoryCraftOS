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
#include <set>
#include <memory>
#include <string>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Hardware/VideoCodec/AMD/AmdUvd6VideoEngine.hpp"
#include "Hardware/VideoCodec/Software/H264.hpp"
#include "Hardware/VideoCodec/Software/H265.hpp"
#include "Hardware/VideoCodec/Software/Mpeg2.hpp"
#include "Hardware/VideoCodec/Software/Vc1.hpp"
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
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestMpeg2Stream.inc"   // and the MPEG-2 one
#include "Hardware/VideoCodec/AMD/AmdVcnSelfTestVc1Stream.inc"   // and the two VC-1 ones

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

// The MPEG-2 stream's pictures, by presentation time (packet k has k * 1000).
static bool make_mpeg2_expected(allocators::IAllocator& a, Expected* out) {
  const ConstByteView stream(kMpeg2SelfTestStream);
  out->luma.assign(kMpeg2SelfTestPictures, {});
  out->chroma.assign(kMpeg2SelfTestPictures, {});
  Mpeg2Decoder cpu(a);
  VideoFrame f;
  uint32_t got = 0;
  auto take = [&] {
    while (cpu.receive(&f)) {
      const uint32_t k = static_cast<uint32_t>(f.pts_us / 1000);
      if (k >= kMpeg2SelfTestPictures) return false;
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
  for (uint32_t i = 0; i < kMpeg2SelfTestPictures; ++i) {
    const uint64_t start = i == 0 ? 0 : kMpeg2SelfTestEnds[i - 1];
    if (!cpu.decode(stream.subview(start, kMpeg2SelfTestEnds[i] - start), int64_t{i} * 1000) || !take()) return false;
  }
  cpu.flush();
  return take() && got == kMpeg2SelfTestPictures;
}

// A VC-1 test stream's pictures, by presentation time; `config` is the Main profile sequence header (null for the Advanced profile stream).
static bool make_vc1_expected(allocators::IAllocator& a, const uint8_t* config, ConstByteView stream, const uint64_t* ends, Expected* out) {
  out->luma.assign(kVc1SelfTestPictures, {});
  out->chroma.assign(kVc1SelfTestPictures, {});
  Vc1Decoder cpu(a);
  if (config && !cpu.configure(ConstByteView(config, 4), kVc1SelfTestSize, kVc1SelfTestSize)) return false;
  VideoFrame f;
  uint32_t got = 0;
  auto take = [&] {
    while (cpu.receive(&f)) {
      const uint32_t k = static_cast<uint32_t>(f.pts_us / 1000);
      if (k >= kVc1SelfTestPictures) return false;
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
  for (uint32_t i = 0; i < kVc1SelfTestPictures; ++i) {
    const uint64_t start = i == 0 ? 0 : ends[i - 1];
    if (!cpu.decode(stream.subview(start, ends[i] - start), int64_t{i} * 1000) || !take()) return false;
  }
  cpu.flush();
  return take() && got == kVc1SelfTestPictures;
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
  const Expected* mpeg2 = nullptr;
  const Expected* vc1_main = nullptr;
  const Expected* vc1_advanced = nullptr;
  std::set<uint32_t> job_commands;   // the commands named since the last engine start
  std::map<uint32_t, uint64_t> buffer;
  uint64_t block_gpu = 0;
  const uint8_t* block_host = nullptr;
  uint32_t expected_first_word = 0;
  bool quiet = false;
  bool smc_running = true;
  uint32_t smc_result = 1;
  std::vector<std::pair<uint32_t, size_t>> smc_log;   // (message, index of the write in ring.writes)
  bool boots = true, write_pictures = true, write_hevc = true, hang_hevc = false, write_mpeg2 = true, hang_mpeg2 = false, write_vc1 = true, hang_vc1 = false;
  int boot_attempts = 0, creates = 0, hevc_creates = 0, mpeg2_creates = 0, vc1_creates = 0, destroys = 0, decodes = 0, hevc_decodes = 0, mpeg2_decodes = 0, vc1_decodes = 0, problems = 0;

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
    ring.write(0x95, 1);   // SMC_RESP_0: idle, the last message was done
  }
  uint32_t read(uint32_t r) override { return ring.read(r); }
  void write(uint32_t r, uint32_t v) override {
    const uint32_t old = ring.read(r);
    ring.write(r, v);
    if (r == 0x94) {   // SMC_MESSAGE_0
      smc_log.push_back({v, ring.writes.size() - 1});
      ring.write(0x95, smc_running ? smc_result : 0);
    }
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
    job_commands.insert(command);
    if (command != 0) return;
    const uint8_t* m = ring.translate(address, kMsgBytes);
    if (!m) { problem("message outside the card's memory"); return; }
    if (word(m, 4) == 0) {
      if (word(m, 16) == 0x10) ++hevc_creates;
      else if (word(m, 16) == 3) ++mpeg2_creates;
      else if (word(m, 16) == 1) ++vc1_creates;
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
  // The MPEG-2 picture: no context buffer and no scaling table in the job, references among the last six jobs, the slices behind a start code; the CPU's picture for the
  // k-th job goes into the target.
  void on_mpeg2(const uint8_t* m, const std::set<uint32_t>& commands, uint32_t height, uint32_t pitch, uint32_t chroma_offset) {
    ++mpeg2_decodes;
    if (hang_mpeg2) ring.stall_fence = true;
    constexpr uint32_t kCodec = dec(52);
    const uint32_t current = word(m, kCodec + 0), forward = word(m, kCodec + 4), backward = word(m, kCodec + 8);
    if (commands.count(0x206) || commands.count(0x204)) problem("MPEG-2 job names a context buffer or a scaling table");
    for (uint32_t c : {0u, 1u, 2u, 3u, 5u, 0x100u})
      if (!commands.count(c)) problem("MPEG-2 job lacks a buffer");
    if (current == 0 || forward >= current || backward >= current || current - forward > 6 || current - backward > 6) problem("MPEG-2 references outside the last six jobs");
    if (word(m, dec(5)) == 0 || word(m, dec(7)) != 0 || word(m, dec(2)) != 64 || height != 64) problem("MPEG-2 message with the wrong buffer sizes");
    const uint8_t* bits = ring.translate(buffer[0x100], 4);
    if (!bits || bits[0] != 0 || bits[1] != 0 || bits[2] != 1) problem("MPEG-2 bitstream does not start with a start code");
    if (!mpeg2 || !write_mpeg2) return;
    const uint32_t k = (current - 1) % static_cast<uint32_t>(mpeg2->luma.size());
    const uint64_t target_bytes = uint64_t{chroma_offset} + uint64_t{pitch} * (height / 2);
    uint8_t* target = ring.translate(buffer[2], target_bytes);
    if (!target || pitch < 64 || uint64_t{pitch} * 64 > chroma_offset) { problem("target does not hold a 64 x 64 picture"); return; }
    for (uint32_t y = 0; y < 64; ++y) memcpy(target + uint64_t{pitch} * y, mpeg2->luma[k].data() + y * 64, 64);
    for (uint32_t y = 0; y < 32; ++y) memcpy(target + chroma_offset + uint64_t{pitch} * y, mpeg2->chroma[k].data() + y * 64, 64);
  }
  // The VC-1 picture: no context buffer and no scaling table in the job, the profile and the size as Mesa gives them (macroblocks for Simple and Main, samples for Advanced), the
  // frame data behind it - which tells which picture of the stream this is, and the CPU's picture for it goes into the target.
  void on_vc1(const uint8_t* m, const std::set<uint32_t>& commands, uint32_t /*height in the message: macroblocks for Simple and Main*/, uint32_t pitch, uint32_t chroma_offset) {
    constexpr uint32_t height = 64;
    ++vc1_decodes;
    if (hang_vc1) ring.stall_fence = true;
    constexpr uint32_t kCodec = dec(52);
    const uint32_t profile = word(m, kCodec + 0);
    if (commands.count(0x206) || commands.count(0x204)) problem("VC-1 job names a context buffer or a scaling table");
    for (uint32_t c : {0u, 1u, 2u, 3u, 5u, 0x100u})
      if (!commands.count(c)) problem("VC-1 job lacks a buffer");
    const bool advanced = profile == 2;
    if (profile != 1 && profile != 2) { problem("VC-1 profile is neither Main nor Advanced"); return; }
    if (word(m, dec(2)) != (advanced ? 64u : 4u) || word(m, dec(3)) != (advanced ? 64u : 4u) || word(m, dec(5)) == 0 || word(m, dec(7)) != 0) problem("VC-1 message with the wrong size or buffers");
    const Expected* expected = advanced ? vc1_advanced : vc1_main;
    if (!expected || !write_vc1) return;
    // Which picture: the frame data is the packet of the stream (the Advanced profile's behind a start code of its own when the packet has none).
    const uint8_t* bits = ring.translate(buffer[0x100], 64);
    if (!bits) { problem("VC-1 bitstream outside the card's memory"); return; }
    const ConstByteView stream = advanced ? ConstByteView(kVc1SelfTestAdvancedStream) : ConstByteView(kVc1SelfTestMainStream);
    const uint64_t* ends = advanced ? kVc1SelfTestAdvancedEnds : kVc1SelfTestMainEnds;
    uint32_t k = kVc1SelfTestPictures;
    for (uint32_t i = 0; i < kVc1SelfTestPictures && k == kVc1SelfTestPictures; ++i) {
      const uint64_t start = i == 0 ? 0 : ends[i - 1];
      const uint64_t length = ends[i] - start;
      // (The packet may carry the sequence and entry point headers before the frame, and the frame may come with a start code of its own in front of it.)
      for (uint32_t offset : {0u, 4u})
        if (length >= 24 && memmem(stream.data() + start, length, bits + offset, 24) != nullptr) { k = i; break; }
    }
    if (k == kVc1SelfTestPictures) { problem("VC-1 frame data is no picture of the stream"); return; }
    const uint64_t target_bytes = uint64_t{chroma_offset} + uint64_t{pitch} * (height / 2);
    uint8_t* target = ring.translate(buffer[2], target_bytes);
    if (!target || pitch < 64 || uint64_t{pitch} * 64 > chroma_offset) { problem("target does not hold a 64 x 64 picture"); return; }
    for (uint32_t y = 0; y < 64; ++y) memcpy(target + uint64_t{pitch} * y, expected->luma[k].data() + y * 64, 64);
    for (uint32_t y = 0; y < 32; ++y) memcpy(target + chroma_offset + uint64_t{pitch} * y, expected->chroma[k].data() + y * 64, 64);
  }
  void on_engine() {
    uint8_t m[kMsgBytes];
    const uint8_t* source = ring.translate(buffer[0], sizeof m);
    if (!source) { problem("decode message outside the card's memory"); return; }
    memcpy(m, source, sizeof m);
    const uint32_t height = word(m, dec(3)), pitch = word(m, dec(24)), chroma_offset = word(m, dec(30));
    const std::set<uint32_t> commands = job_commands;
    job_commands.clear();
    if (word(m, dec(0)) == 0x10 && word(m, 4) == 1 && word(m, 0) == kMsgBytes) {
      on_hevc(m, height, pitch, chroma_offset);
      return;
    }
    if (word(m, dec(0)) == 3 && word(m, 4) == 1 && word(m, 0) == kMsgBytes) {
      on_mpeg2(m, commands, height, pitch, chroma_offset);
      return;
    }
    if (word(m, dec(0)) == 1 && word(m, 4) == 1 && word(m, 0) == kMsgBytes) {
      on_vc1(m, commands, height, pitch, chroma_offset);
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

// Damaged streams of one codec through an engine session: every packet truncated at some point, and single bits flipped across the stream. Nothing may crash or hang (the
// sanitizers are watching), the engine must not wedge, and the session must still hand back as many pictures as it was given or fewer.
static void damaged_streams(AmdUvd6VideoEngine& engine, Card& card, VideoCodec codec, ConstByteView stream, const uint64_t* packet_ends, uint32_t packets, ConstByteView codec_config = ConstByteView()) {
  DecoderConfig config = h264_config();
  config.codec = codec;
  config.codec_config = codec_config;
  config.width = config.height = 64;
  card.quiet = true;
  const int problems_before = card.problems;
  for (uint32_t variant = 0; variant < 48; ++variant) {
    std::vector<uint8_t> bytes(stream.data(), stream.data() + stream.size());
    std::vector<uint64_t> ends(packet_ends, packet_ends + packets);
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
    for (uint32_t i = 0; i < packets; ++i) {
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
    CHECK(frames <= packets);
  }
  card.quiet = false;
  (void)problems_before;
}

// Feeds the MPEG-2 self-test stream to a session; true when every picture came out as the CPU's luma (by presentation time).
static bool run_mpeg2_stream(IVideoDecoder& session, const Expected& e) {
  const ConstByteView stream(kMpeg2SelfTestStream);
  uint32_t count = 0;
  bool same = true;
  auto drain_all = [&] {
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
  for (uint32_t i = 0; i < kMpeg2SelfTestPictures; ++i) {
    VideoPacket p;
    const uint64_t start = i == 0 ? 0 : kMpeg2SelfTestEnds[i - 1];
    p.data = stream.subview(start, kMpeg2SelfTestEnds[i] - start);
    p.pts_us = int64_t{i} * 1000;
    p.keyframe = i == 0;
    if (session.submit(p) != SubmitResult::Accepted) return false;
    drain_all();
  }
  session.flush();
  drain_all();
  return same && count == kMpeg2SelfTestPictures;
}

// Feeds one of the VC-1 test streams to a session; true when every picture came out as the CPU's luma (by presentation time).
static bool run_vc1_stream(IVideoDecoder& session, ConstByteView stream, const uint64_t* ends, const Expected& e) {
  uint32_t count = 0;
  bool same = true;
  auto drain_all = [&] {
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
  for (uint32_t i = 0; i < kVc1SelfTestPictures; ++i) {
    VideoPacket p;
    const uint64_t start = i == 0 ? 0 : ends[i - 1];
    p.data = stream.subview(start, ends[i] - start);
    p.pts_us = int64_t{i} * 1000;
    p.keyframe = i == 0;
    if (session.submit(p) != SubmitResult::Accepted) return false;
    drain_all();
  }
  session.flush();
  drain_all();
  return same && count == kVc1SelfTestPictures;
}

enum class Fault { None, NoFirmware, NoRegisters, SmallAperture, MapperRefuses, NoBoot, NoGbConfig, DeadRing, WrongPictures, Hang, HevcWrong, HevcHang, SmcNotRunning, SmcRefuses, Mpeg2Wrong, Mpeg2Hang, Vc1Wrong, Vc1Hang };

static void run_case(Fault fault) {
  TestAllocator a;
  {
    Expected expected;
    CHECK(make_expected(a, &expected));
    HevcExpected hevc;
    CHECK(make_hevc_expected(a, &hevc));
    Expected mpeg2;
    CHECK(make_mpeg2_expected(a, &mpeg2));
    Expected vc1_main, vc1_advanced;
    CHECK(make_vc1_expected(a, kVc1SelfTestMainConfig, ConstByteView(kVc1SelfTestMainStream), kVc1SelfTestMainEnds, &vc1_main));
    CHECK(make_vc1_expected(a, nullptr, ConstByteView(kVc1SelfTestAdvancedStream), kVc1SelfTestAdvancedEnds, &vc1_advanced));
    Card card;
    card.expected = &expected;
    card.hevc = &hevc;
    card.write_hevc = fault != Fault::HevcWrong;
    card.hang_hevc = fault == Fault::HevcHang;
    card.mpeg2 = &mpeg2;
    card.write_mpeg2 = fault != Fault::Mpeg2Wrong;
    card.hang_mpeg2 = fault == Fault::Mpeg2Hang;
    card.vc1_main = &vc1_main;
    card.vc1_advanced = &vc1_advanced;
    card.write_vc1 = fault != Fault::Vc1Wrong;
    card.hang_vc1 = fault == Fault::Vc1Hang;
    if (fault == Fault::SmcNotRunning) card.ring.write(0x95, 0), card.smc_running = false;
    if (fault == Fault::SmcRefuses) card.smc_result = 0xFF;
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
    const bool engine_up = fault == Fault::None || fault == Fault::HevcWrong || fault == Fault::SmcNotRunning || fault == Fault::SmcRefuses || fault == Fault::Mpeg2Wrong || fault == Fault::Vc1Wrong;
    CHECK(started == engine_up);
    CHECK(card.problems == 0 || fault == Fault::WrongPictures);
    if (engine_up) {
      const bool hevc_on = fault != Fault::HevcWrong, mpeg2_on = fault != Fault::Mpeg2Wrong, vc1_on = fault != Fault::Vc1Wrong;
      CHECK(engine.engine_running() && engine.decode_ready() && engine.hevc_ready() == hevc_on);
      CHECK(engine.mpeg2_ready() == mpeg2_on && engine.vc1_ready() == vc1_on);
      CHECK(engine.info().accelerated_decode_codecs == (codec_mask(VideoCodec::H264) | (hevc_on ? codec_mask(VideoCodec::Hevc) : 0u) | (mpeg2_on ? codec_mask(VideoCodec::Mpeg2) : 0u) | (vc1_on ? codec_mask(VideoCodec::Vc1) : 0u)));
      CHECK(strstr(engine.engine_note(), !hevc_on ? "H.265 self-test failed" : !mpeg2_on ? "MPEG-2 self-test failed" : !vc1_on ? "VC-1 self-test failed" : "H.264, H.265, MPEG-2 and VC-1 decode run on it") != nullptr && strstr(engine.engine_summary(), "UVD 6.3") != nullptr);
      // The tests opened and closed their own sessions: H.264 (five pictures) and H.265 (six; its session is made even when the test then fails).
      CHECK(card.creates == 1 && card.hevc_creates == 1 && card.mpeg2_creates == 1 && card.vc1_creates == (vc1_on ? 2 : 1) && card.destroys == (vc1_on ? 5 : 4) && card.decodes == int(AmdVcnSelfTest::kPictures));
      CHECK(mpeg2_on ? card.mpeg2_decodes == int(kMpeg2SelfTestPictures) : card.mpeg2_decodes > 0);
      CHECK(vc1_on ? card.vc1_decodes == int(2 * kVc1SelfTestPictures) : card.vc1_decodes > 0);
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
      // MPEG-2 sessions are the engine's when its test passed, the CPU's when it did not.
      {
        DecoderConfig mpeg2_config = h264_config();
        mpeg2_config.codec = VideoCodec::Mpeg2;
        const int before = card.mpeg2_decodes;
        UniquePointer<IVideoDecoder> first = engine.create_decoder(mpeg2_config);
        CHECK(static_cast<bool>(first));
        UniquePointer<IVideoDecoder> second = engine.create_decoder(mpeg2_config);   // (the engine is busy: the CPU's)
        CHECK(static_cast<bool>(second) && run_mpeg2_stream(*second, mpeg2) && card.mpeg2_decodes == before);
        CHECK(run_mpeg2_stream(*first, mpeg2) && card.mpeg2_decodes == before + (mpeg2_on ? int(kMpeg2SelfTestPictures) : 0));
      }
      // VC-1 sessions (the Main profile stream takes its sequence header as the configuration): the engine's when its test passed, the CPU's when it did not.
      {
        DecoderConfig vc1_config = h264_config();
        vc1_config.codec = VideoCodec::Vc1;
        vc1_config.width = vc1_config.height = kVc1SelfTestSize;
        vc1_config.codec_config = ConstByteView(kVc1SelfTestMainConfig, 4);
        const int before = card.vc1_decodes;
        UniquePointer<IVideoDecoder> first = engine.create_decoder(vc1_config);
        CHECK(static_cast<bool>(first));
        UniquePointer<IVideoDecoder> second = engine.create_decoder(vc1_config);   // (the engine is busy: the CPU's)
        CHECK(static_cast<bool>(second) && run_vc1_stream(*second, ConstByteView(kVc1SelfTestMainStream), kVc1SelfTestMainEnds, vc1_main) && card.vc1_decodes == before);
        CHECK(run_vc1_stream(*first, ConstByteView(kVc1SelfTestMainStream), kVc1SelfTestMainEnds, vc1_main) && card.vc1_decodes == before + (vc1_on ? int(kVc1SelfTestPictures) : 0));
      }
      if (vc1_on) {
        damaged_streams(engine, card, VideoCodec::Vc1, ConstByteView(kVc1SelfTestAdvancedStream), kVc1SelfTestAdvancedEnds, kVc1SelfTestPictures);
        damaged_streams(engine, card, VideoCodec::Vc1, ConstByteView(kVc1SelfTestMainStream), kVc1SelfTestMainEnds, kVc1SelfTestPictures, ConstByteView(kVc1SelfTestMainConfig, 4));
        DecoderConfig vc1_config = h264_config();
        vc1_config.codec = VideoCodec::Vc1;
        const int before = card.vc1_decodes;
        UniquePointer<IVideoDecoder> clean = engine.create_decoder(vc1_config);
        CHECK(static_cast<bool>(clean) && run_vc1_stream(*clean, ConstByteView(kVc1SelfTestAdvancedStream), kVc1SelfTestAdvancedEnds, vc1_advanced) && card.vc1_decodes == before + int(kVc1SelfTestPictures));
      }
      if (mpeg2_on) {
        damaged_streams(engine, card, VideoCodec::Mpeg2, ConstByteView(kMpeg2SelfTestStream), kMpeg2SelfTestEnds, kMpeg2SelfTestPictures);
        DecoderConfig mpeg2_config = h264_config();
        mpeg2_config.codec = VideoCodec::Mpeg2;
        const int before = card.mpeg2_decodes;
        UniquePointer<IVideoDecoder> clean = engine.create_decoder(mpeg2_config);
        CHECK(static_cast<bool>(clean) && run_mpeg2_stream(*clean, mpeg2) && card.mpeg2_decodes == before + int(kMpeg2SelfTestPictures));
      }
      if (hevc_on) {
        damaged_streams(engine, card, VideoCodec::Hevc, ConstByteView(kHevcSelfTestStream), kHevcSelfTestEnds, kHevcSelfTestPictures);
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
      // The SMC was asked to power the engine before anything else was written to it, and to power it down at shutdown - but only when it said yes.
      size_t first_engine_write = 0;
      while (first_engine_write < card.ring.writes.size() && card.ring.writes[first_engine_write].first != reg::kSoftReset) ++first_engine_write;
      if (fault == Fault::SmcNotRunning || fault == Fault::SmcRefuses) {
        CHECK(card.smc_log.size() == (fault == Fault::SmcRefuses ? 1u : 0u));
        CHECK(strstr(engine.firmware_status(), fault == Fault::SmcRefuses ? "refused to power" : "did not answer") != nullptr);
      } else {
        CHECK(!card.smc_log.empty() && card.smc_log[0].first == 0x61 && card.smc_log[0].second < first_engine_write);
        CHECK(strstr(engine.firmware_status(), "SMC powered the engine") != nullptr);
      }
      const size_t smc_before_shutdown = card.smc_log.size();
      engine.shutdown();
      if (fault == Fault::SmcNotRunning || fault == Fault::SmcRefuses) CHECK(card.smc_log.size() == smc_before_shutdown);
      else CHECK(card.smc_log.size() == smc_before_shutdown + 1 && card.smc_log.back().first == 0x60);
      CHECK(!engine.engine_running() && !engine.decode_ready() && engine.info().accelerated_decode_codecs == 0);
      CHECK(card.ring.writes.size() > writes_before);
    } else {
      // Not trusted, or never started: nothing running, nothing accelerated, and H.264 is still served (by the CPU).
      CHECK(!engine.engine_running() && !engine.decode_ready() && engine.info().accelerated_decode_codecs == 0);
      const char* expected_note = fault == Fault::NoBoot ? "did not answer" : fault == Fault::NoGbConfig ? "address configuration" : fault == Fault::DeadRing ? "did not run a register write" :
                                  fault == Fault::Hang || fault == Fault::HevcHang || fault == Fault::Mpeg2Hang || fault == Fault::Vc1Hang ? "engine stopped" : "self-test failed";
      CHECK(strstr(engine.engine_note(), expected_note) != nullptr);
      if (fault != Fault::NoBoot && fault != Fault::NoGbConfig) CHECK(strstr(engine.firmware_status(), "engine stopped") != nullptr || strstr(engine.firmware_status(), "stopped") != nullptr);
      const int before = card.decodes;
      UniquePointer<IVideoDecoder> software = engine.create_decoder(h264_config());
      CHECK(static_cast<bool>(software) && run_stream(a, *software, expected) && card.decodes == before);
      // Whatever stopped the engine, the power the SMC gave it is given back.
      if (fault == Fault::NoBoot || fault == Fault::DeadRing || fault == Fault::WrongPictures || fault == Fault::Hang || fault == Fault::HevcHang || fault == Fault::Mpeg2Hang || fault == Fault::Vc1Hang)
        CHECK(card.smc_log.size() == 2 && card.smc_log[0].first == 0x61 && card.smc_log[1].first == 0x60);
      if (fault == Fault::NoGbConfig) CHECK(card.smc_log.empty());   // (refused before anything is asked of the card)
      if (fault == Fault::NoBoot) CHECK(card.boot_attempts == 1 + 3);   // the release that starts the VCPU, then three tries (not the driver's ten)
    }
  }
  CHECK(a.live == 0);
}

int main() {
  for (Fault f : {Fault::None, Fault::NoFirmware, Fault::NoRegisters, Fault::SmallAperture, Fault::MapperRefuses, Fault::NoBoot, Fault::NoGbConfig, Fault::DeadRing, Fault::WrongPictures, Fault::Hang, Fault::HevcWrong, Fault::HevcHang, Fault::SmcNotRunning, Fault::SmcRefuses, Fault::Mpeg2Wrong, Fault::Mpeg2Hang, Fault::Vc1Wrong, Fault::Vc1Hang}) {
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
