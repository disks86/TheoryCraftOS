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

// Host tests for the DCE 12 display engine of the discrete Vega GPUs (AMD/AmdDce12.*) against a register file: the absolute register numbers (segment bases of
// vega10_ip_offset.h plus the numbers of dce_12_0_offset.h, mmhub_1_0_offset.h, nbio_6_1_offset.h and hdp_4_0_offset.h, typed in here), the memory map as the POST leaves it
// and its refusals (and random register values), the memory set-up's values, the HDP flush, the pipe fuses, the DDC pads, every Pipe operation's register values, the ATOM
// firmware command tables' parameter words (a video BIOS whose tables copy their parameters to registers), the DisplayPort source, and the parsers of the ATOM firmware data
// tables the driver needs (display_object_info_table 1.4 with gpio_pin_lut 2.1: AtomConnectors; the I2C pin lookup: AtomI2c::lookup_v21) with truncation and bit flips.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDce12.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AtomConnectors.hpp"
#include "AtomBiosImage.hpp"

using namespace hardware;
using namespace hardware::amd_dce12;

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

// Absolute register numbers, typed in: DCE segment 1 0xC0, segment 2 0x34C0; MMHUB 0x1A000; NBIO segment 2 0xD20; HDP 0xF20.
constexpr uint32_t D = 0x34C0;
constexpr uint32_t kGrphEnable = D + 0x55A, kGrphControl = D + 0x55B, kGrphSwap = D + 0x55D, kGrphPrimary = D + 0x55E, kGrphPitch = D + 0x560, kGrphPrimaryHigh = D + 0x561;
constexpr uint32_t kGrphXStart = D + 0x565, kGrphXEnd = D + 0x567, kGrphYEnd = D + 0x568, kGrphUpdate = D + 0x56A, kGrphFlip = D + 0x56B, kPrescale = D + 0x576, kDenorm = D + 0x594;
constexpr uint32_t kLbFormat = D + 0x61A, kLbMemory = D + 0x61B;
constexpr uint32_t kArb1 = D + 0x67A, kWmMask = D + 0x67C, kUrgency = D + 0x67D, kUrgentLevel = D + 0x67E, kStutter = D + 0x67F, kStutter2 = D + 0x680, kLowPower = D + 0x681;
constexpr uint32_t kSclMode = D + 0x69C, kSclBypass = D + 0x69F, kViewportStart = D + 0x6AF, kViewportSize = D + 0x6B0;
constexpr uint32_t kCrtcBlank = D + 0x6F0, kStartLine = D + 0x706, kDoubleBuffer = D + 0x709, kMasterLock = D + 0x70E, kMasterMode = D + 0x70F, kFmtBitDepth = D + 0x747;
constexpr uint32_t kPipeStride = 0x200;
constexpr uint32_t kDmifBuffer0 = D + 0xEF, kPipeDis = D + 0xE0, kAgpBase = D + 0x128, kAgpBot = D + 0x129, kAgpTop = D + 0x12A;
constexpr uint32_t kD1Vga = 0xC0 + 0x0C, kD3Vga = 0xC0 + 0x38, kVgaHdp = 0xC0 + 0x0A, kVgaRender = 0xC0 + 0x00;
constexpr uint32_t kDigFe0 = D + 0x187E, kDigBe0 = D + 0x18C5, kDigStride = 0x100, kHpd0 = D + 0x1602;
constexpr uint32_t kDdc1Mask = D + 0x20E6, kDdc3A = D + 0x20EF, kHpdA = D + 0x210B;
constexpr uint32_t kFbOffset = 0x1A817, kDefaultLsb = 0x1A818, kDefaultMsb = 0x1A819, kFbBase = 0x1A82C, kFbTop = 0x1A82D, kAgpTopMc = 0x1A82E, kAgpBotMc = 0x1A82F,
                   kAgpBaseMc = 0x1A830, kApLow = 0x1A831, kApHigh = 0x1A832, kTlb = 0x1A833;
constexpr uint32_t kMemsize = 0xD20 + 0xC3, kBifFbEn = 0xD20 + 0xFF, kHdpFlush = 0xD20 + 0xF7, kHdpMisc = 0xF20 + 0xD3, kNonsurface = 0xF20 + 0x40, kNonsurfaceHi = 0xF20 + 0x42;

