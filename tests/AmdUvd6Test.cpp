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

// Host-side tests for the UVD 6.3 start and stop sequences (AmdUvd6) against a simulated engine: the register file of the card with the behaviour the driver
// relies on - the VCPU comes out of reset and reports itself in UVD_STATUS only when its clock is on, the memory path is not stalled, the cache window points at a
// block that holds the microcode, and the reset was released - and a record of every write in order. The register numbers here are typed in from AMD's
// uvd_6_0_d.h / oss_3_0_d.h, independently of the generated ones the code uses.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdUvd6.hpp"
#include "FakeAmdVcn.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace reg {
constexpr uint32_t kStatus = 0x3daf, kSoftReset = 0x3da0, kVcpuCntl = 0x3d98, kLmiCtrl2 = 0x3d3d, kLmiCtrl = 0x3d66, kMastIntEn = 0x3d40;
constexpr uint32_t kRbCntl = 0x3da9, kRbRptr = 0x3da4, kRbWptr = 0x3da5, kRbRptrAddr = 0x3daa, kRbBarLow = 0x3c69, kRbBarHigh = 0x3c68;
constexpr uint32_t kCacheBarLow = 0x3c5f, kCacheBarHigh = 0x3c5e;
constexpr uint32_t kOffset0 = 0x3d82, kSize0 = 0x3d83, kOffset1 = 0x3d84, kSize1 = 0x3d85, kOffset2 = 0x3d86, kSize2 = 0x3d87;
constexpr uint32_t kAddrConfig = 0x3bd3, kDbAddrConfig = 0x3bd4, kDbwAddrConfig = 0x3bd5, kScratch4 = 0x3d38;
constexpr uint32_t kCgcGate = 0x3d2a, kSuvdCgcGate = 0x3be4, kCgcCtrl = 0x3d2c, kCtxIndex = 0x3d28, kCtxData = 0x3d29, kPowerStatus = 0x38c4;
constexpr uint32_t kSrbmSoftReset = 0x398, kSwapLmi = 0x3d6d, kSwapMp = 0x3d6f, kMpcMux = 0x3d7d;
}  // namespace reg

struct FakeUvd final : IVcnRegisters {
  std::map<uint32_t, uint32_t> regs;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  // The memory block the engine will look at: where the test put it, and what its first word must be for the firmware to be accepted.
  uint64_t block_gpu = 0;
  const uint8_t* block_host = nullptr;
  uint32_t expected_first_word = 0;
  uint32_t fail_first = 0;       // boot attempts that do not succeed before one does (UINT32_MAX: never)
  uint32_t attempts = 0;
  uint32_t vcpu_resets = 0;      // times the VCPU reset bit was set again after the first release

  uint32_t read(uint32_t r) override { auto it = regs.find(r); return it == regs.end() ? 0 : it->second; }
  void write(uint32_t r, uint32_t v) override {
    writes.push_back({r, v});
    const uint32_t old = read(r);
    regs[r] = v;
    if (r == reg::kSoftReset && (old & 8) && !(v & 8)) boot();
    if (r == reg::kSoftReset && !(old & 8) && (v & 8) && attempts > 0) ++vcpu_resets;
  }
  void boot() {
    ++attempts;
    const bool clock_on = read(reg::kVcpuCntl) & 0x200;
    const bool not_stalled = !(read(reg::kLmiCtrl2) & 0x100);
    const uint64_t bar = (uint64_t{read(reg::kCacheBarHigh)} << 32) | read(reg::kCacheBarLow);
    uint32_t first = 0;
    if (block_host && bar == block_gpu) memcpy(&first, block_host, 4);
    const bool window = read(reg::kOffset0) == (256u >> 3) && read(reg::kSize0) != 0 && read(reg::kSize1) != 0 && read(reg::kSize2) != 0;
    if (clock_on && not_stalled && window && first == expected_first_word && fail_first != UINT32_MAX && attempts > fail_first) regs[reg::kStatus] |= 2;
  }
};

