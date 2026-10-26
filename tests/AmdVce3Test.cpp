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


// Host-side tests for the VCE 3.4 start and stop sequences (AmdVce3) against a simulated engine: the register file of the card with the behaviour the driver relies on -
// the two instances' registers are banked by GRBM_GFX_INDEX, an instance's ECPU reports its microcode loaded in VCE_STATUS only when its clock is on, the memory
// windows are programmed as the driver programs them and point at a block that holds the microcode, and the reset is released - and a record of every write in order.
// The register numbers here are typed in from AMD's vce_3_0_d.h / gfx_8_0_d.h / smu_7_1_2_d.h, independently of the generated ones the code uses.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdVce3.hpp"
#include "FakeAmdVcn.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace reg {
constexpr uint32_t kStatus = 0x8001, kVcpuCntl = 0x8005, kOffset0 = 0x8009, kSize0 = 0x800a, kOffset1 = 0x800b, kSize1 = 0x800c, kOffset2 = 0x800d, kSize2 = 0x800e;
constexpr uint32_t kSoftReset = 0x8048, kBaseLo2 = 0x805b, kBaseHi2 = 0x805c, kSize2r = 0x805d, kRptr2 = 0x805e, kWptr2 = 0x805f;
constexpr uint32_t kBaseLo = 0x8060, kBaseHi = 0x8061, kSizeR = 0x8062, kRptr = 0x8063, kWptr = 0x8064, kArbCtrl = 0x809f, kGatingA = 0x80be, kGatingB = 0x80bf;
constexpr uint32_t kBaseLo3 = 0x80d4, kBaseHi3 = 0x80d5, kSize3 = 0x80d6, kRptr3 = 0x80d7, kWptr3 = 0x80d8;
constexpr uint32_t kUencGating = 0x81ef, kUencRegGating = 0x81f0, kUencGating2 = 0x8210, kUencDmaDclk = 0x8390, kSysIntEn = 0x8540;
constexpr uint32_t kLmiCtrl2 = 0x859d, kLmiCtrl = 0x85a6, kLmiVmCtrl = 0x85a8, kLmiSwap = 0x85ad, kLmiSwap1 = 0x85ae, kLmiCacheCtrl = 0x85bd;
constexpr uint32_t kBar0 = 0x8616, kBar1 = 0x8617, kBar2 = 0x8618;
constexpr uint32_t kGrbmGfxIndex = 0xc200, kSmcIndex = 0x80, kSmcData = 0x81, kHdpFlush = 0x1520;
constexpr uint32_t ixCcHarvestFuses = 0xc00c0028;
}  // namespace reg

static bool is_vce_register(uint32_t r) { return r >= 0x8000 && r < 0x8700; }

struct FakeVce final : IVcnRegisters {
  std::map<uint32_t, uint32_t> bank[2];       // the registers of each instance
  std::map<uint32_t, uint32_t> global;        // everything else
  std::vector<std::pair<uint32_t, uint32_t>> writes;      // every write: (register, value), with the instance a VCE register went to in bits 32+ of a second list
  std::vector<int> write_instance;
  uint32_t grbm_index = 0xE0000000;
  uint32_t fuses = 0;                          // the VCE_DISABLE field of CC_HARVEST_FUSES
  uint32_t smc_index = 0;
  // The memory block the engine will look at: where the test put it, and what its first word must be for the firmware to be accepted.
  uint64_t block_gpu = 0;
  const uint8_t* block_host = nullptr;
  uint32_t expected_first_word = 0;
  uint32_t fail_first = 0;       // boot attempts that do not succeed before one does (UINT32_MAX: never)
  uint32_t attempts[2] = {0, 0};
  int dead_instance = -1;        // an instance that never boots
  uint32_t selected() const { return (grbm_index >> 4) & 1; }

