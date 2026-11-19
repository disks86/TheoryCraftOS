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

// Host tests for the display driver of a non-boot discrete Intel GPU (Intel/IntelSecondaryGpu.*, IntelDg2Display.*, IntelSnpsPhy.*): the SNPS PHY's
// MPLLB tables (each against the port clock formula), the drive levels, CDCLK, M/N and the watermark; then the whole start against a register model
// of a DG2 (power wells, fuses, PLL locks, DBUF slices, DDI buffers, transcoder state, GMBUS with a monitor on a pin, AUX channels with a DisplayPort
// sink each): the power well enable order, CDCLK, the MPLLB programming for one HDMI and one DP rate, the GGTT entries of the surface, the
// transcoder, pipe and plane registers for 1080p, DP link training through AUX, another mode, the refusals (no monitor, local memory not trained,
// a PLL that never locks, a link that never trains, too small a window), damaged EDIDs and a sink that answers rubbish.
//
// Built twice by tests/run_host_tests.sh: with -DTCOS_INTEL_DGPU_NATIVE (everything above) and without it (IntelSecondaryGpuOffTest: the driver must
// refuse before touching the card).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/Intel/IntelSecondaryGpu.hpp"
#include "FakeAuxSink.hpp"

namespace hardware {
uint64_t uptime_us() { return 0; }   // (the kernel clock: the tests give the display a clock of their own)
}  // namespace hardware

using namespace hardware;
using namespace hardware::intel_secondary;
namespace dg2 = hardware::intel_dg2;

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

