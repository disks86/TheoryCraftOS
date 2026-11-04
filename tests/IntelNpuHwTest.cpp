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

// Host test for the Intel NPU hardware layer: a pretend NPU answers the register sequences of the Linux ivpu driver.
#include <stdio.h>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuFirmware.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuHw.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// A BAR whose registers are plain values plus a hook that reacts to writes the way the device does.
struct SimBar final : INpuRegisters {
  std::map<uint64_t, uint32_t> regs;
  std::map<uint64_t, uint64_t> writes64;
  std::function<void(SimBar&, uint64_t, uint32_t)> on_write;
  bool chaos = false;
  uint64_t rng = 0x9E3779B97F4A7C15ull;
  uint32_t read32(uint64_t offset) override {
    if (chaos) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return static_cast<uint32_t>(rng); }
    auto it = regs.find(offset);
    return it == regs.end() ? 0 : it->second;
  }
  void write32(uint64_t offset, uint32_t value) override {
    regs[offset] = value;
    if (on_write) on_write(*this, offset, value);
  }
  void write64(uint64_t offset, uint64_t value) override {
    writes64[offset] = value;
    regs[offset] = static_cast<uint32_t>(value);
    regs[offset + 4] = static_cast<uint32_t>(value >> 32);
  }
};

struct SimNpu {
  SimBar ip, btrs;
  IntelNpuPlatform platform;
  bool wp_stuck = false, island_stuck = false, host_axi_refused = false, top_noc_denied = false, no_clock_ack = false, cpu_noc_refused = false;
  explicit SimNpu(uint16_t device_id) : platform(intel_npu_platform(device_id)) {
    const bool mtl = platform.buttress == IntelNpuButtress::Mtl;
    // Fuses: min ratio 8, power-neutral ratio 0x10, max ratio 0x30; both tiles present.
    btrs.regs[mtl ? 0x24 : 0x174] = 0x1008;
    btrs.regs[mtl ? 0x28 : 0x178] = 0x30;
    if (!mtl) { btrs.regs[0x150] = 0x1; btrs.regs[0x154] = no_clock_ack ? 0 : (1u << 6); }
    ip.regs[0x108] = 0x00010203;
    ip.regs[0x10C] = 0x00000042;
    btrs.on_write = [this, mtl](SimBar& b, uint64_t off, uint32_t v) {
      const uint64_t cmd = mtl ? 0x14 : 0x13C;
      if (off == cmd && (v & 1)) {
        if (wp_stuck) return;
        b.regs[cmd] = v & ~1u;
        const uint32_t p1 = b.regs[mtl ? 0x0C : 0x134];
        const bool enable = (p1 & 0xFFFF) != 0 && (mtl || (b.regs[0x138] >> 16) != 0);
        const uint64_t status = mtl ? 0x44 : 0x154;
        b.regs[status] = (b.regs[status] & ~1u) | (enable ? 1u : 0u);
        if (mtl) b.regs[0x40] = enable ? 2u : 0u;
        else b.regs[0x14C] = b.regs[0x138] >> 16;
      }
      if (off == (mtl ? 0x60u : 0x164u)) b.regs[off] = v & ~1u;  // the D0i3 transition completes at once
    };
    ip.on_write = [this](SimBar& b, uint64_t off, uint32_t v) {
      if (off == 0x154 && !host_axi_refused) b.regs[0x158] = v;       // host NoC QREQN -> QACCEPTN
      if (off == 0x160 && !top_noc_denied) b.regs[0x164] = v;          // top NoC
      if (off == 0x160 && top_noc_denied) b.regs[0x168] = v;           // ... or QDENY
      if (off == 0x30024 && !island_stuck) b.regs[0x3002C] = v;        // power island enable -> status
      if (off == 0x01010030 && !cpu_noc_refused) b.regs[0x01010034] = v;
    };
  }
  IntelNpuHw hw() { return IntelNpuHw(ip, btrs, platform); }
};

static void test_platform() {
  CHECK(intel_npu_platform(0x7D1D).ip == IntelNpuIp::Ip37xx && intel_npu_platform(0x7D1D).buttress == IntelNpuButtress::Mtl);
  CHECK(intel_npu_platform(0xAD1D).ip == IntelNpuIp::Ip37xx);
  CHECK(intel_npu_platform(0x643E).ip == IntelNpuIp::Ip40xx && intel_npu_platform(0x643E).buttress == IntelNpuButtress::Lnl);
  CHECK(intel_npu_platform(0xB03E).ip == IntelNpuIp::Ip50xx && intel_npu_platform(0xFD3E).ip == IntelNpuIp::Ip50xx);
  CHECK(intel_npu_platform(0xD71D).ip == IntelNpuIp::Ip60xx);
  uint32_t known = 0;
  for (uint32_t id = 0; id <= 0xFFFF; ++id) known += intel_npu_platform(static_cast<uint16_t>(id)).valid();
  CHECK(known == 6);
  CHECK(IntelNpuHw::ip_window(IntelNpuIp::Ip37xx) > 0x06010040 && IntelNpuHw::ip_window(IntelNpuIp::Ip40xx) > 0x01010038 && IntelNpuHw::ip_window(IntelNpuIp::Unknown) == 0);
  CHECK(IntelNpuHw::ratio_to_mhz(IntelNpuButtress::Mtl, 0x30) == 1600 && IntelNpuHw::ratio_to_mhz(IntelNpuButtress::Lnl, 0x30) == 1200);
}