  FakeVce() { bank[0][reg::kSoftReset] = 1; bank[1][reg::kSoftReset] = 1; }   // (the ECPU is held in reset until the driver releases it)
  uint32_t read(uint32_t r) override {
    if (is_vce_register(r)) { auto& b = bank[selected()]; auto it = b.find(r); return it == b.end() ? 0 : it->second; }
    if (r == reg::kGrbmGfxIndex) return grbm_index;
    if (r == reg::kSmcData) return smc_index == reg::ixCcHarvestFuses ? fuses << 1 : 0;
    auto it = global.find(r);
    return it == global.end() ? 0 : it->second;
  }
  void write(uint32_t r, uint32_t v) override {
    writes.push_back({r, v});
    write_instance.push_back(is_vce_register(r) ? static_cast<int>(selected()) : -1);
    if (r == reg::kGrbmGfxIndex) { grbm_index = v; return; }
    if (r == reg::kSmcIndex) { smc_index = v; return; }
    if (!is_vce_register(r)) { global[r] = v; return; }
    const uint32_t i = selected();
    const uint32_t old = read(r);
    bank[i][r] = v;
    if (r == reg::kSoftReset && (old & 1) && !(v & 1)) boot(i);
  }
  void boot(uint32_t i) {
    ++attempts[i];
    auto& b = bank[i];
    const bool clock_on = b[reg::kVcpuCntl] & 1;
    const uint64_t bar = block_gpu >> 8;
    uint32_t first = 0;
    if (block_host) memcpy(&first, block_host, 4);
    const bool bars = b[reg::kBar0] == bar && b[reg::kBar1] == bar && b[reg::kBar2] == bar;
    const bool lmi = b[reg::kLmiCtrl] == 0x00398000 && b[reg::kLmiVmCtrl] == 0;
    const bool windows = b[reg::kSize0] != 0 && b[reg::kSize1] != 0 && b[reg::kSize2] != 0 && b[reg::kOffset0] == 256;
    if (static_cast<int>(i) != dead_instance && clock_on && bars && lmi && windows && first == expected_first_word && fail_first != UINT32_MAX && attempts[i] > fail_first) b[reg::kStatus] |= 2;
  }
};

struct TestAllocator final : allocators::IAllocator {
  std::vector<void*> blocks;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment); blocks.push_back(p); return p; }
  void deallocate(void*) override {}
  ~TestAllocator() { for (void* p : blocks) free(p); }
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

// A firmware image: the 32-byte common header (IP version `ip_major`.4), padding up to the microcode at offset 256, and `ucode_bytes` of microcode.
static std::vector<uint8_t> make_firmware(uint32_t ucode_bytes, uint32_t version, uint16_t ip_major = 3) {
  const uint32_t offset = 256;
  std::vector<uint8_t> image(offset + ucode_bytes);
  auto put32 = [&](uint32_t at, uint32_t v) { for (int i = 0; i < 4; ++i) image[at + i] = static_cast<uint8_t>(v >> (8 * i)); };
  auto put16 = [&](uint32_t at, uint16_t v) { image[at] = static_cast<uint8_t>(v); image[at + 1] = static_cast<uint8_t>(v >> 8); };
  put32(0, static_cast<uint32_t>(image.size()));
  put32(4, 32);
  put16(8, 1); put16(10, 0);
  put16(12, ip_major); put16(14, 4);
  put32(16, version);
  put32(20, ucode_bytes);
  put32(24, offset);
  for (uint32_t i = 0; i < ucode_bytes; ++i) image[offset + i] = static_cast<uint8_t>(0x21 + i * 5);
  return image;
}

struct Rig {
  TestAllocator alloc;
  AmdVcnFirmware firmware{alloc};
  std::vector<uint8_t> image;
  FakeVce vce;
  fakevcn::FakeClock clock;
  uint8_t* block = nullptr;
  uint8_t* rings[3] = {};
  AmdVce3::Memory memory;
  AmdVce3::Config config;
  explicit Rig(uint32_t ucode_bytes = 40000, uint32_t version = 0x03501503, uint16_t ip_major = 3) {
    image = make_firmware(ucode_bytes, version, ip_major);
    firmware.adopt(ConstByteView(image.data(), image.size()));
    config.boot_attempts = 3;
    block = static_cast<uint8_t*>(aligned_alloc(4096, AmdVce3::kBlockBytes));
    memset(block, 0xCC, AmdVce3::kBlockBytes);
    memory.block_gpu_address = 0x2'0000'0000ull;
    memory.block = MmioRegion(reinterpret_cast<uint64_t>(block), AmdVce3::kBlockBytes);
    for (int r = 0; r < 3; ++r) {
      rings[r] = static_cast<uint8_t*>(aligned_alloc(4096, 4096));
      memset(rings[r], 0xCC, 4096);
      memory.ring_gpu_address[r] = 0x2'0100'0000ull + 0x10000ull * r;
      memory.ring[r] = MmioRegion(reinterpret_cast<uint64_t>(rings[r]), 4096);
    }
    vce.block_gpu = memory.block_gpu_address;
    vce.block_host = block;
    memcpy(&vce.expected_first_word, firmware.ucode().data(), 4);
  }
  ~Rig() { free(block); for (uint8_t* r : rings) free(r); }
};