// A firmware image: the 32-byte common header (ip version 6.3), padding up to the microcode at offset 256, and `ucode_bytes` of microcode.
static std::vector<uint8_t> make_firmware(uint32_t ucode_bytes, uint32_t version, uint16_t ip_major = 6) {
  const uint32_t offset = 256;
  std::vector<uint8_t> image(offset + ucode_bytes);
  auto put32 = [&](uint32_t at, uint32_t v) { for (int i = 0; i < 4; ++i) image[at + i] = static_cast<uint8_t>(v >> (8 * i)); };
  auto put16 = [&](uint32_t at, uint16_t v) { image[at] = static_cast<uint8_t>(v); image[at + 1] = static_cast<uint8_t>(v >> 8); };
  put32(0, static_cast<uint32_t>(image.size()));
  put32(4, 32);
  put16(8, 1); put16(10, 0);
  put16(12, ip_major); put16(14, 3);
  put32(16, version);
  put32(20, ucode_bytes);
  put32(24, offset);
  for (uint32_t i = 0; i < ucode_bytes; ++i) image[offset + i] = static_cast<uint8_t>(0x11 + i * 7);
  return image;
}

struct TestAllocator final : allocators::IAllocator {
  std::vector<void*> blocks;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment); blocks.push_back(p); return p; }
  void deallocate(void*) override {}
  ~TestAllocator() { for (void* p : blocks) free(p); }
};

struct Rig {
  TestAllocator alloc;
  AmdVcnFirmware firmware{alloc};
  std::vector<uint8_t> image;
  FakeUvd uvd;
  fakevcn::FakeClock clock;
  uint8_t* block = nullptr;
  uint8_t* ring = nullptr;
  uint64_t block_size = 0;
  AmdUvd6::Memory memory;
  AmdUvd6::Config config;
  explicit Rig(uint32_t ucode_bytes = 40000, uint32_t version = 0x01008210) {
    image = make_firmware(ucode_bytes, version);
    CHECK(firmware.adopt(ConstByteView(image.data(), image.size())) == AmdFirmwareStatus::Ok);
    config.gb_addr_config = 0x22011003;
    config.max_handles = AmdUvd6::max_handles_for(firmware);
    config.boot_attempts = 3;
    block_size = AmdUvd6::block_bytes(firmware, config.max_handles);
    block = static_cast<uint8_t*>(aligned_alloc(4096, block_size));
    ring = static_cast<uint8_t*>(aligned_alloc(4096, 4096));
    memset(block, 0xCC, block_size);
    memset(ring, 0xCC, 4096);
    memory.block_gpu_address = 0x1'0000'0000ull;
    memory.block = MmioRegion(reinterpret_cast<uint64_t>(block), block_size);
    memory.ring_gpu_address = 0x1'0100'0000ull;
    memory.ring = MmioRegion(reinterpret_cast<uint64_t>(ring), 4096);
    uvd.block_gpu = memory.block_gpu_address;
    uvd.block_host = block;
    memcpy(&uvd.expected_first_word, image.data() + 256, 4);
  }
  ~Rig() { free(block); free(ring); }
};

static uint32_t at(const FakeUvd& u, uint32_t r) { auto it = u.regs.find(r); return it == u.regs.end() ? 0 : it->second; }

