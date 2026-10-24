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

// Host-side tests for the VCN 1.0 start and stop sequences (AmdVcn1). A simulated register file stands in for the card: it logs every write in order and
// models the one thing the sequence waits for (the VCPU reporting IDLE once it is let out of reset). The shipped Raven microcode is the firmware. What is
// checked: the order of the steps, every address and size programmed (and that each lies inside the memory the engine was given), the timeout path, and
// that bad input touches nothing. That the register *names* are visited in the Linux driver's order is checked by scripts/check_vcn1_port.py.
// Run from the repository root.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <utility>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

using namespace hardware;

namespace {
// The register block's segment bases for Raven (Vega10 table), written out separately from the driver's copy.
constexpr uint32_t kUvdBase[5] = {0x7800, 0x7E00, 0, 0, 0};
constexpr uint32_t mmUVD_RBC_XX_IB_REG_CHECK_1_0 = 0x05ab, mmUVD_RBC_XX_IB_REG_CHECK_1_0_BASE_IDX = 1;
constexpr uint32_t mmUVD_REG_XX_MASK_1_0 = 0x05ac, mmUVD_REG_XX_MASK_1_0_BASE_IDX = 1;
constexpr uint32_t UVD_PGFSM_STATUS__UVDM_UVDU_PWR_ON = 0x2AAAA0, UVD_STATUS__UVD_BUSY = 4, UVD_STATUS__IDLE = 2;
#include "Hardware/VideoCodec/AMD/AmdVcn1Registers.inc"
}  // namespace