struct Clock final : IClock {
  mutable uint64_t now = 0;
  uint64_t now_us() const override { now += 10; return now; }
  const char* name() const override { return "clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

struct Regs final : IVcnRegisters {
  std::map<uint32_t, uint32_t> regs;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  std::vector<uint32_t> reads;
  bool dmif_completes = true;
  uint32_t get(uint32_t r) const { auto it = regs.find(r); return it == regs.end() ? 0 : it->second; }
  uint32_t read(uint32_t r) override {
    reads.push_back(r);
    uint32_t v = get(r);
    if (r >= kDmifBuffer0 && r < kDmifBuffer0 + 6) v = dmif_completes ? (v | 0x10) : (v & ~0x10u);
    return v;
  }
  void write(uint32_t r, uint32_t v) override { writes.push_back({r, v}); regs[r] = v; }
  uint32_t count(uint32_t r) const { uint32_t n = 0; for (const auto& w : writes) n += w.first == r; return n; }
};

// --- register numbers ------------------------------------------------------------------------------------------------------------------------
void test_numbers() {
  CHECK(Pipe::grph_enable(0) == kGrphEnable && Pipe::grph_enable(5) == kGrphEnable + 5 * kPipeStride && Pipe::grph_enable(6) == 0);
  CHECK(Pipe::crtc_blank_control(0) == kCrtcBlank && Pipe::crtc_blank_control(3) == kCrtcBlank + 3 * kPipeStride);
  CHECK(hpd_gpio_register() == kHpdA);
  CHECK(ddc_line_of(kDdc3A) == 2 && ddc_line_of(kDdc3A + 1) == kMaxDdc && ddc_line_of(D + 0x20E7) == 0 && ddc_line_of(D + 0x20FB) == 5);
  const AtomI2c::Bus b = ddc_bus(0);
  CHECK(b.valid && b.mask_clk_reg == kDdc1Mask && b.a_clk_reg == kDdc1Mask + 1 && b.en_clk_reg == kDdc1Mask + 2 && b.y_clk_reg == kDdc1Mask + 3);
  CHECK(b.mask_data_reg == b.mask_clk_reg && b.a_data_reg == b.a_clk_reg && b.en_data_reg == b.en_clk_reg && b.y_data_reg == b.y_clk_reg);
  CHECK(b.mask_clk_mask == 1 && b.mask_data_mask == 0x100 && b.a_clk_mask == 1 && b.a_data_mask == 0x100 && b.en_clk_mask == 1 && b.en_data_mask == 0x100 && b.y_clk_mask == 1 &&
        b.y_data_mask == 0x100);
  CHECK(ddc_bus(2).a_clk_reg == kDdc3A && !ddc_bus(6).valid);
}

// --- memory -------------------------------------------------------------------------------------------------------------------------------------
void set_posted(Regs& r, uint32_t mib, uint32_t base16m, uint32_t fb_offset = 0) {
  r.regs[kMemsize] = mib;
  r.regs[kFbBase] = base16m;
  r.regs[kFbTop] = base16m + (mib + 15) / 16 - 1;
  r.regs[kFbOffset] = fb_offset;
}

void test_memory() {
  Regs r;
  set_posted(r, 8192, 0xF400);
  MemoryMap m = read_memory_map(r);
  CHECK(m.read && m.video_memory_bytes == (8192ull << 20) && m.gpu_base == 0xF400ull << 24 && m.gpu_top == (0xF400ull + 511) << 24 && m.fb_offset == 0);
  CHECK(r.writes.empty());
  // refusals: no size, no answer, a top below the base, a span too small or far too big for the memory
  const struct { uint32_t mib, base, top; } bad[] = {{0, 0xF400, 0xF5FF}, {0xFFFFFFFF, 0xF400, 0xF5FF}, {8192, 0xFFFFFFFF, 0xF5FF}, {8192, 0xF400, 0xF3FF},
                                                    {8192, 0xF400, 0xF400}, {256, 0xF400, 0xF5FF}, {128 * 1024, 0, 0xFFFFFF}};
  for (const auto& b : bad) {
    Regs x;
    x.regs[kMemsize] = b.mib;
    x.regs[kFbBase] = b.base;
    x.regs[kFbTop] = b.top;
    CHECK(!read_memory_map(x).read);
  }
  // random values: whatever is read, an accepted map covers its memory
  uint32_t rng = 7;
  for (int i = 0; i < 20000; ++i) {
    Regs x;
    rng = rng * 1664525u + 1013904223u; x.regs[kMemsize] = rng >> (rng & 15);
    rng = rng * 1664525u + 1013904223u; x.regs[kFbBase] = rng >> (rng & 7);
    rng = rng * 1664525u + 1013904223u; x.regs[kFbTop] = (x.regs[kFbBase] & 0xFFFFFF) + (rng >> 22);
    const MemoryMap mm = read_memory_map(x);
    if (mm.read) CHECK(mm.video_memory_bytes > 0 && mm.gpu_top >= mm.gpu_base && mm.gpu_top + (16ull << 20) - mm.gpu_base >= mm.video_memory_bytes);
  }

  // the set-up: 8 GiB at 0xF4_0000_0000, the scratch page 16 MiB in, a physical offset of 32 MiB
  Regs w;
  set_posted(w, 8192, 0xF400, 2);
  w.regs[kTlb] = 0x80000000;               // (a bit not touched)
  w.regs[kVgaHdp] = 0x1;
  w.regs[kVgaRender] = 0x30001;
  w.regs[kAgpBot] = 0x55555555;
  const MemoryMap map = read_memory_map(w);
  const uint64_t base = 0xF400ull << 24, scratch = base + (16ull << 20);
  CHECK(map.fb_offset == (2ull << 24));
  CHECK(!program_memory(w, map, scratch + 1) && !program_memory(w, map, base - 4096) && !program_memory(w, map, base + map.video_memory_bytes) && !program_memory(w, MemoryMap(), scratch));
  CHECK(w.writes.empty());
  CHECK(program_memory(w, map, scratch));
  CHECK(w.get(kVgaHdp) == 0x11 && w.get(kVgaRender) == 0x1);
  CHECK((w.get(kHdpMisc) & 1) && w.get(kNonsurface) == static_cast<uint32_t>(base >> 8) && w.get(kNonsurfaceHi) == static_cast<uint32_t>(base >> 40));
  CHECK(w.get(kAgpBaseMc) == 0 && w.get(kAgpBotMc) == 0xFFFFFF && w.get(kAgpTopMc) == 0);
  CHECK(w.get(kApLow) == static_cast<uint32_t>(base >> 18) && w.get(kApHigh) == static_cast<uint32_t>((base + (8192ull << 20) - 1) >> 18));
  CHECK(w.get(kDefaultLsb) == static_cast<uint32_t>(((16ull << 20) + (2ull << 24)) >> 12) && w.get(kDefaultMsb) == 0);
  CHECK(w.get(kTlb) == (0x80000000u | 1u | 3u << 3 | 1u << 6 | 3u << 11 | 1u << 13));     // L1 TLB, system access 3, advanced driver model, MTYPE_UC, ATC
  CHECK(w.get(kBifFbEn) == 3 && w.count(kHdpFlush) == 1 && w.get(kHdpFlush) == 0);
  CHECK(w.get(kAgpBase) == 0 && w.get(kAgpBot) == (0x55555555u & ~0x3FFFFu) + 0x3FFFF && w.get(kAgpTop) == 0);
  // the flush: a write of the NBIO's HDP memory coherency register, read back
  Regs f;
  flush_hdp(f);
  CHECK(f.writes.size() == 1 && f.writes[0].first == kHdpFlush && f.reads.size() == 1 && f.reads[0] == kHdpFlush);
  Regs fuse;
  fuse.regs[kPipeDis] = 0xC5;
  CHECK(pipe_fuses(fuse) == 0x05);
}

// --- the pipe -------------------------------------------------------------------------------------------------------------------------------------
void test_pipe() {
  Clock clock;
  Regs r;
  Pipe p(r, clock);
  // blank: the blank data double buffered, then blanked; unblanked writes the control 0
  r.regs[kCrtcBlank + kPipeStride] = 0x10000;
  CHECK(p.set_blank(1, true) && r.get(kDoubleBuffer + kPipeStride) == 0x10000 && r.get(kCrtcBlank + kPipeStride) == 0x100);
  CHECK(!p.blanked(1));
  r.regs[kCrtcBlank + kPipeStride] |= 1;
  CHECK(p.blanked(1));
  CHECK(p.set_blank(1, false) && r.get(kCrtcBlank + kPipeStride) == 0 && !p.set_blank(6, true));
  // the update mode and the underflow lock cleared, the rest kept
  r.regs[kMasterMode] = 0x30003;
  r.regs[kMasterLock] = 0x10101;
  CHECK(p.prepare_enable(0) && r.get(kMasterMode) == 0x30000 && r.get(kMasterLock) == 0x00101);
  // the advanced request: 1080p (sync start 1084, total 1125): 41 lines of sync and back porch, at most 10; the legacy requestor off
  CrtcMode m;
  m.pixel_clock_khz = 148500;
  m.h_display = 1920; m.h_sync_start = 2008; m.h_sync_end = 2052; m.h_total = 2200;
  m.v_display = 1080; m.v_sync_start = 1084; m.v_sync_end = 1089; m.v_total = 1125;
  r.regs[kStartLine] = 0x105;
  CHECK(p.advanced_request(0, m) && r.get(kStartLine) == (0x005u | 10u << 12));
  CrtcMode tight = m;
  tight.v_sync_start = 1080; tight.v_total = 1086;     // no front porch: one line is assumed (the workaround), five left
  CHECK(p.advanced_request(0, tight) && ((r.get(kStartLine) >> 12) & 0xFF) == 5);
  CrtcMode wrong = m;
  wrong.v_total = 1000;
  CHECK(!p.advanced_request(0, wrong));
  // the VGA mode off: the four fields cleared, D3's register in its own place
  r.regs[kD3Vga] = 0x03010301;
  CHECK(p.disable_vga(2) && r.get(kD3Vga) == 0x03000000);
  r.regs[kD1Vga] = 0x1;
  CHECK(p.disable_vga(0) && r.get(kD1Vga) == 0);
  // FMT: truncation to 8 (depth 1) and 10 (depth 2); other depths zero
  CHECK(p.program_fmt(0, 8) && r.get(kFmtBitDepth) == 0x11 && p.program_fmt(0, 10) && r.get(kFmtBitDepth) == 0x21 && p.program_fmt(0, 12) && r.get(kFmtBitDepth) == 0);
  // the surface on pipe 1
  amd_dce11::ScanoutSurface s;
  s.address = 0xF402000000ull;
  s.width = s.viewport_width = 1920;
  s.height = s.viewport_height = 1080;
  s.pitch_pixels = 1920;
  const uint32_t o = kPipeStride;
  r.regs[kGrphControl + o] = 0x7FFFFFFF;       // garbage the plane must clear
  r.regs[kGrphSwap + o] = 0x30;
  r.regs[kPrescale + o] = 0x1F;
  r.regs[kSclMode + o] = 0x13;
  r.regs[kLbFormat + o] = 0x80000003;
  r.writes.clear();
  CHECK(p.set_surface(1, s));
  CHECK(r.get(kGrphEnable + o) == 1 && r.get(kGrphControl + o) == (0x80000000u | 0x2u | (0x7FFFFFFFu & ~(0x3u | 0x4u | 0x700u | 0x7000u | 0xC0000u | 0x01F00000u | 0x70000000u))));
  CHECK(r.get(kGrphSwap + o) == 0 && r.get(kPrescale + o) == 0x10 && r.get(kGrphPitch + o) == 1920 && r.get(kGrphXStart + o) == 0 && r.get(kGrphXEnd + o) == 1920 &&
        r.get(kGrphYEnd + o) == 1080);
  CHECK(r.get(kGrphPrimary + o) == 0x02000000 && r.get(kGrphPrimaryHigh + o) == 0xF4 && r.get(kGrphUpdate + o) == 0 && r.get(kGrphFlip + o) == 0);
  CHECK(r.get(kSclMode + o) == 0 && r.get(kSclBypass + o) == 0 && r.get(kViewportStart + o) == 0 && r.get(kViewportSize + o) == (1920u << 16 | 1080u));
  CHECK(r.get(kLbFormat + o) == 0x4 && r.get(kLbMemory + o) == 0x6B0 && (r.get(kDenorm + o) & 7) == 3);
  // the flip's order: locked, the high half, the low half, unlocked
  int lock_at = -1, high_at = -1, low_at = -1, unlock_at = -1;
  for (size_t i = 0; i < r.writes.size(); ++i) {
    if (r.writes[i].first == kGrphUpdate + o) (r.writes[i].second & 0x10000) ? lock_at = static_cast<int>(i) : unlock_at = static_cast<int>(i);
    if (r.writes[i].first == kGrphPrimaryHigh + o) high_at = static_cast<int>(i);
    if (r.writes[i].first == kGrphPrimary + o) low_at = static_cast<int>(i);
  }
  CHECK(lock_at >= 0 && lock_at < high_at && high_at < low_at && low_at < unlock_at);
  // refusals: misaligned, empty, short pitch, tiled, another format, a viewport beyond 14 bits, no such pipe
  amd_dce11::ScanoutSurface x = s;
  x.address += 128;
  CHECK(!p.set_surface(1, x));
  x = s; x.pitch_pixels = 1000;
  CHECK(!p.set_surface(1, x));
  x = s; x.width = 0;
  CHECK(!p.set_surface(1, x));
  x = s; x.tiling.mode = amd_dce11::ScanoutTiling::Mode::Tiled2D;
  CHECK(!p.set_surface(1, x));
  x = s; x.format = amd_dce11::ScanoutFormat::Rgb565;
  CHECK(!p.set_surface(1, x));
  x = s; x.viewport_width = 0x4000;
  CHECK(!p.set_surface(1, x) && !p.set_surface(6, s));
  CHECK(p.disable_surface(1) && r.get(kGrphEnable + o) == 0);
  r.regs[kGrphUpdate + o] = 0x4;
  CHECK(p.flip_pending(1) && !p.flip_pending(0));
  // DMIF: two buffers asked for and the completion waited for, the pixel duration (1e9 / 148500 = 6734 ps); a second call does nothing; freed
  CHECK(p.allocate_dmif(0, m) && (r.get(kDmifBuffer0) & 7) == 2 && (r.get(kArb1) & 0xFFFF) == 6734);
  r.writes.clear();
  r.regs[kDmifBuffer0] = 2;
  CHECK(p.allocate_dmif(0, m) && r.writes.empty());
  CHECK(p.free_dmif(0) && (r.get(kDmifBuffer0) & 7) == 0);
  r.dmif_completes = false;
  CHECK(!p.allocate_dmif(3, m, 1000) && (r.get(kDmifBuffer0 + 3) & 7) == 2);
  // the safe watermarks: every set urgent at 0xFFFF / 0xFFFF, the P-state change mark 0x7FFF, stutter exit 0xFFFF and entry 0, stutter on ignoring FBC
  CHECK(p.safe_watermarks(2));
  const uint32_t q = 2 * kPipeStride;
  CHECK(r.get(kUrgency + q) == 0xFFFFFFFF && r.get(kUrgentLevel + q) == 0xFFFFFFFF && r.get(kStutter2 + q) == 0x0000FFFF);
  CHECK(r.get(kLowPower + q) == (0x7FFFu << 15 | 0x111u) && (r.get(kStutter + q) & 0x81) == 0x81);
  CHECK(r.get(kWmMask + q) == (3u | 3u << 8 | 3u << 15));          // the last set (d) selected in each field
  CHECK(r.count(kUrgency + q) == 4 && r.count(kUrgentLevel + q) == 4);
  // the DIGs: front end 3 on CRTC 1, back end 3 on front end 3 (bit 3), then let go; the hot plug select; the pins
  r.regs[kDigBe0 + 3 * kDigStride] = 0x0100;
  CHECK(p.connect(3, 1, true) && (r.get(kDigFe0 + 3 * kDigStride) & 7) == 1 && ((r.get(kDigBe0 + 3 * kDigStride) >> 8) & 0x7F) == 0x09);
  CHECK(p.connect(3, 1, false) && ((r.get(kDigBe0 + 3 * kDigStride) >> 8) & 0x7F) == 0x01 && !p.connect(7, 0, true) && !p.connect(0, 6, true));
  CHECK(p.set_hpd_select(6, 4) && (r.get(kDigBe0 + 6 * kDigStride) >> 28) == 4);
  CHECK(p.hpd_enable(5, true) && r.get(kHpd0 + 5 * 8) == 0x10000000 && p.hpd_enable(5, false) && r.get(kHpd0 + 5 * 8) == 0 && !p.hpd_enable(6, true));
}

// --- the command tables -------------------------------------------------------------------------------------------------------------------------
struct Host final : IAtomHost {
  Regs& r;
  explicit Host(Regs& regs) : r(regs) {}
  uint32_t read_register(uint32_t reg) override { return r.read(reg); }
  void write_register(uint32_t reg, uint32_t value) override { r.write(reg, value); }
  uint32_t read_mc(uint32_t) override { return 0; }
  void write_mc(uint32_t, uint32_t) override {}
  uint32_t read_pll(uint32_t) override { return 0; }
  void write_pll(uint32_t, uint32_t) override {}
  void delay_us(uint32_t) override {}
  void delay_ms(uint32_t) override {}
  uint64_t now_us() override { return 0; }
};

// tables 4 (rev 5), 12 (7), 35 (1), 49 (3), 76 (6): each copies its parameter words to registers 0x9000 + 0x10 * slot + i
std::vector<uint8_t> tables_image(uint8_t transmitter_crev = 6) {
  atomimage::Image img;
  const struct { uint32_t index, words; uint8_t crev; } t[5] = {{4, 3, 5}, {12, 4, 7}, {35, 1, 1}, {49, 6, 3}, {76, 4, transmitter_crev}};
  for (uint32_t k = 0; k < 5; ++k) {
    atomimage::Table table{t[k].index, 0, t[k].words * 4, {}, 1, t[k].crev};
    for (uint32_t i = 0; i < t[k].words; ++i) table.code.move(atomimage::REG, 0x9000 + 0x10 * k + i, atomimage::PS, i);
    table.code.eot();
    img.tables.push_back(table);
  }
  return img.build();
}
std::vector<uint32_t> words(const Regs& r, uint32_t slot, uint32_t n) {
  std::vector<uint32_t> w;
  for (uint32_t i = 0; i < n; ++i) w.push_back(r.get(0x9000 + 0x10 * slot + i));
  return w;
}

void test_tables() {
  struct Alloc final : allocators::IAllocator {
    void* allocate(uint64_t size) override { return malloc(size); }
    void* allocate_aligned(uint64_t size, uint64_t a) override {
      const uint64_t al = a < 16 ? 16 : a;
      return aligned_alloc(al, (size + al - 1) / al * al);
    }
    void deallocate(void* p) override { free(p); }
  } alloc;
  Regs r;
  Host host(r);
  const std::vector<uint8_t> image = tables_image();
  AtomBios bios(host, ConstByteView(image.data(), image.size()), alloc);
  CHECK(bios.valid());
  CommandTables t(bios);
  // SetPixelClock 1.7: 148.5 MHz in 100 Hz, combo PHY PLL 2 (22), UNIPHY1 (0x20), HDMI (3), no misc, CRTC 1, deep colour off
  CHECK(t.set_pixel_clock(1, 22, 0x20, 3, 1485000, 0) && words(r, 1, 4) == std::vector<uint32_t>({1485000, 22u | 0x20u << 8 | 3u << 16, 1, 0}));
  // the PLL powered down: no CRTC, no clock, FORCE_PROG_PPLL, the CRT encoder mode
  CHECK(t.set_pixel_clock(0xFF, 20, 0, 15, 0, CommandTables::kPixelClockForcePll) && words(r, 1, 4) == std::vector<uint32_t>({0, 20u | 15u << 16 | 1u << 24, 0xFF, 0}));
  // SetCRTC_UsingDTDTiming 3: 1080p with negative syncs, CRTC 2 (encoder mode byte 0)
  CrtcMode m;
  m.pixel_clock_khz = 148500;
  m.h_display = 1920; m.h_sync_start = 2008; m.h_sync_end = 2052; m.h_total = 2200;
  m.v_display = 1080; m.v_sync_start = 1084; m.v_sync_end = 1089; m.v_total = 1125;
  m.h_sync_negative = m.v_sync_negative = true;
  CHECK(t.set_crtc_timing(2, m) && words(r, 3, 6) == std::vector<uint32_t>({1920u | 280u << 16, 1080u | 45u << 16, 88u | 44u << 16, 4u | 5u << 16, 0x6, 2}));
  CrtcMode il = m;
  il.interlaced = true;
  CrtcMode broken = m;
  broken.h_sync_end = 1900;
  r.writes.clear();
  CHECK(!t.set_crtc_timing(2, il) && !t.set_crtc_timing(2, broken) && r.writes.empty());
  CHECK(t.enable_crtc(3, true) && words(r, 2, 1) == std::vector<uint32_t>({3u | 1u << 8}) && t.enable_crtc(3, false) && words(r, 2, 1) == std::vector<uint32_t>({3u}));
  // DIG1TransmitterControl 1.6: PHY 2 on, HDMI, 4 lanes, 148.5 MHz, HPD 2 (pin 1), no front end, the HDMI connector
  CommandTables::Transmitter tx;
  tx.phy = 2; tx.action = 1; tx.mode_or_lane_set = 3; tx.lanes = 4; tx.symbol_clock_khz = 148500; tx.hpd_select = 2; tx.connector_object_id = 0x0C;
  CHECK(t.transmitter(tx) && words(r, 4, 4) == std::vector<uint32_t>({2u | 1u << 8 | 3u << 16 | 4u << 24, 14850, 2u | 0x0Cu << 16, 0}));
  tx.phy = 7;
  CHECK(!t.transmitter(tx));
  // DIG1EncoderControl 1.5: the stream setup (DIG 0, DP, 4 lanes, 148.5 MHz, 8 bpc = 2, 2.7 GHz = 10), the panel mode, a command
  CHECK(t.encoder_stream_setup(0, 0, 4, 148500, 2, 270000) && words(r, 0, 3) == std::vector<uint32_t>({0u | 0x0Fu << 8 | 0u << 16 | 4u << 24, 14850, 2u | 10u << 8}));
  CHECK(t.encoder_panel_mode(5, 0x11) && words(r, 0, 3) == std::vector<uint32_t>({5u | 0x10u << 8 | 0x11u << 16, 0, 0}));
  CHECK(t.encoder_command(1, 0x0D) && words(r, 0, 3) == std::vector<uint32_t>({1u | 0x0Du << 8, 0, 0}));
  // the DisplayPort source: start, the patterns, the drive once per lane (two lanes), complete
  CommandTables::Transmitter base;
  base.phy = 1; base.lanes = 2; base.symbol_clock_khz = 162000; base.hpd_select = 1; base.connector_object_id = 0x13;
  DpSource src(host, t, 1, base);
  CHECK(src.start_training() && (words(r, 0, 1)[0] >> 8 & 0xFF) == 0x08);
  CHECK(src.set_pattern(Dpcd::kTrainingPattern1) && (words(r, 0, 1)[0] >> 8 & 0xFF) == 0x09 && src.set_pattern(Dpcd::kTrainingPattern3) && (words(r, 0, 1)[0] >> 8 & 0xFF) == 0x13);
  CHECK(!src.set_pattern(0));
  const uint32_t before = r.count(0x9000 + 0x40);
  CHECK(src.set_drive(0x0A) && r.count(0x9000 + 0x40) == before + 2 && words(r, 4, 4) == std::vector<uint32_t>({1u | 11u << 8 | 0x0Au << 16 | 2u << 24, 16200, 1u | 0x13u << 16, 0}));
  CHECK(src.complete_training() && (words(r, 0, 1)[0] >> 8 & 0xFF) == 0x0B);
  // another revision than DC's: refused, nothing run
  const std::vector<uint8_t> old = tables_image(5);
  Regs r2;
  Host host2(r2);
  AtomBios bios2(host2, ConstByteView(old.data(), old.size()), alloc);
  CommandTables t2(bios2);
  tx.phy = 0;
  CHECK(!t2.transmitter(tx) && r2.writes.empty());
}

// --- the ATOM firmware data tables ------------------------------------------------------------------------------------------------------------
// An image with display_object_info_table 1.4 (index 22) and gpio_pin_lut 2.1 (index 12): DP (0x3113, UNIPHY A, HPD gpio 1, I2C 0x91), HDMI (0x310C, UNIPHY1 A 0x2120, HPD gpio 2,
// I2C 0x92), a second path to the same DP connector, and a bridge on a third (DP-2 0x3213 behind TRAVIS 0x2223, UNIPHY B 0x221E, HPD gpio 3 -> no such pin bit).
std::vector<uint8_t> firmware_image() {
  std::vector<uint8_t> b(0x800, 0);
  auto u8 = [&](uint32_t at, uint32_t v) { b[at] = static_cast<uint8_t>(v); };
  auto u16 = [&](uint32_t at, uint32_t v) { u8(at, v); u8(at + 1, v >> 8); };
  auto u32 = [&](uint32_t at, uint32_t v) { u16(at, v); u16(at + 2, v >> 16); };
  u16(0, 0xAA55); u16(0x48, 0x100); memcpy(&b[0x104], "ATOM", 4); u16(0x120, 0x200);
  u16(0x200 + 4 + 12 * 2, 0x300);
  u16(0x200 + 4 + 22 * 2, 0x400);
  // gpio_pin_lut 2.1: HPD pins (gpio 1 bit 0, gpio 2 bit 8, gpio 3 bit 5), the DDC pads (0x91 -> DDC2_A, 0x92 -> DDC3_A)
  const struct { uint32_t reg; uint8_t shift, id; } pins[5] = {{kHpdA, 0, 1}, {kHpdA, 8, 2}, {kHpdA, 5, 3}, {D + 0x20EB, 0, 0x91}, {kDdc3A, 0, 0x92}};
  u16(0x300, 4 + 5 * 8); u8(0x302, 2); u8(0x303, 1);
  for (uint32_t i = 0; i < 5; ++i) { u32(0x304 + i * 8, pins[i].reg); u8(0x308 + i * 8, pins[i].shift); u8(0x30A + i * 8, pins[i].id); }
  // display_object_info_table_v1_4
  u16(0x400, 8 + 4 * 16); u8(0x402, 1); u8(0x403, 4); u16(0x404, 0x0FFF); u8(0x406, 4);
  const struct { uint16_t display, records, encoder, bridge, tag; } paths[4] = {
      {0x3113, 0x80, 0x211E, 0, 0x8}, {0x310C, 0x90, 0x2120, 0, 0x80}, {0x3113, 0x80, 0x211E, 0, 0x40}, {0x3213, 0xA0, 0x221E, 0x2223, 0x200}};
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t at = 0x408 + i * 16;
    u16(at, paths[i].display); u16(at + 2, paths[i].records); u16(at + 4, paths[i].encoder); u16(at + 6, paths[i].bridge); u16(at + 12, paths[i].tag);
  }
  const uint8_t ddc[3] = {0x91, 0x92, 0x93}, gpio[3] = {1, 2, 3};
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t at = 0x480 + i * 0x10;
    u8(at, 1); u8(at + 1, 4); u8(at + 2, ddc[i]); u8(at + 3, 0);          // atom_i2c_record
    u8(at + 4, 2); u8(at + 5, 4); u8(at + 6, gpio[i]); u8(at + 7, 1);      // atom_hpd_int_record
    u8(at + 8, 0xFF); u8(at + 9, 2);                                       // the end
  }
  return b;
}