static void test_layout() {
  Rig rig;
  const uint64_t ucode = 40000;
  const uint64_t window = (ucode + 8 + 4095) / 4096 * 4096;   // 40960
  CHECK(AmdUvd6::firmware_window_bytes(rig.firmware) == window);
  CHECK(AmdUvd6::max_handles_for(rig.firmware) == 40);
  CHECK(AmdUvd6::block_bytes(rig.firmware, 40) == window + 200 * 1024 + 256 * 1024 + 40 * 50 * 1024);
  CHECK(AmdUvd6::block_bytes(rig.firmware, 10) == window + 200 * 1024 + 256 * 1024 + 10 * 50 * 1024);
  // The session count follows the firmware version: 1.80 (minor 0x50) and later have 40.
  for (const auto& c : {std::pair<uint32_t, uint32_t>{0x01005010, 40}, {0x01004F10, 10}, {0x02000010, 40}, {0x00FF5010, 10}, {0x01008210, 40}}) {
    std::vector<uint8_t> image = make_firmware(1000, c.first);
    AmdVcnFirmware f(rig.alloc);
    CHECK(f.adopt(ConstByteView(image.data(), image.size())) == AmdFirmwareStatus::Ok && AmdUvd6::max_handles_for(f) == c.second);
  }
  CHECK(AmdUvd6::kFirmwareOffset == 256 && AmdUvd6::kRingBytes == 4096);
}

static void test_start() {
  Rig rig;
  FakeUvd& u = rig.uvd;
  AmdUvd6 engine(u, rig.clock);
  CHECK(!engine.started());
  CHECK(engine.start(rig.firmware, rig.memory, rig.config) == AmdUvd6::Status::Ok && engine.started());
  const uint64_t window = 40960;
  // The microcode is at the start of the block, a pad of zeros to the end of it, and the ring is zeroed.
  CHECK(memcmp(rig.block, rig.image.data() + 256, 40000) == 0);
  bool zeros = true;
  for (uint64_t i = 40000; i < rig.block_size; ++i) zeros = zeros && rig.block[i] == 0;
  CHECK(zeros);
  bool ring_zero = true;
  for (int i = 0; i < 4096; ++i) ring_zero = ring_zero && rig.ring[i] == 0;
  CHECK(ring_zero);
  // The memory controller: the block's address, and the three cache windows (microcode from byte 256, heap, stack and sessions), in units of eight bytes.
  CHECK(at(u, reg::kCacheBarLow) == 0 && at(u, reg::kCacheBarHigh) == 1);
  CHECK(at(u, reg::kOffset0) == 32 && at(u, reg::kSize0) == window - 256);
  CHECK(at(u, reg::kOffset1) == window / 8 && at(u, reg::kSize1) == 256 * 1024);
  CHECK(at(u, reg::kOffset2) == (window + 256 * 1024) / 8 && at(u, reg::kSize2) == 200 * 1024 + 40 * 50 * 1024);
  CHECK(at(u, reg::kAddrConfig) == 0x22011003 && at(u, reg::kDbAddrConfig) == 0x22011003 && at(u, reg::kDbwAddrConfig) == 0x22011003 && at(u, reg::kScratch4) == 40);
  // The memory interface: write clean timer 0x40, timer enable, data coherency (both), request mode, disable on firmware verification failure.
  CHECK(at(u, reg::kLmiCtrl) == (0x40u | 0x100 | 0x200 | 0x2000 | 0x100000 | 0x200000));
  CHECK(at(u, reg::kSwapLmi) == 0 && at(u, reg::kSwapMp) == 0 && at(u, reg::kMpcMux) == 0x88);
  // The VCPU runs: clock on, memory path not stalled, reset released, interrupt enables set, status reports.
  CHECK((at(u, reg::kVcpuCntl) & 0x200) && !(at(u, reg::kLmiCtrl2) & 0x100) && at(u, reg::kSoftReset) == 0 && (at(u, reg::kMastIntEn) & 0x6) == 0x6);   // VCPU_EN (bit 1) and SYS_EN (bit 2)
  CHECK(u.attempts == 1 && (at(u, reg::kStatus) & 2));
  // The decode ring: 4 KiB (log2 = 12), block size 1, no update, pointer write enable; at the ring's address; pointers at zero; fetching on.
  const uint32_t cntl = at(u, reg::kRbCntl);
  CHECK((cntl & 0x1f) == 12 && ((cntl >> 8) & 0x1f) == 1 && !(cntl & 0x10000) && (cntl & 0x1000000) && (cntl & 0x10000000) && !(cntl & (1u << 20)));
  CHECK(at(u, reg::kRbBarLow) == 0x01000000 && at(u, reg::kRbBarHigh) == 1 && at(u, reg::kRbRptrAddr) == (1u >> 2));
  CHECK(at(u, reg::kRbRptr) == 0 && at(u, reg::kRbWptr) == 0);
  // Clocks ungated: no gate bits (except the sub-block ones the driver always sets), software gating on, memory clock gating off.
  CHECK(at(u, reg::kCgcGate) == 0 && (at(u, reg::kSuvdCgcGate) & 0x1FFF) != 0);
  CHECK(at(u, reg::kCtxIndex) == 0xC0 && (at(u, reg::kCtxData) & 0xFFF) == 0);
  CHECK((at(u, reg::kCgcCtrl) & 1) == 0);   // (DYN_CLOCK_MODE is the lowest bit: off with medium-grain gating not supported)
}