static void test_info() {
  SimNpu mtl(0x7D1D);
  IntelNpuHw h = mtl.hw();
  const IntelNpuInfo& i = h.read_info();
  CHECK(i.valid && i.min_ratio == 8 && i.pn_ratio == 0x10 && i.max_ratio == 0x30 && i.max_mhz == 1600 && i.hw_version == 0x00010203 && i.sw_version == 0);
  SimNpu lnl(0x643E);
  IntelNpuHw hl = lnl.hw();
  const IntelNpuInfo& l = hl.read_info();
  CHECK(l.valid && l.max_mhz == 1200 && l.pn_mhz == 400 && l.sw_version == 0x42 && l.tile_disable_mask == 0 && !l.ready);
  lnl.btrs.regs[0x150] = 0x1 | (0x2u << 1);  // one tile fused off
  CHECK(hl.read_info().valid && hl.info().tile_disable_mask == 0x2);
  lnl.btrs.regs[0x150] = 0;  // fuses not valid
  CHECK(!hl.read_info().valid);
  lnl.btrs.regs[0x150] = 1;
  lnl.btrs.regs[0x178] = 4;  // max below min
  CHECK(!hl.read_info().valid);
  lnl.btrs.regs[0x178] = 0xFFFFFFFF;  // device off the bus
  CHECK(!hl.read_info().valid);
  lnl.btrs.regs[0x178] = 0x30;
  lnl.btrs.regs[0x174] = 0x5008;  // pn above max is clamped
  CHECK(hl.read_info().valid && hl.info().pn_ratio == 0x30);
  IntelNpuPlatform none;
  SimBar a, b;
  IntelNpuHw nh(a, b, none);
  CHECK(!nh.read_info().valid && !nh.power_up().ok && !nh.boot(0x1000).ok);
}

static void test_power_up() {
  SimNpu mtl(0x7D1D);
  IntelNpuHw h = mtl.hw();
  CHECK(!h.power_up().ok && h.power_up().failed == IntelNpuStep::Info);  // fuses first
  h.read_info();
  IntelNpuResult r = h.power_up();
  CHECK(r.ok && !r.d0i3_warning);
  CHECK(mtl.btrs.regs[0x08] == ((0x30u << 16) | 8u));             // min/max ratio
  CHECK(mtl.btrs.regs[0x0C] == ((0x80u << 16) | 0x10u));          // EPP and target (the power-neutral ratio)
  CHECK((mtl.btrs.regs[0x10] & 0xFFFF) == 0x202);                 // two tiles, 4/3 PLL
  CHECK((mtl.ip.regs[0x84] & 0xC02) == 0xC02 && (mtl.ip.regs[0x94] & 0xC02) == 0xC02);  // MTL: CLK_SET and RST_SET
  CHECK(mtl.ip.regs[0x98] == 0xC02 && (mtl.ip.regs[0x30204] & 1));  // reset released, DPU active
  CHECK((mtl.ip.regs[0x30024] & 8) && (mtl.ip.regs[0x30028] & 8) && !(mtl.ip.regs[0x30020] & 8) && !(mtl.ip.regs[0x30200] & 1));
  CHECK((mtl.ip.regs[0x160] & 3) == 3 && (mtl.ip.regs[0x154] & 1));
  CHECK(mtl.btrs.regs.count(0x54) == 0);                          // no port weights on Meteor Lake

  SimNpu lnl(0x643E);
  IntelNpuHw hl = lnl.hw();
  hl.read_info();
  CHECK(hl.power_up().ok);
  CHECK((lnl.ip.regs[0x80] & 0xC02) == 0xC02 && (lnl.ip.regs[0x90] & 0xC02) == 0xC02 && lnl.ip.regs.count(0x84) == 0);  // 40xx: CLK_EN and RST_EN
  CHECK((lnl.ip.regs[0x160] & 0x5) == 0x5);                                                                          // CPU_CTRL and the 40xx L2 bit
  CHECK(lnl.btrs.regs[0x54] == 0xF711F711u && lnl.btrs.regs[0x24] == 0xF711u);
  CHECK((lnl.btrs.regs[0x138] >> 16) == 0x80 && (lnl.btrs.regs[0x134] >> 16) == 0x80);                                // CDYN and EPP
  SimNpu ptl(0xB03E);
  IntelNpuHw hp = ptl.hw();
  hp.read_info();
  CHECK(hp.power_up().ok && ptl.ip.regs[0x30068] == 0 && ptl.ip.regs[0x3006C] == 3);
  SimNpu nvl(0xD71D);
  IntelNpuHw hn = nvl.hw();
  hn.read_info();
  CHECK(hn.power_up().ok && nvl.ip.regs[0x30068] == (17u | (17u << 16)));
}