#define REG(name) (kUvdBase[name##_BASE_IDX] + (name))

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct FakeClock final : IClock {
  mutable uint64_t now = 0;
  uint64_t now_us() const override { now += 50; return now; }
  const char* name() const override { return "fake clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

// The registers of a card whose VCN is idle in reset until the sequence lets the VCPU out.
struct FakeRegisters final : IVcnRegisters {
  std::map<uint32_t, uint32_t> value;
  std::vector<std::pair<uint32_t, uint32_t>> log;   // every write, in order
  bool responsive = true;                          // does the VCPU report IDLE when released?
  bool power_gating = false;                       // what the power state machine will answer
  FakeRegisters() {
    value[REG(mmUVD_LMI_STATUS)] = 0xFFFFFFFFu;     // all the "clean" bits the stop sequence waits for
    value[REG(mmUVD_CGC_CTRL)] = 0xFFFFFFFFu;
    value[REG(mmJPEG_CGC_CTRL)] = 0xFFFFFFFFu;
  }
  uint32_t read(uint32_t dword) override { return value[dword]; }
  void write(uint32_t dword, uint32_t v) override {
    log.push_back({dword, v});
    value[dword] = v;
    if (dword == REG(mmUVD_SOFT_RESET) && !(v & UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK) && responsive) value[REG(mmUVD_STATUS)] |= UVD_STATUS__IDLE;
    if (dword == REG(mmUVD_PGFSM_CONFIG)) value[REG(mmUVD_PGFSM_STATUS)] = power_gating ? UVD_PGFSM_STATUS__UVDM_UVDU_PWR_ON : 0;
  }
  // The position in the log of the first write to `dword` (optionally with a value test), or -1.
  int first(uint32_t dword, int from = 0) const {
    for (size_t i = static_cast<size_t>(from); i < log.size(); ++i) if (log[i].first == dword) return static_cast<int>(i);
    return -1;
  }
  int count(uint32_t dword) const { int n = 0; for (const auto& w : log) n += w.first == dword; return n; }
  uint32_t last(uint32_t dword) const { for (size_t i = log.size(); i-- > 0;) if (log[i].first == dword) return log[i].second; return 0xBAD0BAD0u; }
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

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return malloc(size); }
  void deallocate(void* p) override { free(p); }
};

constexpr uint32_t kSentinel = 0xDEADBEEFu;

struct Setup {
  TestAllocator a;
  AmdVcnFirmware firmware{a};
  std::vector<uint32_t> block, rings;
  AmdVcn1::Memory memory;
  AmdVcn1::Config config;
  uint64_t block_gpu = 0x280000000ull, rings_gpu = 0x270000000ull;   // above 4 GiB: the high halves matter

  Setup() {
    const std::vector<uint8_t> image = read_host_file("firmware/raven_vcn.bin");
    CHECK(firmware.adopt(ConstByteView(image.data(), image.size())) == AmdFirmwareStatus::Ok);
    block.assign(AmdVcn1::block_bytes(firmware) / 4, kSentinel);
    rings.assign(AmdVcn1::kRingsTotalBytes / 4, kSentinel);
    memory.block_gpu_address = block_gpu;
    memory.block = MmioRegion(reinterpret_cast<uint64_t>(block.data()), block.size() * 4);
    memory.rings_gpu_address = rings_gpu;
    memory.rings = MmioRegion(reinterpret_cast<uint64_t>(rings.data()), rings.size() * 4);
    config.gb_addr_config = 0x26013041;
  }
};

static void test_sizes() {
  Setup s;
  // 369376-byte image: the window is the file plus four bytes in whole pages, then the stack, the context and one shared page.
  CHECK(AmdVcn1::firmware_window_bytes(s.firmware) == 372736 && AmdVcn1::block_bytes(s.firmware) == 372736 + 131072 + 524288 + 4096);
  CHECK(AmdVcn1::kRingsTotalBytes == 12288 && AmdVcn1::kFirmwareOffset == 256);
  // The register block of Raven is a 512 KiB BAR: the registers the sequence uses must all be inside it.
  CHECK((kUvdBase[1] + 0x0700) * 4 < 0x80000);
}

static void test_start(bool power_gating, bool clock_gating) {
  Setup s;
  FakeClock clock;
  FakeRegisters regs;
  regs.power_gating = power_gating;
  s.config.power_gating = power_gating;
  s.config.clock_gating = clock_gating;
  AmdVcn1 vcn(regs, clock);
  CHECK(vcn.start(s.firmware, s.memory, s.config) == AmdVcn1::Status::Ok);

  // The microcode is in the block, from its first byte; everything after it up to the end of the block is zero; the rings are clear.
  const ConstByteView ucode = s.firmware.ucode();
  bool same = true;
  for (size_t i = 0; i < ucode.size(); ++i) same = same && ((s.block[i / 4] >> (8 * (i % 4))) & 0xFF) == ucode[i];
  CHECK(same);
  bool zero = true;
  for (size_t i = ucode.size() / 4; i < s.block.size(); ++i) zero = zero && s.block[i] == 0;
  CHECK(zero);
  bool rings_clear = true;
  for (uint32_t w : s.rings) rings_clear = rings_clear && w == 0;
  CHECK(rings_clear);

  // The cache windows: the block as the GPU addresses it, then the stack and the context after the microcode's window.
  const uint64_t size = 372736, g = s.block_gpu;
  CHECK(regs.last(REG(mmUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW)) == static_cast<uint32_t>(g) && regs.last(REG(mmUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH)) == static_cast<uint32_t>(g >> 32));
  CHECK(regs.last(REG(mmUVD_VCPU_CACHE_OFFSET0)) == 32 && regs.last(REG(mmUVD_VCPU_CACHE_SIZE0)) == size);   // 256 >> 3
  CHECK(regs.last(REG(mmUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW)) == static_cast<uint32_t>(g + size) && regs.last(REG(mmUVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH)) == static_cast<uint32_t>((g + size) >> 32));
  CHECK(regs.last(REG(mmUVD_VCPU_CACHE_OFFSET1)) == 0 && regs.last(REG(mmUVD_VCPU_CACHE_SIZE1)) == 128 * 1024);
  const uint64_t ctx = g + size + 128 * 1024;
  CHECK(regs.last(REG(mmUVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW)) == static_cast<uint32_t>(ctx) && regs.last(REG(mmUVD_LMI_VCPU_CACHE2_64BIT_BAR_HIGH)) == static_cast<uint32_t>(ctx >> 32));
  CHECK(regs.last(REG(mmUVD_VCPU_CACHE_OFFSET2)) == 0 && regs.last(REG(mmUVD_VCPU_CACHE_SIZE2)) == 512 * 1024);
  // None of those windows reaches past the block the engine was given.
  CHECK(size + 128 * 1024 + 512 * 1024 <= AmdVcn1::block_bytes(s.firmware));

  // The address configuration reaches every unit that reads or writes pictures.
  const uint32_t gb = s.config.gb_addr_config;
  for (uint32_t r : {REG(mmUVD_UDEC_ADDR_CONFIG), REG(mmUVD_UDEC_DB_ADDR_CONFIG), REG(mmUVD_UDEC_DBW_ADDR_CONFIG), REG(mmUVD_UDEC_DBW_UV_ADDR_CONFIG), REG(mmUVD_MIF_CURR_ADDR_CONFIG),
                     REG(mmUVD_MIF_CURR_UV_ADDR_CONFIG), REG(mmUVD_MIF_RECON1_ADDR_CONFIG), REG(mmUVD_MIF_RECON1_UV_ADDR_CONFIG), REG(mmUVD_MIF_REF_ADDR_CONFIG),
                     REG(mmUVD_MIF_REF_UV_ADDR_CONFIG), REG(mmUVD_JPEG_ADDR_CONFIG), REG(mmUVD_JPEG_UV_ADDR_CONFIG)})
    CHECK(regs.last(r) == gb);

  // The rings: three pages, decode then the two encode rings, 4096 bytes each; the size registers are in dwords for the encode rings.
  const uint64_t rg = s.rings_gpu;
  CHECK(regs.last(REG(mmUVD_LMI_RBC_RB_64BIT_BAR_LOW)) == static_cast<uint32_t>(rg) && regs.last(REG(mmUVD_LMI_RBC_RB_64BIT_BAR_HIGH)) == static_cast<uint32_t>(rg >> 32));
  CHECK(regs.last(REG(mmUVD_RB_BASE_LO)) == static_cast<uint32_t>(rg + 4096) && regs.last(REG(mmUVD_RB_BASE_HI)) == static_cast<uint32_t>((rg + 4096) >> 32));
  CHECK(regs.last(REG(mmUVD_RB_BASE_LO2)) == static_cast<uint32_t>(rg + 8192) && regs.last(REG(mmUVD_RB_BASE_HI2)) == static_cast<uint32_t>((rg + 8192) >> 32));
  CHECK(regs.last(REG(mmUVD_RB_SIZE)) == 1024 && regs.last(REG(mmUVD_RB_SIZE2)) == 1024);
  CHECK(regs.last(REG(mmUVD_RB_RPTR)) == 0 && regs.last(REG(mmUVD_RB_WPTR)) == 0 && regs.last(REG(mmUVD_RB_RPTR2)) == 0 && regs.last(REG(mmUVD_RB_WPTR2)) == 0);
  // The decode ring's control register: first held idle (no fetch), then released (no-fetch cleared), 2^12 bytes, the other bits as the driver sets them.
  const int cntl_first = regs.first(REG(mmUVD_RBC_RB_CNTL));
  const uint32_t idle = regs.log[static_cast<size_t>(cntl_first)].second;
  CHECK(((idle & UVD_RBC_RB_CNTL__RB_BUFSZ_MASK) >> UVD_RBC_RB_CNTL__RB_BUFSZ__SHIFT) == 12);
  CHECK((idle & UVD_RBC_RB_CNTL__RB_NO_FETCH_MASK) && (idle & UVD_RBC_RB_CNTL__RB_NO_UPDATE_MASK) && (idle & UVD_RBC_RB_CNTL__RB_RPTR_WR_EN_MASK));
  CHECK(((idle & UVD_RBC_RB_CNTL__RB_BLKSZ_MASK) >> UVD_RBC_RB_CNTL__RB_BLKSZ__SHIFT) == 1);
  CHECK(regs.count(REG(mmUVD_RBC_RB_CNTL)) == 2 && !(regs.last(REG(mmUVD_RBC_RB_CNTL)) & UVD_RBC_RB_CNTL__RB_NO_FETCH_MASK) && regs.last(REG(mmUVD_RBC_RB_CNTL)) == (idle & ~UVD_RBC_RB_CNTL__RB_NO_FETCH_MASK));

  // The order of the steps: power, the busy flag, clocks, interrupts off, the memory windows, the VCPU clock, the VCPU out of reset, the interrupts on,
  // the busy flag off, and only then the rings.
  const int pg = regs.first(REG(mmUVD_PGFSM_CONFIG)), busy_on = regs.first(REG(mmUVD_STATUS)), cgc = regs.first(REG(mmUVD_CGC_CTRL));
  const int irq_off = regs.first(REG(mmUVD_MASTINT_EN)), lmi = regs.first(REG(mmUVD_LMI_CTRL)), win0 = regs.first(REG(mmUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW));
  const int clk = regs.first(REG(mmUVD_VCPU_CNTL)), release = regs.first(REG(mmUVD_SOFT_RESET)), irq_on = regs.first(REG(mmUVD_MASTINT_EN), irq_off + 1);
  const int busy_off = regs.first(REG(mmUVD_STATUS), busy_on + 1), rb = cntl_first;
  CHECK(pg == 0 && pg < busy_on && busy_on < cgc && cgc < irq_off && irq_off < lmi && lmi < win0 && win0 < clk && clk < release && release < irq_on && irq_on < busy_off && busy_off < rb);
  CHECK(regs.log[static_cast<size_t>(clk)].second == UVD_VCPU_CNTL__CLK_EN_MASK);
  CHECK(regs.log[static_cast<size_t>(busy_on)].second & UVD_STATUS__UVD_BUSY);
  CHECK(!(regs.log[static_cast<size_t>(busy_off)].second & UVD_STATUS__UVD_BUSY) && (regs.log[static_cast<size_t>(busy_off)].second & UVD_STATUS__IDLE));
  CHECK((regs.log[static_cast<size_t>(irq_on)].second & UVD_MASTINT_EN__VCPU_EN_MASK) && !(regs.log[static_cast<size_t>(irq_off)].second & UVD_MASTINT_EN__VCPU_EN_MASK));
  CHECK(regs.value[REG(mmUVD_STATUS)] == UVD_STATUS__IDLE);   // running and idle, busy flag clear

  // Power gating: all units forced on in the plain case; the media units on and the rest left to the state machine with gating enabled (which also sets the enable bit).
  const uint32_t pgfsm = regs.log[static_cast<size_t>(pg)].second;
  auto field = [&](uint32_t shift) { return (pgfsm >> shift) & 3u; };   // each unit's power configuration is two bits
  const uint32_t m = field(UVD_PGFSM_CONFIG__UVDM_PWR_CONFIG__SHIFT), u = field(UVD_PGFSM_CONFIG__UVDU_PWR_CONFIG__SHIFT);
  const uint32_t others[] = {field(UVD_PGFSM_CONFIG__UVDF_PWR_CONFIG__SHIFT), field(UVD_PGFSM_CONFIG__UVDC_PWR_CONFIG__SHIFT), field(UVD_PGFSM_CONFIG__UVDB_PWR_CONFIG__SHIFT),
                             field(UVD_PGFSM_CONFIG__UVDIL_PWR_CONFIG__SHIFT), field(UVD_PGFSM_CONFIG__UVDIR_PWR_CONFIG__SHIFT), field(UVD_PGFSM_CONFIG__UVDTD_PWR_CONFIG__SHIFT),
                             field(UVD_PGFSM_CONFIG__UVDTE_PWR_CONFIG__SHIFT), field(UVD_PGFSM_CONFIG__UVDE_PWR_CONFIG__SHIFT), field(UVD_PGFSM_CONFIG__UVDW_PWR_CONFIG__SHIFT)};
  CHECK(m == 1 && u == 1);
  for (uint32_t o : others) CHECK(o == (power_gating ? 2u : 1u));
  CHECK(((regs.last(REG(mmUVD_POWER_STATUS)) & UVD_POWER_STATUS__UVD_PG_EN_MASK) != 0) == power_gating);
  // Clock gating: dynamic clock mode set with gating enabled, clear without (the registers started with every bit set).
  const uint32_t jpeg_cgc = regs.log[static_cast<size_t>(regs.first(REG(mmJPEG_CGC_CTRL)))].second;
  const uint32_t uvd_cgc = regs.log[static_cast<size_t>(cgc)].second;
  CHECK(((jpeg_cgc & JPEG_CGC_CTRL__DYN_CLOCK_MODE_MASK) != 0) == clock_gating && ((uvd_cgc & UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK) != 0) == clock_gating);
}

static void test_timeout() {
  Setup s;
  FakeClock clock;
  FakeRegisters regs;
  regs.responsive = false;   // the VCPU never answers
  AmdVcn1 vcn(regs, clock);
  CHECK(vcn.start(s.firmware, s.memory, s.config) == AmdVcn1::Status::TimedOut);
  // Ten attempts to reset the VCPU, then it gives up before turning interrupts on or touching a ring...
  int resets = 0;
  for (size_t i = 0; i < regs.log.size(); ++i)
    if (regs.log[i].first == REG(mmUVD_SOFT_RESET) && (regs.log[i].second & UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK)) ++resets;
  CHECK(resets >= 10);
  const int irq_off = regs.first(REG(mmUVD_MASTINT_EN));
  CHECK(regs.count(REG(mmUVD_MASTINT_EN)) == 1 && !(regs.log[static_cast<size_t>(irq_off)].second & UVD_MASTINT_EN__VCPU_EN_MASK));
  CHECK(regs.count(REG(mmUVD_RBC_RB_CNTL)) == 0 && regs.count(REG(mmUVD_RB_BASE_LO)) == 0 && regs.count(REG(mmUVD_LMI_RBC_RB_64BIT_BAR_LOW)) == 0);
  // ...and puts the engine back to sleep: VCPU clock off, in reset, status cleared.
  CHECK(regs.last(REG(mmUVD_STATUS)) == 0);
  CHECK(!(regs.last(REG(mmUVD_VCPU_CNTL)) & UVD_VCPU_CNTL__CLK_EN_MASK) && (regs.last(REG(mmUVD_SOFT_RESET)) & UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK));
}

static void test_stop() {
  Setup s;
  FakeClock clock;
  FakeRegisters regs;
  s.config.power_gating = true;
  regs.power_gating = true;
  AmdVcn1 vcn(regs, clock);
  CHECK(vcn.start(s.firmware, s.memory, s.config) == AmdVcn1::Status::Ok);
  regs.log.clear();
  vcn.stop();
  // Stall the memory channel, VCPU clock off, the memory interfaces and then the VCPU into reset, status zero, clock and power gating back on.
  const int stall = regs.first(REG(mmUVD_LMI_CTRL2)), clk = regs.first(REG(mmUVD_VCPU_CNTL)), umc = regs.first(REG(mmUVD_SOFT_RESET));
  const int lmi = regs.first(REG(mmUVD_SOFT_RESET), umc + 1), vcpu = regs.first(REG(mmUVD_SOFT_RESET), lmi + 1), status = regs.first(REG(mmUVD_STATUS));
  const int cgc = regs.first(REG(mmJPEG_CGC_CTRL)), power = regs.first(REG(mmUVD_POWER_STATUS)), pgfsm = regs.first(REG(mmUVD_PGFSM_CONFIG));
  CHECK(stall == 0 && stall < clk && clk < umc && umc < lmi && lmi < vcpu && vcpu < status && status < cgc && cgc < power && power < pgfsm);
  CHECK(regs.log[static_cast<size_t>(stall)].second & UVD_LMI_CTRL2__STALL_ARB_UMC_MASK && !(regs.log[static_cast<size_t>(clk)].second & UVD_VCPU_CNTL__CLK_EN_MASK));
  CHECK((regs.log[static_cast<size_t>(umc)].second & UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK) && (regs.log[static_cast<size_t>(lmi)].second & UVD_SOFT_RESET__LMI_SOFT_RESET_MASK) &&
        (regs.log[static_cast<size_t>(vcpu)].second & UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK));
  CHECK(regs.log[static_cast<size_t>(status)].second == 0);
  CHECK((regs.log[static_cast<size_t>(power)].second & UVD_POWER_STATUS__UVD_POWER_STATUS_MASK) == 1);   // TILES_OFF before the power is cut
  // Stopping again, or stopping an engine that was never started, is harmless: the same writes, nothing else.
  FakeRegisters other;
  AmdVcn1 never(other, clock);
  never.stop();
  CHECK(other.first(REG(mmUVD_STATUS)) >= 0 && other.last(REG(mmUVD_STATUS)) == 0);
  // Without power gating the stop does not touch the power state.
  FakeRegisters plain;
  AmdVcn1 vcn2(plain, clock);
  CHECK(vcn2.start(s.firmware, s.memory, AmdVcn1::Config{false, false, 0x1234}) == AmdVcn1::Status::Ok);
  plain.log.clear();
  vcn2.stop();
  CHECK(plain.count(REG(mmUVD_PGFSM_CONFIG)) == 0 && plain.count(REG(mmUVD_POWER_STATUS)) == 0 && plain.count(REG(mmUVD_STATUS)) == 1);
}

static void test_bad_input() {
  FakeClock clock;
  auto untouched = [&](Setup& s, FakeRegisters& regs, AmdVcn1::Status expected, const char* what) {
    AmdVcn1 vcn(regs, clock);
    const AmdVcn1::Status got = vcn.start(s.firmware, s.memory, s.config);
    bool memory_clean = true;
    for (uint32_t w : s.block) memory_clean = memory_clean && w == kSentinel;
    for (uint32_t w : s.rings) memory_clean = memory_clean && w == kSentinel;
    if (got != expected || !regs.log.empty() || !memory_clean) { printf("FAILED: %s (status %d, %zu writes, memory clean %d)\n", what, static_cast<int>(got), regs.log.size(), memory_clean); ++g_failed; }
  };
  {  Setup s; FakeRegisters r; s.firmware.clear(); untouched(s, r, AmdVcn1::Status::BadFirmware, "firmware not loaded"); }
  {  Setup s; FakeRegisters r; s.memory.block = MmioRegion(s.memory.block.address(), s.memory.block.length() - 4); untouched(s, r, AmdVcn1::Status::BadMemory, "block window one word short"); }
  {  Setup s; FakeRegisters r; s.memory.rings = MmioRegion(s.memory.rings.address(), AmdVcn1::kRingsTotalBytes - 4); untouched(s, r, AmdVcn1::Status::BadMemory, "ring window one word short"); }
  {  Setup s; FakeRegisters r; s.memory.block = MmioRegion(); untouched(s, r, AmdVcn1::Status::BadMemory, "no block window"); }
  {  Setup s; FakeRegisters r; s.memory.block_gpu_address = 0; untouched(s, r, AmdVcn1::Status::BadMemory, "block at GPU address 0"); }
  {  Setup s; FakeRegisters r; s.memory.rings_gpu_address = 0; untouched(s, r, AmdVcn1::Status::BadMemory, "rings at GPU address 0"); }
  {  Setup s; FakeRegisters r; s.memory.block_gpu_address += 4; untouched(s, r, AmdVcn1::Status::BadMemory, "block not page aligned"); }
  {  Setup s; FakeRegisters r; s.memory.rings_gpu_address += 256; untouched(s, r, AmdVcn1::Status::BadMemory, "rings not page aligned"); }
  {  Setup s; FakeRegisters r; s.memory.block_gpu_address = UINT64_MAX - 4095; untouched(s, r, AmdVcn1::Status::BadMemory, "block address range wraps"); }
  {  Setup s; FakeRegisters r; s.memory.rings_gpu_address = s.memory.block_gpu_address + 8192; untouched(s, r, AmdVcn1::Status::BadMemory, "rings inside the block"); }
  {  Setup s; FakeRegisters r; s.memory.rings_gpu_address = s.memory.block_gpu_address - 4096; untouched(s, r, AmdVcn1::Status::BadMemory, "rings running into the block"); }
  {  // A firmware for another hardware block is refused.
     Setup s; FakeRegisters r;
     std::vector<uint8_t> image = read_host_file("firmware/raven_vcn.bin");
     image[12] = 2;   // ip_version_major
     CHECK(s.firmware.adopt(ConstByteView(image.data(), image.size())) == AmdFirmwareStatus::Ok);
     untouched(s, r, AmdVcn1::Status::BadFirmware, "firmware for another IP version");
  }
}

static void test_mmio_adapter() {
  // The adapter numbers registers by dword and reaches the byte offset four times that, inside its window.
  std::vector<uint32_t> bar(0x100000 / 4, 0);
  MmioVcnRegisters regs(MmioRegion(reinterpret_cast<uint64_t>(bar.data()), bar.size() * 4));
  regs.write(REG(mmUVD_STATUS), 0x1234);
  CHECK(bar[REG(mmUVD_STATUS)] == 0x1234 && regs.read(REG(mmUVD_STATUS)) == 0x1234 && REG(mmUVD_STATUS) == 0x7E00 + 0x5AF);
  CHECK(bar[0x7E00 + 0x5AF] == 0x1234 && bar[0x7E00 + 0x5AE] == 0);
}

int main() {
  test_sizes();
  test_mmio_adapter();
  test_start(false, false);
  test_start(true, false);
  test_start(false, true);
  test_start(true, true);
  test_timeout();
  test_stop();
  test_bad_input();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("AmdVcn1Test passed\n");
  return 0;
}