static void test_order() {
  Rig rig;
  FakeUvd& u = rig.uvd;
  AmdUvd6 engine(u, rig.clock);
  CHECK(engine.start(rig.firmware, rig.memory, rig.config) == AmdUvd6::Status::Ok);
  // The reset sequence as the driver makes it: everything held in reset, then everything but the VCPU, then nothing.
  std::vector<uint32_t> resets;
  for (const auto& w : u.writes) if (w.first == reg::kSoftReset) resets.push_back(w.second);
  CHECK(resets.size() == 3 && resets[0] == (0x4 | 0x8 | 0x2 | 0x1 | 0x20 | 0x40 | 0x80 | (1u << 13)) && resets[1] == 0x8 && resets[2] == 0);
  // The block's address goes in before the reset, the ring's after the VCPU answered; the memory path is stalled before the reset and freed before the boot.
  auto index_of = [&](uint32_t r, uint32_t value, bool any = true) { for (size_t i = 0; i < u.writes.size(); ++i) if (u.writes[i].first == r && (any || u.writes[i].second == value)) return static_cast<int>(i); return -1; };
  const int cache = index_of(reg::kCacheBarLow, 0), first_reset = index_of(reg::kSoftReset, 0), boot = index_of(reg::kSoftReset, 0, false), clock = index_of(reg::kVcpuCntl, 0x200, false), ring_base = index_of(reg::kRbBarLow, 0);
  CHECK(cache >= 0 && cache < first_reset && first_reset < clock && clock < boot && boot < ring_base);
  // Power management is not touched: no write to the power status register's gating bits beyond clearing the DPG mode.
  CHECK(index_of(reg::kPowerStatus, 0) >= 0 && u.regs[reg::kPowerStatus] == 0);
  // The microcode written through the memory BAR is flushed out of the host data path (a write of 1, a read back) after it is written and before the first engine register.
  const int flush = index_of(0x1520, 1, false);
  CHECK(flush >= 0 && flush < cache && u.regs[0x1520] == 1);
  // SRBM reset of the UVD block is released (bit 18 clear).
  CHECK((at(u, reg::kSrbmSoftReset) & 0x40000) == 0 && index_of(reg::kSrbmSoftReset, 0) >= 0);
}