static IntelNpuStep fail_step(uint16_t id, void (*sabotage)(SimNpu&)) {
  SimNpu n(id);
  sabotage(n);
  IntelNpuHw h = n.hw();
  h.read_info();
  return h.power_up().failed;
}

static void test_failures() {
  CHECK(fail_step(0x7D1D, [](SimNpu& n) { n.wp_stuck = true; }) == IntelNpuStep::WorkPoint);
  CHECK(fail_step(0x7D1D, [](SimNpu& n) { n.ip.regs[0x154] = 1; }) == IntelNpuStep::HostSs);   // the NoC queue is already requested
  CHECK(fail_step(0x7D1D, [](SimNpu& n) { n.ip.regs[0x98] = 1; }) == IntelNpuStep::HostSs);    // AON never leaves reset
  CHECK(fail_step(0x643E, [](SimNpu& n) { n.btrs.regs[0x154] &= ~(1u << 6); }) == IntelNpuStep::ClockAck);  CHECK(std::string(intel_npu_step_name(IntelNpuStep::TopNoc)) == "top NoC");
}

static void test_boot() {
  SimNpu mtl(0x7D1D);
  IntelNpuHw h = mtl.hw();
  h.read_info();
  CHECK(h.boot(0x80000000ull).ok);
  CHECK(mtl.ip.regs[0x41040] == ((0x80000000u >> 9) | 1u));
  CHECK((mtl.ip.regs[0x06010040] & 1) && !(mtl.ip.regs[0x06010040] & 2));  // run vector set, resume pulsed and released
  CHECK(!h.boot(0xFFFFFFFFFFull << 4).ok);
  SimNpu lnl(0x643E);
  IntelNpuHw hl = lnl.hw();
  hl.read_info();
  CHECK(hl.boot(0x1234000ull).ok);
  CHECK(lnl.ip.writes64[0x40040] == (0x1234000ull << 3));
  CHECK(lnl.ip.regs[0x40040] == (static_cast<uint32_t>(0x1234000ull << 3) | 1u));
  CHECK((lnl.ip.regs[0x01010030] & 1) != 0);
  CHECK(!hl.boot(~0ull).ok);
  SimNpu refused(0xB03E);
  refused.cpu_noc_refused = true;
  IntelNpuHw hr = refused.hw();
  hr.read_info();
  CHECK(!hr.boot(0x1000).ok && hr.boot(0x1000).failed == IntelNpuStep::CpuNoc && refused.ip.writes64.empty());
  SimNpu nvl(0xD71D);
  nvl.cpu_noc_refused = true;  // Nova Lake never asks the CPU NoC
  IntelNpuHw hn = nvl.hw();
  hn.read_info();
  CHECK(hn.boot(0x1000).ok && nvl.ip.regs.count(0x01010030) == 0);
}


// ---- Firmware image, boot parameters and the load-and-boot sequence ----

using Bytes = std::vector<uint8_t>;
static void st32(Bytes& b, size_t off, uint32_t v) { for (int i = 0; i < 4; ++i) b[off + i] = static_cast<uint8_t>(v >> (8 * i)); }
static void st64(Bytes& b, size_t off, uint64_t v) { for (int i = 0; i < 8; ++i) b[off + i] = static_cast<uint8_t>(v >> (8 * i)); }
static uint32_t ld32(ConstByteView b, size_t off) { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t{b[off + i]} << (8 * i); return v; }
static uint64_t ld64(ConstByteView b, size_t off) { return ld32(b, off) | (uint64_t{ld32(b, off + 4)} << 32); }

// A firmware file for `ip` the way Intel's make_std_fw_image lays it out: 4 KiB header, 4 KiB version page, then the image.
static Bytes make_firmware(IntelNpuIp ip) {
  const uint64_t base = IntelNpuMemoryMap::for_ip(ip).runtime.start;
  const uint32_t image_size = 0x4000;
  Bytes f(IntelNpuFirmware::kFileImageOffset + image_size);
  st32(f, 0, 1);                      // header_version
  st32(f, 4, 1);                      // image_format
  st64(f, 8, base + 0x2000);          // image_load_address
  st32(f, 16, image_size);
  st64(f, 20, base + 0x2000);         // entry_point
  st64(f, 64, base + 0x1000);         // firmware_version_load_address
  st32(f, 72, 32);                    // firmware_version_size
  st64(f, 76, base);                  // boot_params_load_address
  st32(f, 84 + 4 * 0, (3u << 16) | 29);   // boot API
  st32(f, 84 + 4 * 4, (3u << 16) | 34);   // JSM API
  st32(f, 148, 0x2000 + 0x10000);     // runtime_size
  st32(f, 152, 0x1800);               // shave_nn_fw_size
  st32(f, 156, 0x3000); st32(f, 160, 0x5000);  // preemption sizes
  st64(f, 188, base + 0x2000); st32(f, 196, 0x1000);  // read-only section
  const char* v = "FW 2026.1 test";
  for (size_t i = 0; v[i]; ++i) f[4096 + i] = static_cast<uint8_t>(v[i]);
  for (size_t i = 0; i < image_size; ++i) f[IntelNpuFirmware::kFileImageOffset + i] = static_cast<uint8_t>(i * 7 + 1);
  return f;
}
static IntelNpuFirmware parse(const Bytes& f, IntelNpuIp ip, uint32_t sched = kIntelNpuSchedAuto) { return IntelNpuFirmware::parse(ConstByteView(f.data(), f.size()), ip, sched); }