struct Clock final : IClock {
  mutable uint64_t now = 0;
  uint64_t now_us() const override { now += 50; return now; }
  const char* name() const override { return "clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

// Register offsets typed in from i915's headers (i915_reg.h, intel_snps_phy_regs.h, skl_universal_plane_regs.h, intel_gmbus_regs.h), not taken from the code under test.
constexpr uint64_t rPwCtl2 = 0x45404, rPwAux2 = 0x45444, rPwDdi2 = 0x45454, rFuse = 0x42000, rDcState = 0x45504, rDePll = 0x46070, rCdclkCtl = 0x46000,
                   rSquash = 0x46008, rDbuf1 = 0x45008, rDbuf2 = 0x44FE8, rDbuf3 = 0x44300, rDbuf4 = 0x44304, rAbox0 = 0x45038, rAbox1 = 0x45048;
constexpr uint64_t rTransHtotal = 0x60000, rTransHblank = 0x60004, rTransHsync = 0x60008, rTransVtotal = 0x6000C, rTransVblank = 0x60010, rTransVsync = 0x60014,
                   rPipeSrc = 0x6001C, rDataM = 0x60030, rDataN = 0x60034, rLinkM = 0x60040, rLinkN = 0x60044, rFuncCtl = 0x60400, rDpTpCtl = 0x60540,
                   rDpTpStatus = 0x60544, rClkSel = 0x46140, rTransConf = 0x70008, rPipeMisc = 0x70030;
constexpr uint64_t rPlaneCtl = 0x70180, rPlaneStride = 0x70188, rPlaneSize = 0x70190, rPlaneSurf = 0x7019C, rPlaneBufCfg = 0x7027C, rPlaneWm0 = 0x70240;
constexpr uint64_t rGuCntl = 0x101010, rTileRange = 0x1083A0, rGfxFlush = 0x101008;
constexpr uint64_t rGmbus0 = 0xC5100, rGmbus1 = 0xC5104, rGmbus2 = 0xC5108, rGmbus3 = 0xC510C;
constexpr uint64_t snps(uint32_t phy, uint64_t reg) { return reg + phy * 0x1000ull; }
constexpr uint64_t rMpllbCp = 0x168000, rMpllbDiv = 0x168004, rMpllbFracn1 = 0x168008, rMpllbFracn2 = 0x16800C, rMpllbDiv2 = 0x16801C, rTxEq0 = 0x168300;
constexpr uint64_t pll_enable(uint32_t phy) { return phy < 3 ? 0x46010 + phy * 4ull : 0x4601C; }
constexpr uint64_t ddi_buf(uint32_t port) { return 0x64000 + port * 0x100ull; }
constexpr uint64_t aux_ctl(uint32_t port) { return 0x64010 + port * 0x100ull; }

// A DG2: a register file (every write logged) whose status bits follow their enables, a GMBUS controller with a monitor on one pin, and an AUX
// channel per port with a DisplayPort sink (FakeAuxSink, which speaks the AtomBIOS request layout: the model turns the wire bytes into it).
struct Card final : intel_gt::IRegisterBank {
  std::map<uint64_t, uint32_t> regs;
  std::vector<std::pair<uint64_t, uint32_t>> writes;
  bool lmem_init = true;
  bool pll_locks = true;
  bool wells_answer = true;
  FakeAuxSink* sinks[8] = {};          // by port (DDI A 0, B 1, C 2, D 7)
  uint32_t aux_unpowered = 0;          // AUX transactions tried while the port's AUX well was off
  // GMBUS
  uint32_t monitor_pin = 0;            // 0: no monitor
  std::vector<uint8_t> edid;
  uint32_t gm_segment = 0, gm_offset = 0, gm_left = 0;
  bool gm_nak = false;
  uint32_t gmbus_reads = 0;

  uint32_t get(uint64_t r) const { auto it = regs.find(r); return it == regs.end() ? 0 : it->second; }
  uint32_t read32(uint64_t r) override {
    const uint32_t v = get(r);
    if (r == rGuCntl) return lmem_init ? 0x80 : 0;
    if (r == rTileRange) return 16u << 8;        // 16 GiB
    if (r == rFuse) return 0xFFFFFFFFu;
    if (r == rPwCtl2 || r == rPwAux2 || r == rPwDdi2) return wells_answer ? v | ((v >> 1) & 0x55555555u) : v & 0xAAAAAAAAu;
    if (r == rDePll) return v & (1u << 31) ? v | (1u << 30) : v & ~(1u << 30);
    if (r == rDbuf1 || r == rDbuf2 || r == rDbuf3 || r == rDbuf4) return v & (1u << 31) ? v | (1u << 30) : v & ~(1u << 30);
    if ((r >= 0x46010 && r <= 0x4601C) || r == 0x46030) return v & (1u << 31) && pll_locks ? v | (1u << 30) : v & ~(1u << 30);
    if (r >= 0x64000 && r < 0x64800 && (r & 0xFF) == 0) return v & (1u << 31) ? v & ~(1u << 7) : v | (1u << 7);   // DDI_BUF_CTL: idle while off
    if (r == rTransConf) return v & (1u << 31) ? v | (1u << 30) : v & ~(1u << 30);
    if (r == rDpTpStatus) return ((get(rDpTpCtl) >> 8) & 7) == 2 ? 1u << 25 : 0;
    if (r == rGmbus2) return gm_nak ? (1u << 10) : (1u << 11) | (1u << 14);
    if (r == rGmbus3) return gmbus_read();
    return v;
  }
  void write32(uint64_t r, uint32_t v) override {
    writes.push_back({r, v});
    if (r >= 0x64010 && r < 0x64800 && (r & 0xFF) == 0x10) return aux(static_cast<uint32_t>((r - 0x64010) / 0x100), v);
    if (r == rGmbus1) return gmbus_command(v);
    regs[r] = v;
  }
  uint32_t written(uint64_t r) const {   // how many times
    uint32_t n = 0;
    for (auto& w : writes) n += w.first == r;
    return n;
  }
  int32_t first_write(uint64_t r, uint32_t mask = 0, uint32_t value = 0) const {
    for (size_t i = 0; i < writes.size(); ++i)
      if (writes[i].first == r && (writes[i].second & mask) == value) return static_cast<int32_t>(i);
    return -1;
  }
  int32_t last_write(uint64_t r) const {
    for (size_t i = writes.size(); i-- > 0;)
      if (writes[i].first == r) return static_cast<int32_t>(i);
    return -1;
  }

  // ---- AUX: the data registers hold the request big-endian; the reply goes back the same way
  void aux(uint32_t port, uint32_t ctl) {
    if (!(ctl & (1u << 31))) { regs[aux_ctl(port)] = 0; return; }   // the clear of done and errors
    const uint32_t size = (ctl >> 20) & 0x1F;
    uint8_t wire[20] = {};
    for (uint32_t i = 0; i < 20; ++i) wire[i] = static_cast<uint8_t>(get(aux_ctl(port) + 4 + (i / 4) * 4) >> ((3 - i % 4) * 8));
    static constexpr uint8_t kAuxWell[8] = {0, 1, 2, 3, 0, 0, 0, 7};
    const bool powered = get(rPwAux2) & (2u << (kAuxWell[port] * 2));
    if (!powered) ++aux_unpowered;
    FakeAuxSink* sink = port < 8 ? sinks[port] : nullptr;
    if (!sink || !powered || size < 3) { regs[aux_ctl(port)] = (1u << 30) | (1u << 28); return; }   // done, timed out
    uint8_t atom[20] = {};
    atom[0] = wire[2];
    atom[1] = wire[1];
    atom[2] = wire[0];
    atom[3] = static_cast<uint8_t>((size & 15) << 4 | (size == 3 ? 0 : wire[3] & 0xF));
    for (uint32_t i = 4; i < size; ++i) atom[i] = wire[i];
    const FakeAuxSink::Answer a = sink->handle(atom, 0, 0, 0);
    if (a.status != 0 && a.status < 4) { regs[aux_ctl(port)] = (1u << 30) | (1u << 28); return; }
    std::vector<uint8_t> reply{a.status};
    for (uint8_t b : a.data) reply.push_back(b);
    if (reply.size() > 20) reply.resize(20);
    for (uint32_t i = 0; i < 5; ++i) {
      uint32_t w = 0;
      for (uint32_t k = 0; k < 4; ++k) w |= uint32_t{i * 4 + k < reply.size() ? reply[i * 4 + k] : uint8_t{0}} << ((3 - k) * 8);
      regs[aux_ctl(port) + 4 + i * 4] = w;
    }
    regs[aux_ctl(port)] = (1u << 30) | static_cast<uint32_t>(reply.size()) << 20;
  }

  // ---- GMBUS: a monitor's EEPROM at 0x50 (with the segment pointer at 0x30) on one pin
  void gmbus_command(uint32_t v) {
    regs[rGmbus1] = v;
    if (v & (1u << 31)) { gm_nak = false; return; }        // SW_CLR_INT
    if (!(v & (1u << 30))) return;
    const uint32_t cycle = (v >> 25) & 7, address = (v >> 1) & 0x7F, count = (v >> 16) & 0x1FF;
    if (cycle & 4) { gm_segment = 0; return; }             // STOP
    const uint32_t pin = get(rGmbus0) & 0x1F;
    if (!monitor_pin || pin != monitor_pin || (address != 0x50 && address != 0x30)) { gm_nak = true; return; }
    if (cycle & 2) gm_offset = (v >> 8) & 0xFF;            // INDEX
    if (v & 1) {
      gm_left = count;
      ++gmbus_reads;
    } else if (address == 0x30) {
      gm_segment = get(rGmbus3) & 0xFF;
    } else {
      gm_offset = get(rGmbus3) & 0xFF;
    }
  }
  uint32_t gmbus_read() {
    uint32_t w = 0;
    for (uint32_t k = 0; k < 4 && gm_left; ++k, --gm_left) {
      const size_t at = gm_segment * 256 + gm_offset++;
      w |= uint32_t{at < edid.size() ? edid[at] : uint8_t{0xFF}} << (8 * k);
    }
    return w;
  }
};

// 1080p60 (148.5 MHz, 1920 + 88 + 44 + 148 / 1080 + 4 + 5 + 36, both syncs positive), 720p60 (74.25 MHz), 4K60 (594 MHz)
const std::vector<uint8_t> t1080 = {0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C, 0x45, 0x00, 0, 0, 0, 0, 0, 0x1E};
const std::vector<uint8_t> t720 = {0x01, 0x1D, 0x00, 0x72, 0x51, 0xD0, 0x1E, 0x20, 0x6E, 0x28, 0x55, 0x00, 0, 0, 0, 0, 0, 0x1E};
const std::vector<uint8_t> t4k = {0x28, 0xE8, 0x00, 0x30, 0xF0, 0x70, 0x2C, 0x80, 0x30, 0x20, 0x36, 0x00, 0, 0, 0, 0, 0, 0x18};

void fix_checksum(std::vector<uint8_t>& e, size_t block) {
  uint8_t sum = 0;
  for (size_t i = block; i < block + 127; ++i) sum = static_cast<uint8_t>(sum + e[i]);
  e[block + 127] = static_cast<uint8_t>(0 - sum);
}

// An EDID 1.3 with the detailed timings given and, if `hdmi`, a CTA extension with the HDMI vendor block.
std::vector<uint8_t> edid_with(const std::vector<std::vector<uint8_t>>& dtds, bool hdmi) {
  std::vector<uint8_t> e(hdmi ? 256 : 128, 0);
  const uint8_t header[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
  memcpy(e.data(), header, 8);
  e[18] = 1; e[19] = 3; e[20] = 0x80;
  for (size_t i = 0; i < dtds.size() && i < 4; ++i) memcpy(&e[54 + i * 18], dtds[i].data(), 18);
  if (hdmi) {
    e[126] = 1;
    const uint8_t cta[10] = {0x02, 0x03, 10, 0x00, 0x65, 0x03, 0x0C, 0x00, 0x10, 0x00};
    memcpy(&e[128], cta, sizeof(cta));
    fix_checksum(e, 128);
  }
  fix_checksum(e, 0);
  return e;
}

[[maybe_unused]] FakeAuxSink make_sink(const std::vector<uint8_t>& edid) {
  FakeAuxSink s;
  s.dpcd.assign(0x800, 0);
  const uint8_t caps[8] = {0x12, 0x14, 0xC4, 0x01, 0x00, 0x00, 0x01, 0x00};     // DPCD 1.2, HBR2, four lanes, TPS3 and enhanced framing, downspread
  for (size_t i = 0; i < 8; ++i) s.dpcd[i] = caps[i];
  s.dpcd[0x202] = s.dpcd[0x203] = 0x77;       // clock recovery, equalization and symbol lock at once
  s.dpcd[0x204] = 0x01;
  s.edid = edid;
  return s;
}

// A rig: the card, a GGTT window and local memory in host memory.
struct Rig {
  TestAllocator alloc;
  Card card;
  Clock clock;
  std::vector<uint64_t> ggtt = std::vector<uint64_t>(kGgttWindowBytes / 8, 0);
  std::vector<uint8_t> lmem = std::vector<uint8_t>(kLmemViewBytes, 0xCD);
  Display::Inputs inputs() {
    Display::Inputs in;
    in.registers = &card;
    in.clock = &clock;
    in.allocator = &alloc;
    in.ggtt = MmioRegion(reinterpret_cast<uint64_t>(ggtt.data()), ggtt.size() * 8);
    in.lmem = ByteView(lmem.data(), lmem.size());
    return in;
  }
};

// --- the tables and the arithmetic ----------------------------------------------------------------------------------------------------------
void test_tables() {
  // every MPLLB state makes the clock it is listed for (intel_mpllb_calc_port_clock); the DP rates the link training can choose are there
  for (const dg2::MpllbState& s : dg2::dp_mpllb_tables()) CHECK(dg2::mpllb_port_clock_khz(s) == s.clock_khz);
  uint32_t off = 0;
  for (const dg2::MpllbState& s : dg2::hdmi_mpllb_tables()) {
    const int64_t d = int64_t{dg2::mpllb_port_clock_khz(s)} - s.clock_khz;
    if (d < -1 || d > 1) { printf("  HDMI %u: formula %u\n", s.clock_khz, dg2::mpllb_port_clock_khz(s)); ++off; }
  }
  CHECK(off == 0);
  CHECK(dg2::dp_mpllb_tables().size() == 4 && dg2::hdmi_mpllb_tables().size() == 44);
  CHECK(dg2::find_dp_mpllb(162000) && dg2::find_dp_mpllb(270000) && dg2::find_dp_mpllb(540000) && dg2::find_dp_mpllb(810000) && !dg2::find_dp_mpllb(1000000));
  CHECK(dg2::find_hdmi_mpllb(148500) && dg2::find_hdmi_mpllb(74250) && dg2::find_hdmi_mpllb(25175) && dg2::find_hdmi_mpllb(594000) && !dg2::find_hdmi_mpllb(148000));
  // RBR, by hand from dg2_dp_rbr_100: CP_INT 4, CP_PROP 20, CP_INT_GS 65, CP_PROP_GS 127; DIV5_CLK_EN, TX_CLK_DIV 2, PMIX_EN, V2I 2, FREQ_VCO 2; ...
  const dg2::MpllbState& rbr = *dg2::find_dp_mpllb(162000);
  CHECK(rbr.cp == (4u << 25 | 20u << 9 | 65u << 17 | 127u << 1));
  CHECK(rbr.div == (1u << 29 | 2u << 5 | 1u << 10 | 2u << 26 | 2u << 24));
  CHECK(rbr.div2 == (2u << 12 | 226u) && rbr.fracn1 == (1u << 30 | 1u << 31 | 5u) && rbr.fracn2 == (39321u | 3u << 16) && rbr.ref_control == 3u << 27);
  // 148.5 MHz HDMI (dg2_hdmi_148_5): REF_CLK_DIV 1, MULTIPLIER 86, HDMI_DIV 1; fractional 26214 + 2/5; TX_CLK_DIV 2; up spread
  const dg2::MpllbState& h = *dg2::find_hdmi_mpllb(148500);
  CHECK((h.div2 & 0xFFF) == 86 && ((h.div2 >> 12) & 7) == 1 && ((h.div2 >> 15) & 7) == 1 && (h.fracn1 & 0xFFFF) == 5 && h.fracn2 == (26214u | 2u << 16) && h.sscen == 1u << 30);
  CHECK(h.cp == (4u << 25 | 15u << 9 | 64u << 17 | 124u << 1) && h.div == (1u << 29 | 2u << 5 | 1u << 10 | 2u << 26 | 3u << 24));

  // the drive levels: dg2_snps_trans by DP_TRAINING_LANEx_SET
  CHECK(dg2::dp_level(0x00) == 0 && dg2::dp_level(0x08) == 1 && dg2::dp_level(0x18) == 3 && dg2::dp_level(0x01) == 4 && dg2::dp_level(0x11) == 6);
  CHECK(dg2::dp_level(0x02) == 7 && dg2::dp_level(0x0A) == 8 && dg2::dp_level(0x03) == 9 && dg2::dp_level(0x0B) == 0 && dg2::dp_level(0x07) == 9);
  CHECK(dg2::tx_eq_value(0) == 25u << 18 && dg2::tx_eq_value(3) == (43u << 18 | 17u << 10) && dg2::tx_eq_value(9) == 62u << 18);

  // CDCLK: two pixels a clock, the lowest table entry that carries it
  CHECK(dg2::cdclk_for(148500) == 163200 && dg2::cdclk_for(594000) == 326400 && dg2::cdclk_for(1305600) == 652800 && dg2::cdclk_for(1400000) == 0);
  CHECK(dg2::cdclk_waveform(163200) == 0x8888 && dg2::cdclk_waveform(652800) == 0xFFFF && dg2::cdclk_waveform(300000) == 0);

  // M/N, 1080p on four lanes at RBR: data 148500 * 24 / (162000 * 4 * 8) = 0.6875, link 148500 / 162000; N fixed and reduced to 24 bits
  const dg2::LinkMn mn = dg2::compute_link_mn(24, 4, 148500, 162000);
  CHECK(mn.data_n == 0x800000 && mn.data_m == 0x580000 && mn.link_n == 0x80000 && mn.link_m == static_cast<uint32_t>(148500ull * 0x80000 / 162000));

  // the watermark: 148.5 MHz * 4 bytes * 20 us = 11880 bytes = 24 blocks (rounded up) + 1; a line is 14.8 us, so 2 lines + 1
  CrtcMode m;
  m.pixel_clock_khz = 148500; m.h_total = 2200;
  const dg2::Watermark wm = dg2::plane_watermark(m, 1024);
  CHECK(wm.blocks == 25 && wm.lines == 3);
  m.pixel_clock_khz = 2000000;
  CHECK(dg2::plane_watermark(m, 64).blocks == 63);

  // the modes of an EDID: HDMI takes a table clock up to 340 MHz (not 4K60's 594 MHz), DP what the link carries
  const std::vector<uint8_t> e = edid_with({t4k, t1080, t720, t1080}, false);
  Mode modes[kMaxModes];
  CHECK(edid_modes(ConstByteView(e.data(), e.size()), nullptr, ArrayView<Mode>(modes)) == 2 && modes[0].timing.h_display == 1920 && !modes[0].preferred);
  CHECK(!choose_mode(ArrayView<const Mode>(modes, 0)) && *choose_mode(ArrayView<const Mode>(modes, 2)) == 0);
  Dpcd::Capabilities caps;
  const uint8_t rx[16] = {0x12, 0x14, 0xC4, 0x01, 0, 0, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  CHECK(Dpcd::parse_capabilities(ConstByteView(rx, 16), Dpcd::kNoEdp, ConstByteView(), &caps));
  CHECK(edid_modes(ConstByteView(e.data(), e.size()), &caps, ArrayView<Mode>(modes)) == 3);
  CHECK(modes[0].timing.h_display == 3840 && modes[0].preferred && modes[0].lanes == 4 && modes[0].link_khz == 540000);
  CHECK(modes[1].lanes == 4 && modes[1].link_khz == 162000);
  // a damaged EDID: every truncation and bit flip of the base block is taken or refused without reading past it
  for (size_t n = 0; n < e.size(); ++n) (void)edid_modes(ConstByteView(e.data(), n), &caps, ArrayView<Mode>(modes));
  for (size_t bit = 0; bit < 128 * 8; ++bit) {
    std::vector<uint8_t> f = e;
    f[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    const uint32_t n = edid_modes(ConstByteView(f.data(), f.size()), bit % 2 ? &caps : nullptr, ArrayView<Mode>(modes));
    CHECK(n <= kMaxModes);
    for (uint32_t i = 0; i < n; ++i) CHECK(modes[i].timing.h_display > 0 && uint64_t{stride_of(modes[i].timing.h_display)} * modes[i].timing.v_display <= kSurfaceBytes);
  }
}

#ifdef TCOS_INTEL_DGPU_NATIVE
// --- the display core: the power well order, CDCLK, DBUF, MBUS --------------------------------------------------------------------------------
void test_core_init() {
  Rig rig;
  dg2::Engine engine(rig.card, rig.clock);
  CHECK(engine.init());
  Card& c = rig.card;
  const int32_t dc = c.first_write(rDcState), pw1 = c.first_write(rPwCtl2, 2u, 2u), pll = c.first_write(rDePll, 1u << 31, 1u << 31);
  const int32_t pwa = c.first_write(rPwCtl2, 2u << 10, 2u << 10), pw2 = c.first_write(rPwCtl2, 2u << 2, 2u << 2);
  CHECK(dc >= 0 && c.writes[dc].second == 0 && dc < pw1);                 // DC states off first
  CHECK(pw1 >= 0 && pw1 < pll && pll < pwa && pwa < pw2);                 // PW1, CDCLK, ..., PGA, PG2
  CHECK(c.first_write(rDePll) < pll && c.writes[c.first_write(rDePll)].second == 34);   // the ratio, then the enable
  CHECK(c.get(rDePll) == (34u | 1u << 31));
  CHECK(c.get(rSquash) == (1u << 31 | 0xFu << 24 | 0x8888));            // 163.2 MHz: half the clocks of 326.4 squashed away
  CHECK(c.get(rCdclkCtl) == (7u << 19 | 324));                          // cd2x divider 1, no pipe, decimal (163200 - 1000) / 500
  CHECK(engine.cdclk_khz() == 163200);
  CHECK((c.get(rDbuf1) & 0xC0F80000u) == (1u << 31 | 8u << 19) && (c.get(rDbuf2) & 0xC0F80000u) == (1u << 31 | 8u << 19));
  CHECK((c.get(rDbuf3) & (1u << 31)) == 0 && (c.get(rDbuf4) & 0x00F80000u) == 8u << 19);
  CHECK(c.get(rAbox0) == (1u << 20 | 1u << 16 | 16u << 8 | 16u) && c.get(rAbox1) == c.get(rAbox0));
  CHECK((c.get(0x46434) & 0x0F000000u) == 0x0F000000u && c.get(0x4421C) == 0xFFFFFFFFu && (c.get(0x46408) & 0x10));
  CHECK(engine.uncalibrated_phys() == 0);
  // CDCLK up for a 4K60 mode: the PLL stays (same VCO), only the waveform and CDCLK_CTL change
  const size_t before = c.written(rDePll);
  CHECK(engine.set_cdclk(326400) && c.written(rDePll) == before && c.get(rSquash) == (1u << 31 | 0xFu << 24 | 0xAAAA) && c.get(rCdclkCtl) == (7u << 19 | 651));
  CHECK(!engine.set_cdclk(300000));
  // a well that never answers
  Rig dead;
  dead.card.wells_answer = false;
  dg2::Engine e2(dead.card, dead.clock);
  CHECK(!e2.init() && strstr(e2.why(), "power well") != nullptr);
}

// --- HDMI on DDI B from scratch -----------------------------------------------------------------------------------------------------------
void test_hdmi() {
  Rig rig;
  rig.card.monitor_pin = 2;           // DDI B's GMBUS pin
  rig.card.edid = edid_with({t1080, t720}, true);
  Display display(rig.inputs());
  CHECK(display.start() == Status::Ok && display.running());
  CHECK(display.local_memory_bytes() == 16ull << 30);
  CHECK(display.connector() == 1 && display.connectors()[1].hdmi && !display.connectors()[1].dp && display.connectors()[1].edid_size == 256);
  CHECK(display.mode_count() == 2 && display.current_mode() == 0 && display.width() == 1920 && display.height() == 1080 && display.pitch_bytes() == 7680);
  CHECK(rig.card.gmbus_reads >= 2);
  Card& c = rig.card;
  // the PHY: DDI B is PHY B (0x169000), its PLL enable 0x46014
  const dg2::MpllbState& s = *dg2::find_hdmi_mpllb(148500);
  CHECK(c.get(snps(1, rMpllbCp)) == s.cp && c.get(snps(1, rMpllbDiv2)) == s.div2 && c.get(snps(1, rMpllbFracn1)) == s.fracn1 && c.get(snps(1, rMpllbFracn2)) == s.fracn2);
  CHECK(c.get(snps(1, rMpllbDiv)) == (s.div | 1u << 31));               // forced on after the enable
  const int32_t div_plain = c.first_write(snps(1, rMpllbDiv), 0xFFFFFFFFu, s.div), enable = c.first_write(pll_enable(1), 1u << 31, 1u << 31);
  const int32_t forced = c.first_write(snps(1, rMpllbDiv), 1u << 31, 1u << 31);
  CHECK(div_plain >= 0 && div_plain < enable && enable < forced);
  CHECK(c.first_write(snps(0, rMpllbCp)) < 0);                           // no other PHY touched
  for (uint32_t lane = 0; lane < 4; ++lane) CHECK(c.get(snps(1, rTxEq0) + lane * 0x10) == 62u << 18);   // the HDMI default level
  // the DDI IO well of B (index 1) on; TRANS_CLK_SEL by PHY B; TRANS_DDI_FUNC_CTL: enable, port B, HDMI, 8 bpc, both syncs positive
  CHECK(c.get(rPwDdi2) & (2u << 2));
  CHECK(c.get(rClkSel) == 2u << 28);
  CHECK(c.get(rFuncCtl) == (1u << 31 | 2u << 27 | 0u << 24 | 1u << 17 | 1u << 16));
  // timings
  CHECK(c.get(rTransHtotal) == (1919u | 2199u << 16) && c.get(rTransHblank) == (1919u | 2199u << 16) && c.get(rTransHsync) == (2007u | 2051u << 16));
  CHECK(c.get(rTransVtotal) == (1079u | 1124u << 16) && c.get(rTransVblank) == (0u | 1124u << 16) && c.get(rTransVsync) == (1083u | 1088u << 16));
  CHECK(c.get(rPipeSrc) == (1919u << 16 | 1079u) && c.get(rPipeMisc) == 1u << 8 && c.get(rTransConf) == 1u << 31);
  // the plane: linear XRGB8888, 120 x 64 bytes a row, the surface at its GGTT address; DDB 0..1023, watermark level 0
  CHECK(c.get(rPlaneCtl) == (1u << 31 | 4u << 24) && c.get(rPlaneStride) == 120 && c.get(rPlaneSize) == (1079u << 16 | 1919u) && c.get(rPlaneSurf) == kSurfaceGgtt);
  CHECK(c.get(rPlaneBufCfg) == 1023u << 16 && c.get(rPlaneWm0) == (1u << 31 | 3u << 14 | 25u));
  CHECK(c.last_write(rPlaneSurf) > c.last_write(rPlaneCtl));
  // the order of intel_enable_ddi + intel_enable_ddi_hdmi: the function enabled, the transcoder on, then the buffer
  const int32_t func = c.first_write(rFuncCtl, 1u << 31, 1u << 31), conf = c.first_write(rTransConf, 1u << 31, 1u << 31), buf = c.first_write(ddi_buf(1), 1u << 31, 1u << 31);
  CHECK(enable < func && func < conf && conf < buf);
  // the GGTT: every page of the surface in local memory with the LM bit, the flush written
  bool ptes = true;
  for (uint64_t i = 0; i < kSurfaceBytes / 4096; ++i) ptes = ptes && rig.ggtt[kSurfaceGgtt / 4096 + i] == ((kSurfaceLmemOffset + i * 4096) | 1u | 2u);
  CHECK(ptes && rig.ggtt[kSurfaceGgtt / 4096 - 1] == 0 && rig.ggtt[(kSurfaceGgtt + kSurfaceBytes) / 4096] == 0);
  CHECK(c.first_write(rGfxFlush) >= 0);
  // the picture is local memory at the surface, cleared
  CHECK(display.pixels().data() == rig.lmem.data() + kSurfaceLmemOffset && display.pixels().size() == 7680ull * 1080);
  CHECK(rig.lmem[kSurfaceLmemOffset] == 0 && rig.lmem[kSurfaceLmemOffset + 7680ull * 1080 - 1] == 0 && rig.lmem[kSurfaceLmemOffset - 1] == 0xCD);

  // another mode: 720p at 74.25 MHz on the same port
  c.writes.clear();
  CHECK(display.set_mode(1) == Status::Ok && display.width() == 1280 && display.current_mode() == 1);
  CHECK(c.get(snps(1, rMpllbDiv2)) == dg2::find_hdmi_mpllb(74250)->div2 && c.get(rPlaneSize) == (719u << 16 | 1279u) && c.get(rPlaneStride) == 80);
  CHECK(c.first_write(rTransConf, 1u << 31, 0) < c.first_write(rTransConf, 1u << 31, 1u << 31));   // off before on again
  CHECK(display.set_mode(5) == Status::BadIndex);
  // the shutdown: plane, transcoder, buffer, PLL, DDI IO well off
  display.shutdown();
  CHECK(c.get(rPlaneCtl) == 0 && (c.get(rTransConf) & (1u << 31)) == 0 && (c.get(ddi_buf(1)) & (1u << 31)) == 0 && (c.get(pll_enable(1)) & (1u << 31)) == 0);
  CHECK((c.get(rPwDdi2) & (2u << 2)) == 0 && c.get(rClkSel) == 0 && !display.running());
}

// --- DisplayPort on DDI A: AUX, EDID over AUX, link training --------------------------------------------------------------------------------
void test_displayport() {
  Rig rig;
  FakeAuxSink sink = make_sink(edid_with({t1080, t720}, false));
  rig.card.sinks[0] = &sink;
  Display display(rig.inputs());
  CHECK(display.start() == Status::Ok);
  CHECK(display.connector() == 0 && display.connectors()[0].dp && display.mode_count() == 2 && display.width() == 1920);
  CHECK(rig.card.aux_unpowered == 0);              // every AUX transaction came after its well
  CHECK(sink.malformed == 0 && sink.outside_transaction == 0);
  Card& c = rig.card;
  const dg2::Engine& engine = display.engine();
  CHECK(engine.training().trained() && engine.training().lanes == 4 && engine.training().bandwidth_code == 6);
  // RBR on PHY A
  const dg2::MpllbState& rbr = *dg2::find_dp_mpllb(162000);
  CHECK(c.get(snps(0, rMpllbCp)) == rbr.cp && c.get(snps(0, rMpllbDiv)) == (rbr.div | 1u << 31) && c.get(snps(0, rMpllbDiv2)) == rbr.div2);
  CHECK(c.get(snps(0, rMpllbFracn1)) == rbr.fracn1 && c.get(snps(0, rMpllbFracn2)) == rbr.fracn2);
  CHECK(c.get(pll_enable(0)) & (1u << 31));
  // the buffer: four lanes, on; DP_TP_CTL: on, SST, enhanced framing, normal pattern at the end; the pattern sequence 1, 3, idle, normal
  CHECK(c.get(ddi_buf(0)) == (1u << 31 | 3u << 1));
  CHECK(c.get(rDpTpCtl) == (1u << 31 | 1u << 18 | 3u << 8));
  std::vector<uint32_t> patterns;
  for (auto& w : c.writes)
    if (w.first == rDpTpCtl && (patterns.empty() || patterns.back() != ((w.second >> 8) & 7))) patterns.push_back((w.second >> 8) & 7);
  CHECK(patterns == std::vector<uint32_t>({0, 4, 2, 3}));
  CHECK(c.first_write(rDpTpCtl) < c.first_write(ddi_buf(0), 1u << 31, 1u << 31));    // the pattern before the buffer
  // the function: port A, DP SST, four lanes; programmed (without the enable) before the training and enabled after it
  CHECK(c.get(rFuncCtl) == (1u << 31 | 1u << 27 | 2u << 24 | 3u << 1 | 1u << 17 | 1u << 16));
  CHECK(c.first_write(rFuncCtl) < c.first_write(rDpTpCtl) && c.first_write(rFuncCtl, 1u << 31, 1u << 31) > c.last_write(rDpTpCtl));
  CHECK(c.get(rClkSel) == 1u << 28);
  // M/N
  const dg2::LinkMn mn = dg2::compute_link_mn(24, 4, 148500, 162000);
  CHECK(c.get(rDataM) == (63u << 25 | mn.data_m) && c.get(rDataN) == mn.data_n && c.get(rLinkM) == mn.link_m && c.get(rLinkN) == mn.link_n);
  // the drive: level 0 on every lane (the sink asked for nothing more)
  for (uint32_t lane = 0; lane < 4; ++lane) CHECK(c.get(snps(0, rTxEq0) + lane * 0x10) == 25u << 18);
  // the sink: D0, downspread, four lanes with enhanced framing, RBR, patterns, off at the end
  auto wrote = [&](uint32_t address, uint8_t value) {
    for (auto& w : sink.dpcd_writes)
      if (w.address == address && !w.bytes.empty() && w.bytes[0] == value) return true;
    return false;
  };
  CHECK(wrote(0x600, 1) && wrote(0x107, 0x10) && wrote(0x101, 0x84) && wrote(0x100, 0x06) && wrote(0x102, 0x01) && wrote(0x102, 0x03));
  CHECK(sink.dpcd_writes.back().address == 0x102 && sink.dpcd_writes.back().bytes[0] == 0);
  // the AUX well of A on, and the DDI IO well; nothing on the GMBUS of A (it is a DP port)
  CHECK((c.get(rPwAux2) & 2u) && (c.get(rPwDdi2) & 2u) && c.gmbus_reads == 0);
  // the other ports were asked: their AUX wells turned off again (no sink), their DDC asked
  CHECK((c.get(rPwAux2) & (2u << 2)) == 0 && display.connectors()[1].looked && display.connectors()[4].looked == false);
  display.shutdown();
  CHECK((c.get(rDpTpCtl) & (1u << 31)) == 0 && (c.get(ddi_buf(0)) & (1u << 31)) == 0);
}

// --- what is refused -----------------------------------------------------------------------------------------------------------------------
void test_refusals() {
  {   // nothing plugged in
    Rig rig;
    Display d(rig.inputs());
    CHECK(d.start() == Status::NoMonitor && !d.running() && d.connector() == -1);
    CHECK(rig.card.first_write(rPlaneCtl) < 0 && rig.card.first_write(snps(0, rMpllbCp)) < 0);
  }
  {   // local memory not trained: nothing written
    Rig rig;
    rig.card.lmem_init = false;
    rig.card.monitor_pin = 1;
    rig.card.edid = edid_with({t1080}, true);
    Display d(rig.inputs());
    CHECK(d.start() == Status::NoLocalMemory && rig.card.writes.empty());
  }
  {   // too small a window onto local memory
    Rig rig;
    Display::Inputs in = rig.inputs();
    in.lmem = in.lmem.first(kLmemViewBytes - 1);
    Display d(in);
    CHECK(d.start() == Status::TooLittleMemory && rig.card.writes.empty());
  }
  {   // a monitor whose only mode the PHY has no table for (4K60 over HDMI: 594 MHz needs scrambling)
    Rig rig;
    rig.card.monitor_pin = 3;
    rig.card.edid = edid_with({t4k}, true);
    Display d(rig.inputs());
    CHECK(d.start() == Status::NoMode);
  }
  {   // the port PLL never locks: the mode set fails at the PLL, which is switched off again
    Rig rig;
    rig.card.pll_locks = false;
    rig.card.monitor_pin = 4;          // DDI D (port 7, PHY D)
    rig.card.edid = edid_with({t1080}, true);
    Display d(rig.inputs());
    CHECK(d.start() == Status::ModeSet && d.stage() == dg2::Stage::Pll && strstr(d.status(), "PLL") != nullptr);
    CHECK(rig.card.first_write(0x4601C, 1u << 31, 1u << 31) >= 0 && (rig.card.get(0x4601C) & (1u << 31)) == 0);
    CHECK(rig.card.first_write(rPlaneCtl, 1u << 31, 1u << 31) < 0);
  }
  {   // a link that never trains: the buffer, the PLL and the IO well off again, no plane
    Rig rig;
    FakeAuxSink sink = make_sink(edid_with({t1080}, false));
    sink.dpcd[0x202] = sink.dpcd[0x203] = 0;
    rig.card.sinks[2] = &sink;
    Display d(rig.inputs());
    CHECK(d.start() == Status::ModeSet && d.stage() == dg2::Stage::LinkTraining && !d.engine().training().trained());
    CHECK((rig.card.get(ddi_buf(2)) & (1u << 31)) == 0 && (rig.card.get(pll_enable(2)) & (1u << 31)) == 0 && (rig.card.get(rPwDdi2) & (2u << 4)) == 0);
    CHECK(rig.card.first_write(rPlaneCtl, 1u << 31, 1u << 31) < 0);
  }
  {   // a sink that answers rubbish on AUX: no crash, nothing it said is trusted beyond the bounds
    Rig rig;
    FakeAuxSink sink = make_sink(edid_with({t1080}, false));
    sink.random_replies = true;
    rig.card.sinks[0] = &sink;
    Display d(rig.inputs());
    const Status s = d.start();
    CHECK(s == Status::Ok || s == Status::NoMonitor || s == Status::NoMode || s == Status::ModeSet);
  }
  {   // a damaged EDID over DDC: each truncation of the monitor's memory and flips in the base block
    for (uint32_t n = 0; n < 256; n += 7) {
      Rig rig;
      rig.card.monitor_pin = 2;
      rig.card.edid = edid_with({t1080, t720}, true);
      rig.card.edid.resize(n);
      Display d(rig.inputs());
      const Status s = d.start();
      CHECK(s == Status::Ok || s == Status::NoMonitor || s == Status::NoMode);
    }
    for (uint32_t bit = 0; bit < 1024; bit += 37) {
      Rig rig;
      rig.card.monitor_pin = 2;
      rig.card.edid = edid_with({t1080, t720}, true);
      rig.card.edid[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
      Display d(rig.inputs());
      const Status s = d.start();
      CHECK(s == Status::Ok || s == Status::NoMonitor || s == Status::NoMode);
    }
  }
}
#endif

// --- the IGraphicsProcessingUnit ---------------------------------------------------------------------------------------------------------------
void test_gpu() {
  TestAllocator alloc;
  PciDeviceInfo device{};
  device.vendor_id = 0x8086;
  device.device_id = 0x56A0;      // Arc A770 (DG2)
  device.class_code = kPciClassDisplay;
  CHECK(IntelSecondaryGpu::supports(device));
  PciDeviceInfo other = device;
  other.device_id = 0x4905;       // DG1: supported by the row, refused by initialize()
  CHECK(IntelSecondaryGpu::supports(other));
  other.device_id = 0x9A49;       // Tiger Lake (integrated): not this driver's
  CHECK(!IntelSecondaryGpu::supports(other));
  other = device;
  other.vendor_id = 0x1002;
  CHECK(!IntelSecondaryGpu::supports(other));

  Rig rig;
  rig.card.monitor_pin = 2;
  rig.card.edid = edid_with({t1080, t720}, true);
  GpuContext context{nullptr, nullptr, device, &alloc, nullptr};
  IntelSecondaryGpu gpu(context);
  CHECK(strstr(gpu.name(), "Arc") != nullptr || strstr(gpu.name(), "Xe") != nullptr);
#ifndef TCOS_INTEL_DGPU_NATIVE
  // built without the flag: both ways in refuse at once, nothing is written to the card
  CHECK(!gpu.initialize() && strstr(gpu.status(), "TCOS_INTEL_DGPU_NATIVE") != nullptr);
  CHECK(!gpu.initialize_with(rig.inputs(), 0x56A0) && rig.card.writes.empty());
  CHECK(gpu.mode_count() == 0 && gpu.connector_count() == 0 && gpu.width() == 0);
  gpu.shutdown();
#else
  CHECK(!gpu.initialize());           // (no register mapper or PCI bus in the context)
  CHECK(rig.card.writes.empty());
  // DG1 and Battlemage: refused before anything is touched
  CHECK(!gpu.initialize_with(rig.inputs(), 0x4905) && strstr(gpu.status(), "DG1") != nullptr && rig.card.writes.empty());
  CHECK(!gpu.initialize_with(rig.inputs(), 0xE20B) && strstr(gpu.status(), "Battlemage") != nullptr && rig.card.writes.empty());
  CHECK(gpu.initialize_with(rig.inputs(), 0x56A0) && strcmp(gpu.status(), "running") == 0);
  CHECK(gpu.mode_count() == 2 && gpu.width() == 1920 && gpu.height() == 1080 && gpu.current_mode() == 0);
  DisplayMode dm;
  CHECK(gpu.mode_info(1, &dm) && dm.width == 1280 && dm.height == 720 && !gpu.mode_info(2, &dm));
  uint64_t total = 0, used = 0;
  CHECK(gpu.video_memory(&total, &used) && total == (16ull << 30));
  // the connectors: the driven one (DDI B, HDMI) first, then DDI A, C, D (TC1 not listed)
  CHECK(gpu.connector_count() == 4);
  ConnectorInfo ci;
  CHECK(gpu.connector_info(0, &ci) && ci.connected && ci.type == ConnectorType::Hdmi && strcmp(ci.name, "DDI B") == 0 && ci.preferred_width == 1920);
  CHECK(gpu.connector_info(1, &ci) && !ci.connected && ci.type == ConnectorType::Unknown && strcmp(ci.name, "DDI A") == 0);
  uint8_t edid[512];
  CHECK(gpu.read_edid(0, ByteView(edid, sizeof(edid))) == 256 && gpu.read_edid(1, ByteView(edid, sizeof(edid))) == 0);
  CHECK(gpu.mode_available(0, 1280, 720) && !gpu.mode_available(0, 1024, 768) && !gpu.mode_available(1, 1280, 720));
  MonitorMode mm[4];
  CHECK(gpu.device_modes(0, mm, 4) == 2 && mm[0].preferred && mm[0].refresh_millihz == 60000 && mm[1].width == 1280);
  // drawing reaches local memory; another mode
  gpu.fill_rect(0, 0, 1, 1, 0x00112233);
  CHECK(rig.lmem[kSurfaceLmemOffset] == 0x33 && rig.lmem[kSurfaceLmemOffset + 2] == 0x11);
  CHECK(gpu.set_display_mode(0, 1280, 720) && gpu.width() == 1280 && gpu.current_mode() == 1);
  gpu.shutdown();
  CHECK(gpu.mode_count() == 0 && gpu.width() == 0 && (rig.card.get(rPlaneCtl) & (1u << 31)) == 0);
#endif
}

}  // namespace

int main() {
  test_tables();
#ifdef TCOS_INTEL_DGPU_NATIVE
  test_core_init();
  test_hdmi();
  test_displayport();
  test_refusals();
#endif
  test_gpu();
  if (g_failed) {
    printf("IntelSecondaryGpuTest: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  printf("IntelSecondaryGpuTest: all %d checks passed\n", g_checks);
  return 0;
}