static void test_boot_retries_and_failure() {
  {  // The VCPU answers on the third try, after two resets.
    Rig rig;
    rig.uvd.fail_first = 2;
    AmdUvd6 engine(rig.uvd, rig.clock);
    CHECK(engine.start(rig.firmware, rig.memory, rig.config) == AmdUvd6::Status::Ok && rig.uvd.attempts == 3 && rig.uvd.vcpu_resets == 2);
  }
  {  // It never answers: TimedOut after the allowed tries, and the engine is stopped the way the driver stops it.
    Rig rig;
    rig.uvd.fail_first = UINT32_MAX;
    AmdUvd6 engine(rig.uvd, rig.clock);
    CHECK(engine.start(rig.firmware, rig.memory, rig.config) == AmdUvd6::Status::TimedOut && !engine.started());
    CHECK(rig.uvd.attempts == 1 + rig.config.boot_attempts);   // (the release that starts the VCPU, then one reset and release per try)
    CHECK(at(rig.uvd, reg::kRbCntl) == 0x11010101 && at(rig.uvd, reg::kVcpuCntl) == 0 && at(rig.uvd, reg::kStatus) == 0 && at(rig.uvd, reg::kSoftReset) == 8);
    CHECK(!(at(rig.uvd, reg::kLmiCtrl2) & 0x100));   // the memory path is unstalled again
  }
  {  // A microcode the engine's checks do not accept is the same as no answer.
    Rig rig;
    rig.uvd.expected_first_word ^= 1;
    AmdUvd6 engine(rig.uvd, rig.clock);
    CHECK(engine.start(rig.firmware, rig.memory, rig.config) == AmdUvd6::Status::TimedOut);
  }
  {  // At least one try is made even when none is asked for.
    Rig rig;
    rig.config.boot_attempts = 0;
    AmdUvd6 engine(rig.uvd, rig.clock);
    CHECK(engine.start(rig.firmware, rig.memory, rig.config) == AmdUvd6::Status::Ok && rig.uvd.attempts == 1);
  }
}

static void test_stop() {
  Rig rig;
  AmdUvd6 engine(rig.uvd, rig.clock);
  CHECK(engine.start(rig.firmware, rig.memory, rig.config) == AmdUvd6::Status::Ok);
  engine.stop();
  CHECK(!engine.started() && at(rig.uvd, reg::kRbCntl) == 0x11010101 && at(rig.uvd, reg::kVcpuCntl) == 0 && at(rig.uvd, reg::kStatus) == 0 && at(rig.uvd, reg::kSoftReset) == 8);
  engine.stop();   // (again: harmless)
  AmdUvd6 never_started(rig.uvd, rig.clock);
  never_started.stop();
  CHECK(!never_started.started());
  // It can start again.
  CHECK(engine.start(rig.firmware, rig.memory, rig.config) == AmdUvd6::Status::Ok && engine.started());
}

static void test_bad_arguments() {
  Rig rig;
  auto untouched = [&](AmdUvd6::Status s) { return s != AmdUvd6::Status::Ok && rig.uvd.writes.empty() && rig.block[0] == 0xCC && rig.ring[0] == 0xCC; };
  AmdUvd6 engine(rig.uvd, rig.clock);
  AmdVcnFirmware none(rig.alloc);
  CHECK(untouched(engine.start(none, rig.memory, rig.config)));                                   // no firmware loaded
  std::vector<uint8_t> vcn = make_firmware(1000, 0x01008210, 1);                                   // a VCN image: wrong block
  AmdVcnFirmware wrong(rig.alloc);
  CHECK(wrong.adopt(ConstByteView(vcn.data(), vcn.size())) == AmdFirmwareStatus::Ok && untouched(engine.start(wrong, rig.memory, rig.config)));
  AmdUvd6::Memory m = rig.memory;
  m.block_gpu_address += 8;                                                                       // not page aligned
  CHECK(untouched(engine.start(rig.firmware, m, rig.config)));
  m = rig.memory; m.block_gpu_address = 0;
  CHECK(untouched(engine.start(rig.firmware, m, rig.config)));
  m = rig.memory; m.block = MmioRegion(m.block.address(), rig.block_size - 4096);                 // window too short
  CHECK(untouched(engine.start(rig.firmware, m, rig.config)));
  m = rig.memory; m.ring = MmioRegion(m.ring.address(), 2048);
  CHECK(untouched(engine.start(rig.firmware, m, rig.config)));
  m = rig.memory; m.ring_gpu_address = m.block_gpu_address + 4096;                                // the ring inside the block
  CHECK(untouched(engine.start(rig.firmware, m, rig.config)));
  m = rig.memory; m.ring_gpu_address = m.block_gpu_address - 2048;                                // overlapping from below
  CHECK(untouched(engine.start(rig.firmware, m, rig.config)));
  m = rig.memory; m.block_gpu_address = UINT64_MAX - 8191 + 1; m.block_gpu_address &= ~4095ull;   // the range wraps
  CHECK(untouched(engine.start(rig.firmware, m, rig.config)));
  AmdUvd6::Config c = rig.config;
  c.max_handles = 0;
  CHECK(untouched(engine.start(rig.firmware, rig.memory, c)));
  c.max_handles = 41;
  CHECK(untouched(engine.start(rig.firmware, rig.memory, c)));
  CHECK(!engine.started());
}