static void test_firmware_parse() {
  using E = IntelNpuFirmwareError;
  const IntelNpuIp ips[] = {IntelNpuIp::Ip37xx, IntelNpuIp::Ip40xx, IntelNpuIp::Ip50xx, IntelNpuIp::Ip60xx};
  for (IntelNpuIp ip : ips) {
    const uint64_t base = IntelNpuMemoryMap::for_ip(ip).runtime.start;
    const Bytes f = make_firmware(ip);
    const IntelNpuFirmware fw = parse(f, ip);
    CHECK(fw.valid() && fw.runtime_addr == base + 0x2000 && fw.runtime_size == 0x10000 && fw.image_size == 0x4000 && fw.image.size() == 0x4000);
    CHECK(fw.boot_params_addr == base && fw.fw_version_addr == base + 0x1000 && fw.fw_version_size == 4096 && fw.cold_boot_entry_point == base + 0x2000);
    CHECK(fw.shave_nn_size == 0x2000 && fw.image[1] == 8 && fw.version.size() == 14 && fw.version[0] == 'F');
    CHECK(fw.sched_mode == kIntelNpuSchedHw && fw.primary_preempt_buf_size == 0x3000 && fw.secondary_preempt_buf_size == 0x5000);
    CHECK(fw.read_only_addr == base + 0x2000 && fw.read_only_size == 0x1000);
    CHECK(IntelNpuFirmware::api_major(fw.api_version[4]) == 3 && IntelNpuFirmware::api_minor(fw.api_version[4]) == 34);
  }
  const IntelNpuIp ip = IntelNpuIp::Ip40xx;
  const uint64_t base = IntelNpuMemoryMap::for_ip(ip).runtime.start;
  auto mutated = [&](auto fn) { Bytes f = make_firmware(ip); fn(f); return parse(f, ip).error; };
  CHECK(mutated([](Bytes& f) { st32(f, 0, 2); }) == E::HeaderVersion);
  CHECK(mutated([&](Bytes& f) { st64(f, 76, base - 0x1000); }) == E::BootParamsAddress);
  CHECK(mutated([&](Bytes& f) { st64(f, 76, ~0ull - 100); }) == E::BootParamsAddress);   // wraps
  CHECK(mutated([](Bytes& f) { st32(f, 72, 5000); }) == E::VersionSize);
  CHECK(mutated([&](Bytes& f) { st64(f, 64, base + (64u << 20)); }) == E::VersionAddress);
  CHECK(mutated([](Bytes& f) { st32(f, 148, 0x1000); }) == E::RuntimeSize);              // smaller than the two pages: ivpu would wrap
  CHECK(mutated([](Bytes& f) { st32(f, 148, 0x2000 + 0x10001); }) == E::RuntimeSize);    // not page aligned
  CHECK(mutated([&](Bytes& f) { st64(f, 8, base + 0x2001); }) == E::RuntimeSize);
  CHECK(mutated([](Bytes& f) { st32(f, 16, 0x4001); }) == E::ImageSize);                 // beyond the file
  CHECK(mutated([](Bytes& f) { st32(f, 16, 0xFFFFFFF0u); }) == E::ImageSize);
  CHECK(mutated([](Bytes& f) { st32(f, 148, 0x2000 + 0x3000); }) == E::ImageSize);       // runtime smaller than the image
  CHECK(mutated([&](Bytes& f) { st64(f, 20, base + 0x2000 + 0x3001); }) == E::EntryPoint);  // less than a page left in the image
  CHECK(mutated([&](Bytes& f) { st64(f, 20, base); }) == E::EntryPoint);
  CHECK(mutated([](Bytes& f) { st32(f, 152, (2u << 20) + 1); }) == E::ShaveSize);
  CHECK(mutated([&](Bytes& f) { st64(f, 188, base + 0x5000); st32(f, 196, 0x2000); }) == E::ReadOnlySection);
  CHECK(mutated([&](Bytes& f) { st64(f, 188, 0); st32(f, 196, 0xFFFFFFFFu); }) == E::None);  // no section: its size is ignored
  CHECK(mutated([](Bytes& f) { st32(f, 84, (2u << 16) | 99); }) == E::BootApiVersion);
  CHECK(mutated([](Bytes& f) { st32(f, 84 + 16, 1u << 16); }) == E::JsmApiVersion);
  CHECK(std::string(intel_npu_firmware_error_name(E::EntryPoint)) == "entry point outside the image");
  // 37xx has a different runtime range: a 40xx image is not valid there.
  CHECK(parse(make_firmware(IntelNpuIp::Ip40xx), IntelNpuIp::Ip37xx).error == E::BootParamsAddress);

  // Scheduler selection.
  { Bytes f = make_firmware(ip); st32(f, 84 + 16, (3u << 16) | 23); const IntelNpuFirmware o = parse(f, ip);
    CHECK(o.valid() && o.sched_mode == kIntelNpuSchedOs && o.primary_preempt_buf_size == 0);          // OS scheduling has no preemption buffers
    CHECK(parse(f, ip, kIntelNpuSchedHw).sched_mode == kIntelNpuSchedHw);
    CHECK(parse(f, IntelNpuIp::Ip40xx, kIntelNpuSchedOs).sched_mode == kIntelNpuSchedOs); }
  { Bytes f = make_firmware(IntelNpuIp::Ip60xx); st32(f, 84 + 16, (3u << 16) | 23);
    CHECK(parse(f, IntelNpuIp::Ip60xx, kIntelNpuSchedOs).sched_mode == kIntelNpuSchedHw); }               // 60xx has no OS scheduler
  { Bytes f = make_firmware(ip); st32(f, 156, 100); st32(f, 164, 0); CHECK(parse(f, ip).primary_preempt_buf_size == 0);   // too small
    st32(f, 156, 0x1001); st32(f, 164, 0); CHECK(parse(f, ip).primary_preempt_buf_size == 0x2000);                        // rounded up
    st32(f, 164, 64u << 20); CHECK(parse(f, ip).primary_preempt_buf_size == 0); }                                         // max too big
  { Bytes f = make_firmware(ip); for (size_t i = 0; i < 4096; ++i) f[4096 + i] = 'x'; const IntelNpuFirmware o = parse(f, ip);
    CHECK(o.valid() && o.version.size() == 255); }                                                       // an unterminated version is cut, not overrun
}