void test_firmware_tables() {
  const std::vector<uint8_t> img = firmware_image();
  AtomConnectors::Connector c[AtomConnectors::kMaxConnectors];
  const uint32_t n = AtomConnectors::parse(ConstByteView(img.data(), img.size()), ArrayView<AtomConnectors::Connector>(c));
  CHECK(n == 3);
  if (n == 3) {
    CHECK(c[0].type == AtomConnectors::Type::DisplayPort && c[0].object_id == 0x13 && c[0].number == 1 && c[0].hpd_pin == 0 && c[0].has_ddc && c[0].ddc_id == 0x91 &&
          c[0].devices == 0x48 && c[0].encoder_object == 0x211E && c[0].bridge_object == 0 && AtomConnectors::dig_encoder(c[0]) == 0);
    CHECK(c[1].type == AtomConnectors::Type::HdmiA && c[1].hpd_pin == 1 && c[1].ddc_id == 0x92 && AtomConnectors::dig_encoder(c[1]) == 2 && c[1].plugged_state == 1);
    CHECK(c[2].type == AtomConnectors::Type::DisplayPort && c[2].number == 2 && c[2].hpd_pin == AtomConnectors::kNoPin && c[2].bridge_object == 0x2223 &&
          AtomConnectors::dig_encoder(c[2]) == 1 && AtomConnectors::link_b(c[2]));
  }
  // the I2C pads by id: hardware capable, engine and lane must agree
  const ConstByteView view(img.data(), img.size());
  AtomI2c::PinV21 pin = AtomI2c::lookup_v21(view, 0x92);
  CHECK(pin.found && pin.a_register == kDdc3A && pin.shift == 0 && pin.gpio_id == 0x92 && ddc_line_of(pin.a_register) == 2);
  pin = AtomI2c::lookup_v21(view, 0x91);
  CHECK(pin.found && ddc_line_of(pin.a_register) == 1);
  CHECK(!AtomI2c::lookup_v21(view, 0x93).found && !AtomI2c::lookup_v21(view, 0x12).found);
  // a legacy-format table (content revision 2) is not read as a pin table; a Navi object table (1.5) is not read
  std::vector<uint8_t> other = img;
  other[0x303] = 2;
  CHECK(!AtomI2c::lookup_v21(ConstByteView(other.data(), other.size()), 0x92).found);
  other = img;
  other[0x403] = 5;
  CHECK(AtomConnectors::parse(ConstByteView(other.data(), other.size()), ArrayView<AtomConnectors::Connector>(c)) == 0);
  // truncations and bit flips: everything stays inside the bytes, the results are well formed
  for (size_t len = 0; len <= img.size(); len += 3) {
    const ConstByteView part(img.data(), len);
    const uint32_t k = AtomConnectors::parse(part, ArrayView<AtomConnectors::Connector>(c));
    CHECK(k <= AtomConnectors::kMaxConnectors);
    (void)AtomI2c::lookup_v21(part, 0x92);
  }
  uint32_t rng = 31;
  for (int round = 0; round < 5000; ++round) {
    std::vector<uint8_t> e = img;
    for (int f = 0; f < 1 + round % 5; ++f) {
      rng = rng * 1664525u + 1013904223u;
      const uint32_t bit = 0x200 * 8 + (rng >> 8) % ((0x4C0 - 0x200) * 8);
      e[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    }
    const ConstByteView v(e.data(), e.size());
    const uint32_t k = AtomConnectors::parse(v, ArrayView<AtomConnectors::Connector>(c, 2));
    CHECK(k <= 2);
    for (uint32_t i = 0; i < k; ++i) CHECK(c[i].hpd_pin < 6 || c[i].hpd_pin == AtomConnectors::kNoPin);
    const uint8_t id = round % 2 ? 0x92 : static_cast<uint8_t>(rng);
    const AtomI2c::PinV21 p = AtomI2c::lookup_v21(v, id);
    if (p.found) CHECK(p.gpio_id == id);      // (the hardware capable bit, the engine and the lane are all eight bits of the id)
  }
}

}  // namespace

int main() {
  test_numbers();
  test_memory();
  test_pipe();
  test_tables();
  test_firmware_tables();
  if (g_failed) {
    printf("AmdDce12Test: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  printf("AmdDce12Test: all %d checks passed\n", g_checks);
  return 0;
}