static uint32_t at(FakeVce& v, int instance, uint32_t r) { auto it = v.bank[instance].find(r); return it == v.bank[instance].end() ? 0 : it->second; }
static uint32_t gat(FakeVce& v, uint32_t r) { auto it = v.global.find(r); return it == v.global.end() ? 0 : it->second; }
static int writes_to_instance(const FakeVce& v, int instance) {
  int n = 0;
  for (int i : v.write_instance) n += i == instance;
  return n;
}

static void test_names_and_versions() {
  CHECK(strcmp(amd_vce3_firmware_name(0x694C), "vegam_vce.bin") == 0 && strcmp(amd_vce3_firmware_name(0x694E), "vegam_vce.bin") == 0 && strcmp(amd_vce3_firmware_name(0x694F), "vegam_vce.bin") == 0);
  CHECK(amd_vce3_firmware_name(0x15DD) == nullptr && amd_vce3_firmware_name(0x67DF) == nullptr && amd_vce3_firmware_name(0) == nullptr);
  // The version word of the header is (major << 20 | minor << 8 | binary id); the driver keeps (major << 24 | minor << 16 | binary id << 8).
  Rig rig;
  CHECK(AmdVce3::firmware_version(rig.firmware) == ((53u << 24) | (21u << 16) | (3u << 8)));
  CHECK(AmdVce3::rings_for((52u << 24) | (8u << 16) | (3u << 8)) == 3 && AmdVce3::rings_for((52u << 24) | (8u << 16) | (2u << 8)) == 2 && AmdVce3::rings_for((40u << 24) | (2u << 16)) == 2 &&
        AmdVce3::rings_for(53u << 24) == 3);
  CHECK(AmdVce3::firmware_usable(rig.firmware));
  Rig wrong_ip(40000, 0x03501503, 6);
  CHECK(!AmdVce3::firmware_usable(wrong_ip.firmware));
  Rig too_big(AmdVce3::kFirmwareWindowBytes + 4);
  CHECK(!AmdVce3::firmware_usable(too_big.firmware));
  Rig fits(AmdVce3::kFirmwareWindowBytes);
  CHECK(AmdVce3::firmware_usable(fits.firmware));
  AmdVcnFirmware none(rig.alloc);
  CHECK(!AmdVce3::firmware_usable(none));
  // The layout: the microcode window, then a stack and a data area for each of the two instances.
  CHECK(AmdVce3::kDataBytes == 16 * 1024 * 16 + 52 * 1024 && AmdVce3::kBlockBytes == 384 * 1024 + uint64_t{64 * 1024 + AmdVce3::kDataBytes} * 2);
}

static void test_harvest() {
  FakeVce v;
  for (uint32_t fuse = 0; fuse < 4; ++fuse) {
    v.fuses = fuse;
    const uint32_t expected = fuse == 1 ? AmdVce3::kHarvestVce0 : fuse == 2 ? AmdVce3::kHarvestVce1 : fuse == 3 ? 3u : AmdVce3::kHarvestVce1;   // (no fuse: a Polaris-family chip has the second instance off)
    CHECK(AmdVce3::read_harvest_config(v) == expected);
  }
  CHECK(v.smc_index == reg::ixCcHarvestFuses);
}