// Every prefix of a good file and every single-bit change of its header either parses into something inside the file or is refused.
static void test_firmware_corruption() {
  const IntelNpuIp ip = IntelNpuIp::Ip40xx;
  const Bytes f = make_firmware(ip);
  for (size_t n = 0; n < f.size(); ++n) {
    const IntelNpuFirmware fw = IntelNpuFirmware::parse(ConstByteView(f.data(), n), ip);
    CHECK(!fw.valid() && fw.error == (n <= IntelNpuFirmware::kFileImageOffset ? IntelNpuFirmwareError::TooSmall : IntelNpuFirmwareError::ImageSize));
  }
  uint32_t accepted = 0;
  for (size_t byte = 0; byte < 204; ++byte)
    for (int bit = 0; bit < 8; ++bit) {
      Bytes g = f;
      g[byte] ^= static_cast<uint8_t>(1u << bit);
      const IntelNpuFirmware fw = parse(g, ip);
      if (!fw.valid()) continue;
      ++accepted;
      CHECK(fw.image.data() >= g.data() && fw.image.data() + fw.image.size() <= g.data() + g.size());
      CHECK(fw.version.data() >= g.data() && fw.version.data() + fw.version.size() <= g.data() + g.size());
      const IntelNpuMemoryMap m = IntelNpuMemoryMap::for_ip(ip);
      CHECK(m.runtime.contains(fw.runtime_addr, fw.runtime_size) && fw.runtime_size >= fw.image_size);
      CHECK(fw.cold_boot_entry_point >= fw.runtime_addr && fw.cold_boot_entry_point + 4096 <= fw.runtime_addr + fw.image_size);
    }
  CHECK(accepted > 0);  // flips of ignored fields (format, version string, reserved) must not be refused
}

static IntelNpuBootInputs boot_inputs(const IntelNpuMemoryMap& m) {
  IntelNpuBootInputs in;
  in.pci_bus = 0; in.si_stepping = 4; in.device_id = 0x643E; in.sku = 0x55; in.tile_fuse = 0;
  in.min_ratio = 8; in.pn_ratio = 0x10; in.max_ratio = 0x30;
  in.ipc_rx_addr = m.global.start; in.ipc_rx_size = 0x2000;
  in.crit_log_addr = m.global.start + 0x10000; in.crit_log_size = 0x1000;
  in.verbose_log_addr = m.global.start + 0x20000; in.verbose_log_size = 0x8000;
  in.shave_nn_addr = m.shave.start; in.trace_level = 2; in.telemetry_base = 0x1234; in.telemetry_size = 0x80; in.telemetry_enable = true;
  in.uses_ecc_mca_signal = true; in.disable_d0i2 = true; in.system_time_us = 1789000000123456ull;
  return in;
}