static void test_firmware_names() {
  CHECK(strcmp(amd_uvd6_firmware_name(0x694C), "vegam_uvd.bin") == 0 && strcmp(amd_uvd6_firmware_name(0x694E), "vegam_uvd.bin") == 0 && strcmp(amd_uvd6_firmware_name(0x694F), "vegam_uvd.bin") == 0);
  CHECK(amd_uvd6_firmware_name(0x694B) == nullptr && amd_uvd6_firmware_name(0x67DF) == nullptr && amd_uvd6_firmware_name(0x15D8) == nullptr && amd_uvd6_firmware_name(0) == nullptr);
}

// The real firmware file from /firmware (the tests run from the repository root).
static void test_real_firmware() {
  FILE* f = fopen("firmware/vegam_uvd.bin", "rb");
  CHECK(f != nullptr);
  if (!f) return;
  std::vector<uint8_t> bytes;
  uint8_t buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
  fclose(f);
  TestAllocator alloc;
  AmdVcnFirmware firmware(alloc);
  CHECK(firmware.adopt(ConstByteView(bytes.data(), bytes.size())) == AmdFirmwareStatus::Ok);
  const AmdFirmwareHeader& h = firmware.header();
  CHECK(h.ip_version_major == 6 && h.ip_version_minor == 3 && h.ucode_array_offset_bytes == 256 && h.ucode_size_bytes == 375168 && h.ucode_version == 0x01008210);
  CHECK(AmdUvd6::max_handles_for(firmware) == 40 && AmdUvd6::firmware_window_bytes(firmware) == 376832);
  CHECK(AmdUvd6::block_bytes(firmware, 40) == 376832 + 200 * 1024 + 256 * 1024 + 40 * 50 * 1024);
  // And a full start with it.
  Rig rig;
  rig.image = bytes;
  CHECK(rig.firmware.adopt(ConstByteView(rig.image.data(), rig.image.size())) == AmdFirmwareStatus::Ok);
  free(rig.block);
  rig.config.max_handles = AmdUvd6::max_handles_for(rig.firmware);
  rig.block_size = AmdUvd6::block_bytes(rig.firmware, rig.config.max_handles);
  rig.block = static_cast<uint8_t*>(aligned_alloc(4096, rig.block_size));
  memset(rig.block, 0xCC, rig.block_size);
  rig.memory.block = MmioRegion(reinterpret_cast<uint64_t>(rig.block), rig.block_size);
  rig.uvd.block_host = rig.block;
  memcpy(&rig.uvd.expected_first_word, rig.image.data() + 256, 4);
  AmdUvd6 engine(rig.uvd, rig.clock);
  CHECK(engine.start(rig.firmware, rig.memory, rig.config) == AmdUvd6::Status::Ok);
  CHECK(at(rig.uvd, reg::kSize0) == 376832 - 256 && at(rig.uvd, reg::kOffset1) == 376832 / 8 && at(rig.uvd, reg::kScratch4) == 40);
  CHECK(memcmp(rig.block, rig.image.data() + 256, 375168) == 0);
}

int main() {
  test_layout();
  test_start();
  test_order();
  test_boot_retries_and_failure();
  test_stop();
  test_bad_arguments();
  test_firmware_names();
  test_real_firmware();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("amd uvd6 tests passed\n");
  return 0;
}