static void test_real_firmware() {
  const std::vector<uint8_t> image = read_file("firmware/vegam_vce.bin");
  CHECK(image.size() > 150000);   // the file is in the repository, not an LFS pointer
  TestAllocator alloc;
  AmdVcnFirmware fw(alloc);
  CHECK(fw.adopt(ConstByteView(image.data(), image.size())) == AmdFirmwareStatus::Ok);
  CHECK(AmdVce3::firmware_usable(fw) && fw.header().ip_version_major == 3 && fw.header().ip_version_minor == 4);
  CHECK(AmdVce3::firmware_version(fw) >= ((52u << 24) | (8u << 16) | (3u << 8)) && AmdVce3::rings_for(AmdVce3::firmware_version(fw)) == 3);
}

static void test_start_default() {
  Rig rig;
  AmdVce3 vce(rig.vce, rig.clock);
  CHECK(vce.start(rig.firmware, rig.memory, rig.config) == AmdVce3::Status::Ok && vce.started());
  FakeVce& v = rig.vce;
  CHECK(rig.vce.attempts[0] == 1 && rig.vce.attempts[1] == 0 && writes_to_instance(v, 1) == 0);   // (the second instance is fused off: not touched)
  CHECK(v.grbm_index == 0xE0000000);
  // The rings: pointers at the start, the base and the size in dwords.
  CHECK(at(v, 0, reg::kRptr) == 0 && at(v, 0, reg::kWptr) == 0 && at(v, 0, reg::kBaseLo) == 0x0100'0000u && at(v, 0, reg::kBaseHi) == 2 && at(v, 0, reg::kSizeR) == 1024);
  CHECK(at(v, 0, reg::kBaseLo2) == 0x0101'0000u && at(v, 0, reg::kBaseHi2) == 2 && at(v, 0, reg::kSize2r) == 1024);
  CHECK(at(v, 0, reg::kBaseLo3) == 0x0102'0000u && at(v, 0, reg::kBaseHi3) == 2 && at(v, 0, reg::kSize3) == 1024);
  // The memory controller side: the clock gating starting values, the LMI control, the three 40-bit bases (the block's address >> 8), the three windows of instance 0.
  CHECK(at(v, 0, reg::kLmiCtrl) == 0x00398000 && at(v, 0, reg::kLmiSwap) == 0 && at(v, 0, reg::kLmiSwap1) == 0 && at(v, 0, reg::kLmiVmCtrl) == 0);
  CHECK(at(v, 0, reg::kBar0) == static_cast<uint32_t>(0x2'0000'0000ull >> 8) && at(v, 0, reg::kBar1) == at(v, 0, reg::kBar0) && at(v, 0, reg::kBar2) == at(v, 0, reg::kBar0));
  CHECK(at(v, 0, reg::kOffset0) == 256 && at(v, 0, reg::kSize0) == 384 * 1024);
  CHECK(at(v, 0, reg::kOffset1) == 256 + 384 * 1024 && at(v, 0, reg::kSize1) == 64 * 1024);
  CHECK(at(v, 0, reg::kOffset2) == 256 + 384 * 1024 + 64 * 1024 && at(v, 0, reg::kSize2) == AmdVce3::kDataBytes);
  CHECK(at(v, 0, reg::kGatingB) == 0x1FF && (at(v, 0, reg::kUencRegGating) & 0x3F) == 0x3F && (at(v, 0, reg::kGatingA) & (1u << 16)) == 0);
  CHECK((at(v, 0, reg::kSysIntEn) & 8) != 0);                     // the trap interrupt
  CHECK((at(v, 0, reg::kVcpuCntl) & 0x00100001) == 0x00100001);   // the clock on, and the LMI bit
  CHECK((at(v, 0, reg::kSoftReset) & 1) == 0 && (at(v, 0, reg::kStatus) & 2) != 0 && (at(v, 0, reg::kStatus) & 1) == 0);   // out of reset, the microcode loaded, no longer busy
  CHECK((at(v, 0, reg::kArbCtrl) & 0x10000) != 0);                // the clock gating override (hw_init) stays on (the sw clock gating that clears it is not run)
  // The memory: the microcode at the start of the block, zeros behind it; the rings cleared; the host data path flushed.
  CHECK(memcmp(rig.block, rig.firmware.ucode().data(), rig.firmware.ucode().size()) == 0);
  bool clean = true;
  for (uint64_t i = rig.firmware.ucode().size(); i < AmdVce3::kBlockBytes; ++i) clean = clean && rig.block[i] == 0;
  for (int r = 0; r < 3; ++r)
    for (int i = 0; i < 4096; ++i) clean = clean && rig.rings[r][i] == 0;
  CHECK(clean);
  CHECK(gat(v, reg::kHdpFlush) == 1);
  // Stop: the clock off, the ECPU held, the status cleared, the default instance selected again.
  vce.stop();
  CHECK(!vce.started() && (at(v, 0, reg::kVcpuCntl) & 1) == 0 && (at(v, 0, reg::kSoftReset) & 1) == 1 && at(v, 0, reg::kStatus) == 0 && v.grbm_index == 0xE0000000);
  CHECK(at(v, 0, reg::kVcpuCntl) & 0x00100000);   // (only the two bits of the mask are cleared)
  // Restart: the pointers start where the rings' write pointers are.
  rig.config.ring_write_pointer[0] = 64;
  rig.vce.attempts[0] = 0;
  CHECK(vce.start(rig.firmware, rig.memory, rig.config) == AmdVce3::Status::Ok && at(v, 0, reg::kRptr) == 64 && at(v, 0, reg::kWptr) == 64);
}

static void test_two_instances() {
  {  // Neither fused off: both are programmed, the rings only on the first; the second instance's windows follow the first's.
    Rig rig;
    rig.config.harvest_config = 0;
    AmdVce3 vce(rig.vce, rig.clock);
    CHECK(vce.start(rig.firmware, rig.memory, rig.config) == AmdVce3::Status::Ok);
    FakeVce& v = rig.vce;
    CHECK(rig.vce.attempts[0] == 1 && rig.vce.attempts[1] == 1);
    CHECK(at(v, 1, reg::kBaseLo) == 0 && at(v, 1, reg::kSizeR) == 0 && at(v, 1, reg::kSize3) == 0);   // (the ring registers belong to instance 0)
    CHECK(at(v, 1, reg::kOffset0) == 256 && at(v, 1, reg::kSize0) == 384 * 1024);
    CHECK(at(v, 1, reg::kOffset1) == 256 + 384 * 1024 + 64 * 1024 + AmdVce3::kDataBytes && at(v, 1, reg::kSize1) == 64 * 1024);
    CHECK(at(v, 1, reg::kOffset2) == at(v, 1, reg::kOffset1) + 64 * 1024 && at(v, 1, reg::kSize2) == AmdVce3::kDataBytes);
    CHECK((at(v, 1, reg::kStatus) & 2) != 0 && (at(v, 1, reg::kSysIntEn) & 8) != 0);
    CHECK(v.grbm_index == 0xE0000000);
    vce.stop();
    CHECK((at(v, 1, reg::kSoftReset) & 1) == 1 && at(v, 1, reg::kStatus) == 0);
  }
  {  // Only the second is there: the rings go to it.
    Rig rig;
    rig.config.harvest_config = AmdVce3::kHarvestVce0;
    AmdVce3 vce(rig.vce, rig.clock);
    CHECK(vce.start(rig.firmware, rig.memory, rig.config) == AmdVce3::Status::Ok);
    FakeVce& v = rig.vce;
    CHECK(rig.vce.attempts[0] == 0 && rig.vce.attempts[1] == 1);
    CHECK(writes_to_instance(v, 0) == 1 && at(v, 0, reg::kArbCtrl) != 0);   // (only the clock gating override, which the driver writes with the default instance selected)
    CHECK(at(v, 1, reg::kBaseLo) == 0x0100'0000u && at(v, 1, reg::kSizeR) == 1024 && at(v, 1, reg::kOffset1) == 256 + 384 * 1024 + 64 * 1024 + AmdVce3::kDataBytes);
  }
  {  // Both fused off: no engine, nothing written.
    Rig rig;
    rig.config.harvest_config = 3;
    AmdVce3 vce(rig.vce, rig.clock);
    CHECK(vce.start(rig.firmware, rig.memory, rig.config) == AmdVce3::Status::NoEngine && rig.vce.writes.empty() && rig.block[0] == 0xCC);
  }
}

static void test_rings_and_gating() {
  {  // Two rings (firmware before 52.8.3): the third ring's registers are written with zeros, as the driver writes its uninitialised ring.
    Rig rig;
    rig.config.rings = 2;
    AmdVce3 vce(rig.vce, rig.clock);
    CHECK(vce.start(rig.firmware, rig.memory, rig.config) == AmdVce3::Status::Ok);
    FakeVce& v = rig.vce;
    CHECK(at(v, 0, reg::kSizeR) == 1024 && at(v, 0, reg::kSize2r) == 1024 && at(v, 0, reg::kSize3) == 0 && at(v, 0, reg::kBaseLo3) == 0 && at(v, 0, reg::kBaseHi3) == 0);
    bool third_ring_memory_untouched = true;
    for (int i = 0; i < 4096; ++i) third_ring_memory_untouched = third_ring_memory_untouched && rig.rings[2][i] == 0xCC;
    CHECK(third_ring_memory_untouched);
  }
  {  // Medium grain clock gating asked for: the ungate sequence runs before the start (the delays of VCE_CLOCK_GATING_A and VCE_UENC_CLOCK_GATING, then the software gating).
    Rig rig;
    rig.config.medium_grain_clock_gating = true;
    AmdVce3 vce(rig.vce, rig.clock);
    CHECK(vce.start(rig.firmware, rig.memory, rig.config) == AmdVce3::Status::Ok);
    FakeVce& v = rig.vce;
    CHECK(((at(v, 0, reg::kGatingA) >> 4) & 0xFF) == 0x04 && (at(v, 0, reg::kGatingA) & 0xF) == 0);   // the clock on/off delay (mc_resume only clears bit 16)
    bool dclk_written = false;
    for (auto& w : v.writes) dclk_written = dclk_written || (w.first == reg::kUencDmaDclk && (w.second & 0x8));
    CHECK(dclk_written);   // the force-on bits of the DMA clock control (ungated = clocks forced on)
    CHECK((at(v, 0, reg::kArbCtrl) & 0x10000) == 0);   // the sw clock gating ends by clearing the override
  }
  {  // Not asked for: none of that runs.
    Rig rig;
    AmdVce3 vce(rig.vce, rig.clock);
    CHECK(vce.start(rig.firmware, rig.memory, rig.config) == AmdVce3::Status::Ok);
    bool dclk_written = false;
    for (auto& w : rig.vce.writes) dclk_written = dclk_written || w.first == reg::kUencDmaDclk;
    CHECK(!dclk_written);
  }
}

static void test_failures() {
  {  // It never answers: TimedOut after the allowed tries, the engine stopped the way the driver stops it, and a snapshot of the moment it gave up.
    Rig rig;
    rig.vce.fail_first = UINT32_MAX;
    AmdVce3 vce(rig.vce, rig.clock);
    CHECK(vce.start(rig.firmware, rig.memory, rig.config) == AmdVce3::Status::TimedOut && !vce.started());
    CHECK(rig.vce.attempts[0] == 1 + rig.config.boot_attempts);   // (the release that starts the ECPU, then one reset and release per try)
    FakeVce& v = rig.vce;
    CHECK((at(v, 0, reg::kVcpuCntl) & 1) == 0 && (at(v, 0, reg::kSoftReset) & 1) == 1 && at(v, 0, reg::kStatus) == 0);
    const AmdVce3::Diagnostics d = vce.failure_snapshot();
    CHECK((d.vcpu_cntl & 1) == 1 && (d.soft_reset & 1) == 0 && (d.status & 2) == 0 && d.lmi_ctrl == 0x00398000);   // the state at the moment it gave up, not after the stop
    // The snapshot function reads the instance the start programs and leaves the default selected.
    v.bank[0][reg::kRptr] = 0x11;
    CHECK(AmdVce3::read_diagnostics(v, 0).rb_rptr == 0x11 && v.grbm_index == 0xE0000000);
  }
  {  // A microcode the engine's checks do not accept is the same as no answer.
    Rig rig;
    rig.vce.expected_first_word ^= 1;
    AmdVce3 vce(rig.vce, rig.clock);
    CHECK(vce.start(rig.firmware, rig.memory, rig.config) == AmdVce3::Status::TimedOut);
  }
  {  // It answers after resets: two failures, then the third attempt works.
    Rig rig;
    rig.vce.fail_first = 2;
    AmdVce3 vce(rig.vce, rig.clock);
    CHECK(vce.start(rig.firmware, rig.memory, rig.config) == AmdVce3::Status::Ok && rig.vce.attempts[0] == 3);
  }
  {  // At least one try is made even when none is asked for.
    Rig rig;
    rig.config.boot_attempts = 0;
    AmdVce3 vce(rig.vce, rig.clock);
    CHECK(vce.start(rig.firmware, rig.memory, rig.config) == AmdVce3::Status::Ok && rig.vce.attempts[0] == 1);
  }
  {  // The second instance failing fails the start (and stops both).
    Rig rig;
    rig.config.harvest_config = 0;
    rig.vce.dead_instance = 1;
    AmdVce3 vce(rig.vce, rig.clock);
    CHECK(vce.start(rig.firmware, rig.memory, rig.config) == AmdVce3::Status::TimedOut && !vce.started());
    CHECK(rig.vce.attempts[0] == 1 && rig.vce.attempts[1] == 1 + rig.config.boot_attempts);
    // Both are put to sleep again, and the snapshot is of the instance that failed (the second has no ring pointers of its own: they are the first's).
    CHECK((at(rig.vce, 0, reg::kSoftReset) & 1) == 1 && (at(rig.vce, 1, reg::kSoftReset) & 1) == 1 && rig.vce.grbm_index == 0xE0000000);
  }
}

static void test_refusals() {
  {  // A firmware that is not VCE 3 (a UVD image), none at all, or too big.
    Rig rig(40000, 0x03501503, 6);
    AmdVce3 vce(rig.vce, rig.clock);
    CHECK(vce.start(rig.firmware, rig.memory, rig.config) == AmdVce3::Status::BadFirmware && rig.vce.writes.empty() && rig.block[0] == 0xCC);
  }
  {  // Memory the engine cannot use: unaligned, too small, overlapping, a ring count that does not exist. Nothing is written.
    Rig rig;
    AmdVce3 vce(rig.vce, rig.clock);
    AmdVce3::Memory bad = rig.memory;
    bad.block_gpu_address += 16;
    CHECK(vce.start(rig.firmware, bad, rig.config) == AmdVce3::Status::BadMemory);
    bad = rig.memory;
    bad.block = MmioRegion(reinterpret_cast<uint64_t>(rig.block), AmdVce3::kBlockBytes - 4);
    CHECK(vce.start(rig.firmware, bad, rig.config) == AmdVce3::Status::BadMemory);
    bad = rig.memory;
    bad.ring_gpu_address[1] = bad.ring_gpu_address[0] + 2048;   // two rings overlap
    CHECK(vce.start(rig.firmware, bad, rig.config) == AmdVce3::Status::BadMemory);
    bad = rig.memory;
    bad.ring_gpu_address[2] = bad.block_gpu_address + 4096;     // a ring inside the block
    CHECK(vce.start(rig.firmware, bad, rig.config) == AmdVce3::Status::BadMemory);
    bad = rig.memory;
    bad.ring[0] = MmioRegion(reinterpret_cast<uint64_t>(rig.rings[0]), 2048);
    CHECK(vce.start(rig.firmware, bad, rig.config) == AmdVce3::Status::BadMemory);
    AmdVce3::Config odd = rig.config;
    odd.rings = 1;
    CHECK(vce.start(rig.firmware, rig.memory, odd) == AmdVce3::Status::BadMemory);
    odd.rings = 4;
    CHECK(vce.start(rig.firmware, rig.memory, odd) == AmdVce3::Status::BadMemory);
    CHECK(rig.vce.writes.empty() && rig.block[0] == 0xCC && rig.rings[0][0] == 0xCC);
  }
  {  // Stopping an engine that never started is harmless.
    Rig rig;
    AmdVce3 vce(rig.vce, rig.clock);
    vce.stop();
    CHECK(!vce.started());
  }
}

int main() {
  test_names_and_versions();
  test_harvest();
  test_real_firmware();
  test_start_default();
  test_two_instances();
  test_rings_and_gating();
  test_failures();
  test_refusals();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("amd vce3 tests passed\n");
  return 0;
}