static void test_boot_params() {
  using O = IntelNpuBoot;
  const IntelNpuIp ip = IntelNpuIp::Ip40xx;
  const IntelNpuMemoryMap m = IntelNpuMemoryMap::for_ip(ip);
  const IntelNpuFirmware fw = parse(make_firmware(ip), ip);
  Bytes page(4096, 0xAA);
  ByteView pv(page.data(), page.size());
  const IntelNpuBootInputs in = boot_inputs(m);
  CHECK(O::build_cold(pv, fw, ip, m, in) == IntelNpuBootParamsError::None);
  ConstByteView p(page.data(), page.size());
  CHECK(ld32(p, O::kMagic) == 0x10000 && ld32(p, O::kBootType) == 0 && ld32(p, O::kVpuId) == 0 && ld32(p, O::kSiStepping) == 4);
  CHECK(ld64(p, O::kSharedBase) == 0x90000000ull && ld32(p, O::kSharedSize) == 256u << 20);
  CHECK(ld64(p, O::kIpcHeaderStart) == m.global.start && ld32(p, O::kIpcHeaderSize) == 0x1000);
  CHECK(ld64(p, O::kIpcPayloadStart) == m.global.start + 0x1000 && ld32(p, O::kIpcPayloadSize) == 0x1000);
  CHECK(ld64(p, O::kPioBase) == 0 && ld32(p, O::kPioSize) == 0);                       // only 37xx has the aliased PIO window
  CHECK(ld32(p, O::kAutoconfig) == 1 && p[O::kCacheDefaults + 2] == 1 && p[O::kCacheDefaults + 3] == 1);   // 0x80000000 >> 31
  CHECK(ld64(p, O::kShaveNnBase) == m.shave.start && ld32(p, O::kWatchdogMss) == 32 && ld32(p, O::kWatchdogNce) == 33);
  CHECK(ld64(p, O::kDeviceId) == 0x643E && ld64(p, O::kSku) == 0x55 && ld64(p, O::kFeatureExclusion) == 0);
  CHECK(ld32(p, O::kMinRatio) == 8 && ld32(p, O::kPnRatio) == 0x10 && ld32(p, O::kMaxRatio) == 0x30);
  CHECK(ld32(p, O::kTraceLevel) == 2 && ld32(p, O::kTraceDestination) == 2 && ld64(p, O::kTraceFormats) == 1 && ld64(p, O::kTraceHwComponents) == ~0ull);
  CHECK(ld64(p, O::kCritBufAddr) == in.crit_log_addr && ld32(p, O::kCritBufSize) == 0x1000 && ld64(p, O::kVerboseBufAddr) == in.verbose_log_addr && ld32(p, O::kVerboseBufSize) == 0x8000);
  CHECK(ld64(p, O::kTelemetryBase) == 0x1234 && ld64(p, O::kTelemetrySize) == 0x80 && ld32(p, O::kTelemetryEnable) == 1);
  CHECK(ld32(p, O::kSchedulingMode) == 1 && ld32(p, O::kFocusPresentTimer) == 1000 && ld32(p, O::kD0i3DelayedEntry) == 1);
  CHECK(ld32(p, O::kPowerProfile) == 2 && ld32(p, O::kEccMcaSignal) == 3 && ld64(p, O::kSystemTime) == in.system_time_us);
  uint32_t nonzero = 0;
  for (size_t i = 0; i < page.size(); ++i) nonzero += page[i] != 0;
  CHECK(nonzero < 120);  // the 0xAA fill is gone: the whole page was written
  // 37xx: aliased PIO window is the user range; OS scheduler leaves the focus timer at zero.
  { const IntelNpuIp i37 = IntelNpuIp::Ip37xx; const IntelNpuMemoryMap m37 = IntelNpuMemoryMap::for_ip(i37);
    Bytes f37 = make_firmware(i37); st32(f37, 84 + 16, (3u << 16) | 10);
    const IntelNpuFirmware w = parse(f37, i37);
    Bytes pg(4096); ByteView v(pg.data(), pg.size()); IntelNpuBootInputs in37 = boot_inputs(m37);
    CHECK(O::build_cold(v, w, i37, m37, in37) == IntelNpuBootParamsError::None);
    ConstByteView c(pg.data(), pg.size());
    CHECK(ld64(c, O::kPioBase) == 0xa0000000ull && ld32(c, O::kPioSize) == 511u << 20 && ld32(c, O::kSchedulingMode) == 0 && ld32(c, O::kFocusPresentTimer) == 0);
    CHECK(c[O::kCacheDefaults + 3] == 0x180000000ull >> 31); }
  // No SHAVE NN section: the base stays zero.
  { Bytes f = make_firmware(ip); st32(f, 152, 0); const IntelNpuFirmware w = parse(f, ip); Bytes pg(4096); ByteView v(pg.data(), pg.size());
    IntelNpuBootInputs i2 = in; i2.shave_nn_addr = 0;
    CHECK(O::build_cold(v, w, ip, m, i2) == IntelNpuBootParamsError::None && ld64(ConstByteView(pg.data(), pg.size()), O::kShaveNnBase) == 0); }
  // Refusals.
  using E = IntelNpuBootParamsError;
  Bytes small(4095); ByteView sv(small.data(), small.size());
  CHECK(O::build_cold(sv, fw, ip, m, in) == E::PageSize && O::update_warm(sv, 1, 2, 3) == E::PageSize);
  { IntelNpuBootInputs b = in; b.ipc_rx_size = 0; CHECK(O::build_cold(pv, fw, ip, m, b) == E::IpcBuffer);
    b = in; b.ipc_rx_size = 0x2001; CHECK(O::build_cold(pv, fw, ip, m, b) == E::IpcBuffer);
    b = in; b.ipc_rx_addr = m.global.end - 0x1000; CHECK(O::build_cold(pv, fw, ip, m, b) == E::IpcBuffer);
    b = in; b.ipc_rx_addr = ~0ull - 0x10; CHECK(O::build_cold(pv, fw, ip, m, b) == E::IpcBuffer);
    b = in; b.crit_log_addr = 0x1000; CHECK(O::build_cold(pv, fw, ip, m, b) == E::LogBuffer);
    b = in; b.verbose_log_size = 0; CHECK(O::build_cold(pv, fw, ip, m, b) == E::LogBuffer);
    b = in; b.shave_nn_addr = m.shave.end - 0x1000; CHECK(O::build_cold(pv, fw, ip, m, b) == E::ShaveAddress); }
  // Warm boot changes only the varying fields.
  O::build_cold(pv, fw, ip, m, in);
  Bytes before = page;
  CHECK(O::update_warm(pv, 1000, 2000, 3000) == E::None);
  CHECK(ld32(p, O::kBootType) == 1 && ld64(p, O::kD0i3Residency) == 1000 && ld64(p, O::kD0i3EntryTs) == 2000 && ld64(p, O::kSystemTime) == 3000 && ld64(p, O::kSaveRestoreRet) == 0);
  uint32_t changed = 0;
  for (size_t i = 0; i < page.size(); ++i) changed += page[i] != before[i];
  CHECK(changed > 0 && changed < 8 + 8 + 8 + 4 + 1);
}

static void test_load_and_start() {
  const IntelNpuIp ip = IntelNpuIp::Ip40xx;
  const IntelNpuMemoryMap m = IntelNpuMemoryMap::for_ip(ip);
  const Bytes file = make_firmware(ip);
  const IntelNpuFirmware fw = parse(file, ip);
  Bytes rt(fw.runtime_size, 0xAA);
  ByteView rv(rt.data(), rt.size());
  CHECK(IntelNpuBoot::load_image(rv, fw));
  CHECK(fw.image_load_offset == 0 && rt[fw.image_load_offset] == file[IntelNpuFirmware::kFileImageOffset] && rt[fw.image_load_offset + 0x3FFF] == file[IntelNpuFirmware::kFileImageOffset + 0x3FFF]);
  CHECK(rt.back() == 0 && rt[0x4000] == 0);  // the memory after the image is cleared too
  Bytes wrong(fw.runtime_size - 1);
  CHECK(!IntelNpuBoot::load_image(ByteView(wrong.data(), wrong.size()), fw) && !IntelNpuBoot::load_image(rv, IntelNpuFirmware()));

  auto run = [&](SimNpu& n, const IntelNpuBootInputs& in, Bytes& page, Bytes& runtime) {
    IntelNpuHw h = n.hw();
    h.read_info();
    IntelNpuBootMemory mem{ByteView(page.data(), page.size()), ByteView(runtime.data(), runtime.size())};
    return intel_npu_start(h, fw, ip, m, in, mem);
  };
  { SimNpu n(0x643E); Bytes page(4096), runtime(fw.runtime_size);
    n.btrs.regs[0x150] = 0x1 | (0x2u << 1);  // a tile fused off: it must reach the firmware through the page, not through the caller's inputs
    IntelNpuBootInputs in = boot_inputs(m); in.min_ratio = in.max_ratio = in.pn_ratio = 1; in.tile_fuse = 0;
    const IntelNpuStartResult r = run(n, in, page, runtime);
    CHECK(r.ok && r.stage == IntelNpuStartStage::Done);
    ConstByteView p(page.data(), page.size());
    CHECK(ld32(p, IntelNpuBoot::kMinRatio) == 8 && ld32(p, IntelNpuBoot::kMaxRatio) == 0x30 && ld32(p, IntelNpuBoot::kPnRatio) == 0x10 && ld64(p, IntelNpuBoot::kFeatureExclusion) == 2);
    CHECK(runtime[fw.image_load_offset] == file[IntelNpuFirmware::kFileImageOffset] && runtime[fw.image_load_offset + 5] == file[IntelNpuFirmware::kFileImageOffset + 5]);
    CHECK(n.ip.writes64[0x40040] == (fw.cold_boot_entry_point << 3)); }          // the CPU was started at the firmware's entry point
  { SimNpu n(0x643E); Bytes page(4096), runtime(fw.runtime_size); n.island_stuck = true;
    const IntelNpuStartResult r = run(n, boot_inputs(m), page, runtime);
    CHECK(!r.ok && r.stage == IntelNpuStartStage::PowerUp && r.hw_step == IntelNpuStep::PowerDomain && n.ip.writes64.empty()); }   // no CPU start after a failed power-up
  { SimNpu n(0x643E); Bytes page(4096), runtime(fw.runtime_size); n.cpu_noc_refused = true;
    const IntelNpuStartResult r = run(n, boot_inputs(m), page, runtime);
    CHECK(!r.ok && r.stage == IntelNpuStartStage::CpuBoot && r.hw_step == IntelNpuStep::CpuNoc); }
  { SimNpu n(0x643E); Bytes page(4096), runtime(fw.runtime_size); IntelNpuBootInputs in = boot_inputs(m); in.ipc_rx_size = 3;
    const IntelNpuStartResult r = run(n, in, page, runtime);
    CHECK(!r.ok && r.stage == IntelNpuStartStage::BootParams && r.params_error == IntelNpuBootParamsError::IpcBuffer && n.btrs.regs.count(0x13C) == 0 ); }  // nothing powered
  { SimNpu n(0x643E); Bytes page(4096), runtime(fw.runtime_size - 4096);
    const IntelNpuStartResult r = run(n, boot_inputs(m), page, runtime);
    CHECK(!r.ok && r.stage == IntelNpuStartStage::Memory); }
  { SimNpu n(0x643E); Bytes page(100), runtime(fw.runtime_size);
    CHECK(!run(n, boot_inputs(m), page, runtime).ok); }
}

static void test_power_down() {
  for (uint16_t id : {0x7D1D, 0x643E, 0xB03E}) {
    SimNpu n(id);
    const bool mtl = n.platform.buttress == IntelNpuButtress::Mtl;
    const uint64_t reset = mtl ? 0x50 : 0x160, d0i3 = mtl ? 0x60 : 0x164, status = mtl ? 0x44 : 0x154;
    uint32_t resets = 0;
    auto base = n.btrs.on_write;
    n.btrs.on_write = [&, base](SimBar& b, uint64_t off, uint32_t v) {
      if (off == reset && (v & 1)) { b.regs[off] = v & ~1u; ++resets; return; }
      base(b, off, v);
    };
    IntelNpuHw h = n.hw();
    h.read_info();
    CHECK(h.power_up().ok && h.is_idle() == ((n.btrs.regs[status] & 3) == 3));
    n.btrs.regs[status] |= 2;
    CHECK(h.is_idle() && h.wait_idle());
    IntelNpuResult r = h.power_down();
    CHECK(r.ok && resets == 1 && !(n.btrs.regs[status] & 1) && (n.btrs.regs[d0i3] & 4));   // reset, work point off (not ready), D0i3 requested
    CHECK(!h.is_idle());
    // Each step is tried even when an earlier one failed; the first failure is the one reported.
    n.btrs.regs[reset] = 1;   // a reset that never finishes
    n.btrs.on_write = nullptr;
    IntelNpuResult bad = h.power_down();
    CHECK(!bad.ok && bad.failed == IntelNpuStep::Reset);
    CHECK(std::string(intel_npu_step_name(IntelNpuStep::Reset)) == "IP reset" && std::string(intel_npu_step_name(IntelNpuStep::D0i3)) == "D0i3");
    // The performance counter, as two halves.
    n.ip.regs[id == 0x7D1D ? 0x06029000 : 0x01029008] = 0x11223344;
    n.ip.regs[(id == 0x7D1D ? 0x06029000 : 0x01029008) + 4] = 0x55;
    CHECK(h.perf_counter() == 0x5511223344ull);
    // Before the CPU starts: translation-buffer stream ids valid, page walks snoop as asked.
    h.prepare_boot(true);
    CHECK(n.ip.regs[0x00360004] == (id == 0x7D1D ? 0x33u : 0x3Fu) && (n.ip.regs[0x00360000] & 8));
    CHECK(id == 0x7D1D ? !(n.ip.regs[0x00360000] & 0x20) : (n.ip.regs[0x00360000] & 0x20));
    h.prepare_boot(false);
    CHECK(id == 0x7D1D ? (n.ip.regs[0x00360000] & 0x20) != 0 : !(n.ip.regs[0x00360000] & 0x20));
  }
  IntelNpuPlatform none;
  SimBar a, b;
  IntelNpuHw nh(a, b, none);
  CHECK(!nh.power_down().ok && !nh.is_idle() && !nh.ip_reset() && nh.perf_counter() == 0);
}

// Whatever the registers read, every sequence ends (with a result or a refusal) and never touches an offset outside the windows.
static void test_chaos() {
  const uint16_t ids[] = {0x7D1D, 0x643E, 0xB03E, 0xD71D};
  for (uint16_t id : ids)
    for (uint32_t seed = 1; seed <= 3; ++seed) {
      SimNpu n(id);
      n.ip.chaos = n.btrs.chaos = true;
      n.ip.rng = n.btrs.rng = 0x1234567ull * seed;
      IntelNpuHw h = n.hw();
      h.read_info();
      (void)h.power_up();
      (void)h.boot(0x10000ull * seed);
      for (const auto& w : n.ip.regs) CHECK(w.first < IntelNpuHw::ip_window(n.platform.ip));
      for (const auto& w : n.btrs.regs) CHECK(w.first < IntelNpuHw::kButtressWindow);
    }
}

int main() {
  test_platform();
  test_info();
  test_power_up();
  test_failures();
  test_boot();
  test_firmware_parse();
  test_firmware_corruption();
  test_boot_params();
  test_load_and_start();
  test_power_down();
  test_chaos();
  if (g_failed) { printf("IntelNpuHwTest: %d failed\n", g_failed); return 1; }
  printf("IntelNpuHwTest: ok\n");
  return 0;
}
