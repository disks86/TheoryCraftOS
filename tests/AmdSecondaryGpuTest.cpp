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

// Host tests for the display driver of a non-boot AMD GPU (AMD/AmdSecondaryGpu.*): the modes of an EDID and the one started in, then the whole
// start against a simulated card - a register file with a monitor on DDC pads (FakeI2cMonitor) and a DisplayPort sink behind a video BIOS whose
// AUX, encoder, transmitter and mode setting tables pass their parameters to registers (FakeAuxCard): the POST (ASIC_Init) of a card that needs
// it, the memory controller's apertures, the display block's start, the EDIDs, the mode chosen, the command tables of one mode on DCE 11 in the
// driver's order, the surface the CRTC scans out and the CPU draws in, another mode, a DisplayPort output trained, the refusals (no monitor, a
// BIOS that is no AtomBIOS image, too little video memory), the shutdown; truncated and damaged video BIOS images and EDIDs.
//
// Built twice by tests/run_host_tests.sh: with -DTCOS_AMD_NATIVE (everything above) and without it (AmdSecondaryGpuOffTest: the driver must refuse
// before touching the card, so the pool falls back to the probe).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdSecondaryGpu.hpp"
#include "AtomBiosImage.hpp"
#include "FakeAuxSink.hpp"
#include "FakeI2cMonitor.hpp"

namespace hardware {
uint64_t uptime_us() { return 0; }   // (the kernel clock: the tests give the display a clock of their own)
}  // namespace hardware

using namespace hardware;
using namespace hardware::amd_secondary;

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

// Register numbers typed in from AMD's headers (bif_5_0_d.h, gmc_8_1_d.h, dce_11_2_d.h, oss_3_0_d.h), not taken from the code under test.
constexpr uint32_t kConfigMemsize = 0x150a, kFbLocation = 0x809, kBiosScratch7 = 0x5d0, kAsicInitComplete = 0x200;
constexpr uint32_t kApertureLow = 0x80d, kApertureHigh = 0x80e, kApertureDefault = 0x80f, kBifFbEn = 0x1524, kHdpFlush = 0x1520;
constexpr uint32_t kVgaHdpControl = 0xca, kVgaRenderControl = 0xc0;
constexpr uint32_t kGrphEnable = 0x1a00, kGrphPrimary = 0x1a04, kGrphPitch = 0x1a06, kGrphPrimaryHigh = 0x1a07, kGrphXEnd = 0x1a0d, kGrphYEnd = 0x1a0e;
constexpr uint32_t kPostMarker = 0x7777;      // the test video BIOS's ASIC_Init writes the default engine clock here
constexpr uint64_t kGpuBase = 0xF00000000ull;  // FB_BASE 0xF00 (16 MiB units)

// The card: a plain register file (every write logged) with the monitor's DDC pads and the AUX sink's registers routed to their models. The POST
// "trains the memory": ASIC_Init's marker write sets the memory size, the framebuffer location and BIOS_SCRATCH_7's ASIC_INIT_COMPLETE.
struct Card final : IVcnRegisters {
  std::map<uint32_t, uint32_t> regs;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  FakeI2cMonitor* monitor = nullptr;
  FakeAuxCard* aux = nullptr;
  uint32_t memsize_mib = 256;
  uint32_t posts = 0;
  static bool in(uint32_t r, uint32_t lo, uint32_t hi) { return r >= lo && r <= hi; }
  uint32_t get(uint32_t r) const { auto at = regs.find(r); return at == regs.end() ? 0 : at->second; }
  void post() {
    regs[kConfigMemsize] = memsize_mib;
    regs[kFbLocation] = (0xF00u + (memsize_mib + 15) / 16 - 1) << 16 | 0xF00u;
    regs[kBiosScratch7] |= kAsicInitComplete;
  }
  uint32_t read(uint32_t r) override {
    if (monitor && in(r, 0x7000, 0x7007)) return monitor->read(r);
    if (aux && (in(r, 0x7010, 0x7061) || in(r, 0x7100, 0x718F))) return aux->read(r);
    if (r == 0x394) return 0;        // SRBM_STATUS: the memory controller is idle
    return get(r);
  }
  void write(uint32_t r, uint32_t v) override {
    writes.push_back({r, v});
    if (monitor && in(r, 0x7000, 0x7007)) return monitor->write(r, v);
    if (aux && (in(r, 0x7010, 0x7061) || in(r, 0x7100, 0x718F))) return aux->write(r, v);
    regs[r] = v;
    if (r == kPostMarker) {
      ++posts;
      post();
    }
  }
  bool written(uint32_t r) const {
    for (const auto& w : writes) if (w.first == r) return true;
    return false;
  }
  uint32_t count(uint32_t r) const {
    uint32_t n = 0;
    for (const auto& w : writes) n += w.first == r;
    return n;
  }
};

// The video BIOS: the connectors of AmdVegaMTest's image (eDP on DDC 0x92 / pin 0 / DIG 0, HDMI-A on 0x91 / pin 1 / DIG 2, DisplayPort on 0x93 /
// pin 2 / DIG 3 link B; every DDC line on the same simulated pads), its AUX, transmitter (76, rev 1.5), DIGx encoder (4, rev 1.5) and mode setting
// tables, plus the firmware info (default clocks 300 / 400 MHz) and an ASIC_Init that writes the engine clock to kPostMarker.
std::vector<uint8_t> make_vbios() {
  std::vector<uint8_t> b(0xC00, 0);
  auto u8 = [&](uint32_t at, uint32_t v) { b[at] = static_cast<uint8_t>(v); };
  auto u16 = [&](uint32_t at, uint32_t v) { u8(at, v); u8(at + 1, v >> 8); };
  auto u32 = [&](uint32_t at, uint32_t v) { u16(at, v); u16(at + 2, v >> 16); };
  u8(0, 0x55); u8(1, 0xAA); u16(0x48, 0x100);
  memcpy(&b[0x104], "ATOM", 4);
  memcpy(&b[0x30], " 761295520", 10);
  u16(0x120, 0x200);
  u16(0x11E, 0x640);
  u16(0x200 + 4 + 23 * 2, 0x5C0);   // the indirect I/O programs: none
  u16(0x200 + 4 + 10 * 2, 0x600);   // GPIO_I2C_Info
  u16(0x200 + 4 + 12 * 2, 0x300);
  u16(0x200 + 4 + 22 * 2, 0x400);
  u16(0x200 + 4 + 4 * 2, 0x340);    // FirmwareInfo
  u16(0x340, 0x20); u8(0x342, 1); u8(0x343, 1);
  u32(0x344, 0x01020304); u32(0x348, 30000); u32(0x34C, 40000);
  u16(0x300, 4 + 3 * 4);
  const uint32_t shifts[3] = {0, 8, 16};
  for (uint32_t i = 0; i < 3; ++i) { u16(0x304 + i * 4, 0x488d); u8(0x306 + i * 4, shifts[i]); u8(0x307 + i * 4, i + 1); }
  u16(0x400, 0x20); u8(0x403, 2); u16(0x404, 0x0FFF); u16(0x406, 0x100); u16(0x40E, 0x40);
  const uint32_t tags[3] = {0x2, 0x8, 0x80}, ids[3] = {0x1114, 0x110C, 0x1113};
  u8(0x440, 3);
  const uint32_t encoders[3] = {0x211E, 0x2120, 0x2220};
  for (uint32_t i = 0; i < 3; ++i) { u16(0x444 + i * 12, tags[i]); u16(0x446 + i * 12, 12); u16(0x448 + i * 12, ids[i]); u16(0x44C + i * 12, encoders[i]); }
  u8(0x500, 3);
  const uint32_t ddc[3] = {0x92, 0x91, 0x93};
  for (uint32_t i = 0; i < 3; ++i) {
    u16(0x504 + i * 8, ids[i]); u16(0x508 + i * 8, 0x180 + i * 10);
    u8(0x580 + i * 10, 1); u8(0x581 + i * 10, 4); u8(0x582 + i * 10, ddc[i]); u8(0x583 + i * 10, 0);
    u8(0x584 + i * 10, 2); u8(0x585 + i * 10, 4); u8(0x586 + i * 10, i + 1); u8(0x587 + i * 10, 1);
  }
  u16(0x600, 4 + 3 * 27);
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t at = 0x604 + i * 27;
    const uint32_t regs[8] = {0x7000, 0x7001, 0x7002, 0x7003, 0x7004, 0x7005, 0x7006, 0x7007};
    const uint8_t bits[8] = {3, 9, 13, 17, 5, 11, 15, 19};
    for (int k = 0; k < 8; ++k) { u16(at + k * 2, regs[k]); u8(at + 17 + k, bits[k]); }
    u8(at + 16, ddc[i]);
  }
  const auto table = [&](uint32_t index, uint32_t at, uint32_t crev, uint32_t param_bytes, const atomimage::Code& code) {
    u16(0x640 + 4 + index * 2, at);
    u16(at, 6 + static_cast<uint32_t>(code.b.size()));
    u8(at + 2, 1);
    u8(at + 3, crev);
    u8(at + 4, param_bytes);
    u8(at + 5, param_bytes);
    for (size_t i = 0; i < code.b.size(); ++i) u8(at + 6 + static_cast<uint32_t>(i), code.b[i]);
  };
  atomimage::Code aux;
  aux.move(atomimage::REG, FakeAuxCard::kParams1, atomimage::PS, 1);
  for (uint32_t w = 1; w <= 5; ++w) aux.move(atomimage::REG, FakeAuxCard::kRequest + w, atomimage::FB, w);
  aux.move(atomimage::REG, FakeAuxCard::kDoorbell, atomimage::PS, 0);
  for (uint32_t k = 0; k < 4; ++k) aux.move(atomimage::FB, 5 + k, atomimage::REG, FakeAuxCard::kData + k);
  aux.move(atomimage::PS, 1, atomimage::REG, FakeAuxCard::kStatus, atomimage::BYTE0, 1);
  aux.move(atomimage::PS, 1, atomimage::REG, FakeAuxCard::kLength, atomimage::BYTE0, 2);
  aux.eot();
  table(78, 0x700, 1, 8, aux);
  atomimage::Code transmitter;
  for (uint32_t i = 0; i < 3; ++i) transmitter.move(atomimage::REG, FakeAuxCard::kPanel + i, atomimage::PS, i);
  transmitter.eot();
  table(76, 0x780, 5, 12, transmitter);
  atomimage::Code enc;
  for (uint32_t i = 0; i < 3; ++i) enc.move(atomimage::REG, 0x7050 + i, atomimage::PS, i);
  enc.eot();
  table(4, 0x7A0, 5, 12, enc);
  const struct { uint32_t index, words, crev; } mode_tables[9] = {{12, 4, 7}, {34, 2, 1}, {35, 1, 1}, {49, 6, 1}, {42, 1, 3}, {44, 1, 1}, {13, 1, 1}, {40, 3, 1}, {33, 1, 1}};
  for (uint32_t k = 0; k < 9; ++k) {
    atomimage::Code m;
    for (uint32_t i = 0; i < mode_tables[k].words; ++i) m.move(atomimage::REG, FakeAuxCard::kModeSet + k * 0x10 + i, atomimage::PS, i);
    m.eot();
    table(mode_tables[k].index, 0x800 + k * 0x40, mode_tables[k].crev, mode_tables[k].words * 4, m);
  }
  atomimage::Code init;
  init.move(atomimage::REG, kPostMarker, atomimage::PS, 0);
  init.eot();
  table(0, 0xA40, 1, 8, init);
  return b;
}

// 1080p60 (148.5 MHz, 1920 + 88 + 44 + 148 / 1080 + 4 + 5 + 36), 4K60 (594 MHz), 720p60 (74.25 MHz)
const std::vector<uint8_t> t1080 = {0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C, 0x45, 0x00, 0, 0, 0, 0, 0, 0x1E};
const std::vector<uint8_t> t4k = {0x28, 0xE8, 0x00, 0x30, 0xF0, 0x70, 0x2C, 0x80, 0x30, 0x20, 0x36, 0x00, 0, 0, 0, 0, 0, 0x18};
const std::vector<uint8_t> t720 = {0x01, 0x1D, 0x00, 0x72, 0x51, 0xD0, 0x1E, 0x20, 0x6E, 0x28, 0x55, 0x00, 0, 0, 0, 0, 0, 0x1E};

void fix_checksum(std::vector<uint8_t>& e, size_t block) {
  uint8_t sum = 0;
  for (size_t i = block; i < block + 127; ++i) sum = static_cast<uint8_t>(sum + e[i]);
  e[block + 127] = static_cast<uint8_t>(0 - sum);
}

std::vector<uint8_t> edid_with_timings(const std::vector<std::vector<uint8_t>>& dtds) {
  std::vector<uint8_t> e(128, 0);
  const uint8_t header[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
  memcpy(e.data(), header, 8);
  e[18] = 1; e[19] = 3; e[20] = 0x80;
  for (size_t i = 0; i < dtds.size() && i < 4; ++i) memcpy(&e[54 + i * 18], dtds[i].data(), 18);
  fix_checksum(e, 0);
  return e;
}

// A rig: the card with its monitor models, a video BIOS, a BAR 0 of kApertureBytes in host memory.
struct Rig {
  TestAllocator alloc;
  Card card;
  Clock clock;
  FakeI2cMonitor monitor;
  FakeAuxCard aux;
  std::vector<uint8_t> vbios = make_vbios();
  std::vector<uint8_t> bar0 = std::vector<uint8_t>(kApertureBytes, 0xCD);
  Rig() {
    card.monitor = &monitor;
    card.aux = &aux;
    for (uint32_t i = 0; i < 6; ++i) card.regs[0x321 + i] = 0x10;    // the DMIF buffer allocation completes at once
    aux.sink.monitor_present = false;
    monitor.present = false;
    std::vector<uint8_t>& dpcd = aux.sink.dpcd;
    dpcd.assign(0x800, 0);
    const uint8_t caps[8] = {0x12, 0x14, 0xC4, 0x01, 0x00, 0x00, 0x01, 0x00};     // HBR2, four lanes, TPS3, enhanced framing
    for (size_t i = 0; i < 8; ++i) dpcd[i] = caps[i];
    dpcd[0x202] = dpcd[0x203] = 0x77;       // clock recovery, equalization and symbol lock at once
    dpcd[0x204] = 0x01;
  }
  Display::Inputs inputs() {
    Display::Inputs in;
    in.registers = &card;
    in.clock = &clock;
    in.allocator = &alloc;
    in.vbios = ConstByteView(vbios.data(), vbios.size());
    in.aperture = ByteView(bar0.data(), bar0.size());
    return in;
  }
  void clear_logs() {
    card.writes.clear();
    aux.table_order.clear();
    aux.mode_calls.clear();
    aux.dig_calls.clear();
  }
};

// --- modes ------------------------------------------------------------------------------------------------------------------------------------
void test_modes() {
  Mode modes[kMaxModes];
  // no check: every distinct timing in slot order, the first preferred; the choice is the preferred one
  std::vector<uint8_t> e = edid_with_timings({t1080, t720, t1080});
  uint32_t n = edid_modes(ConstByteView(e.data(), e.size()), nullptr, ArrayView<Mode>(modes));
  CHECK(n == 2 && modes[0].preferred && !modes[1].preferred && modes[0].timing.h_display == 1920 && modes[1].timing.h_display == 1280);
  CHECK(modes[0].timing.pixel_clock_khz == 148500 && modes[0].timing.h_total == 2200 && modes[0].timing.v_total == 1125);
  CHECK(modes[0].refresh_millihz == 60000 && modes[1].refresh_millihz == 60000);
  Optional<uint32_t> c = choose_mode(ArrayView<const Mode>(modes, n));
  CHECK(c && *c == 0);
  // an HDMI connector without an HDMI block: 4K (594 MHz) is above single-link TMDS, the preferred one is refused, the largest that is left is taken
  ModeCheck check;
  check.type = AtomConnectors::Type::HdmiA;
  check.use_digital = true;
  e = edid_with_timings({t4k, t720, t1080});
  CHECK(parse_monitor_info(ConstByteView(e.data(), e.size()), &check.monitor));
  n = edid_modes(ConstByteView(e.data(), e.size()), &check, ArrayView<Mode>(modes));
  CHECK(n == 2 && !modes[0].preferred && !modes[1].preferred);
  c = choose_mode(ArrayView<const Mode>(modes, n));
  CHECK(c && modes[*c].timing.h_display == 1920);
  // a full output list is not overrun; an empty list chooses nothing
  n = edid_modes(ConstByteView(e.data(), e.size()), nullptr, ArrayView<Mode>(modes, 1));
  CHECK(n == 1);
  CHECK(!choose_mode(ArrayView<const Mode>()));
  CHECK(drivable(AtomConnectors::Type::HdmiA) && drivable(AtomConnectors::Type::DisplayPort) && drivable(AtomConnectors::Type::DviD));
  CHECK(!drivable(AtomConnectors::Type::EmbeddedDisplayPort) && !drivable(AtomConnectors::Type::Lvds) && !drivable(AtomConnectors::Type::Vga));
  uint8_t mode = 0xFF;
  CHECK(encoder_mode_of(AtomConnectors::Type::HdmiB, &mode) && mode == AmdModeSet::kModeHdmi && !encoder_mode_of(AtomConnectors::Type::Vga, &mode));
}

// Truncated and damaged EDIDs: the parser stays inside the bytes and every mode it returns has a size and fits the surface.
void test_edid_damage() {
  const std::vector<uint8_t> good = edid_with_timings({t1080, t4k, t720});
  Mode modes[kMaxModes];
  for (size_t len = 0; len <= good.size(); ++len) {
    std::vector<uint8_t> e(good.begin(), good.begin() + static_cast<long>(len));
    const uint32_t n = edid_modes(ConstByteView(e.data(), e.size()), nullptr, ArrayView<Mode>(modes));
    CHECK(n <= kMaxModes);
  }
  uint32_t rng = 1234567;
  for (int round = 0; round < 4000; ++round) {
    std::vector<uint8_t> e = good;
    for (int flips = 0; flips < 1 + round % 6; ++flips) {
      rng = rng * 1664525u + 1013904223u;
      const uint32_t bit = (rng >> 8) % (128 * 8);
      e[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    }
    if (round & 1) fix_checksum(e, 0);
    ModeCheck check;
    check.type = AtomConnectors::Type::DisplayPort;
    (void)parse_monitor_info(ConstByteView(e.data(), e.size()), &check.monitor);
    const uint32_t n = edid_modes(ConstByteView(e.data(), e.size()), round % 3 ? &check : nullptr, ArrayView<Mode>(modes));
    CHECK(n <= kMaxModes);
    for (uint32_t i = 0; i < n; ++i)
      CHECK(modes[i].timing.h_display > 0 && modes[i].timing.v_display > 0 &&
            uint64_t{(modes[i].timing.h_display + kPitchAlignPixels - 1) / kPitchAlignPixels * kPitchAlignPixels} * 4 * modes[i].timing.v_display <= kSurfaceBytes);
    (void)choose_mode(ArrayView<const Mode>(modes, n));
  }
}

#ifdef TCOS_AMD_NATIVE
std::vector<uint32_t> calls_of(const FakeAuxCard& aux, uint32_t table) {
  std::vector<uint32_t> out;
  for (const FakeAuxCard::ModeCall& c : aux.mode_calls)
    if (c.table == table) out.push_back(c.words[0]);
  return out;
}

// --- HDMI from scratch: POST, apertures, display block, EDID, mode set, drawing, another mode, shutdown --------------------------------------
void test_hdmi() {
  Rig rig;
  rig.monitor.present = true;
  rig.monitor.edid = edid_with_timings({t1080, t720});
  Display display(rig.inputs());
  CHECK(display.start() == Status::Ok && display.running());
  // the POST: the card did not say ASIC_INIT_COMPLETE, so ASIC_Init ran (with the default engine clock as its first parameter)
  CHECK(display.posted() && rig.card.posts == 1 && rig.card.get(kPostMarker) == 30000);
  // video memory where the POST put it; the system aperture over all of it, unmapped accesses to the scratch page, the VGA aperture locked out
  CHECK(display.memory_map().read && display.memory_map().gpu_base == kGpuBase && display.memory_map().video_memory_bytes == (256ull << 20));
  CHECK(rig.card.get(kApertureLow) == static_cast<uint32_t>(kGpuBase >> 12) && rig.card.get(kApertureHigh) == static_cast<uint32_t>((kGpuBase + (256ull << 20) - 1) >> 12));
  CHECK(rig.card.get(kApertureDefault) == static_cast<uint32_t>((kGpuBase + kScratchOffset) >> 12) && rig.card.get(kBifFbEn) == 3);
  CHECK((rig.card.get(kVgaHdpControl) & 0x10) && (rig.card.get(kVgaRenderControl) & 0x30000) == 0);
  // the display block's start: power gating initialised (ATOM_INIT), the PHY of each of the three DIGs (eDP 0, HDMI 2, DisplayPort 3) initialised
  const std::vector<uint32_t>& order = rig.aux.table_order;
  CHECK(order.size() >= 4 && order[0] == 13 && order[1] == 76 && order[2] == 76 && order[3] == 76);
  CHECK(!rig.aux.mode_calls.empty() && rig.aux.mode_calls[0].table == 13 && rig.aux.mode_calls[0].words[0] == (7u << 8));
  CHECK(rig.aux.dig_calls.size() >= 3 && (rig.aux.dig_calls[0].words[0] >> 24) == 7 && ((rig.aux.dig_calls[0].words[0] >> 16) & 0xFF) == 0 &&
        ((rig.aux.dig_calls[1].words[0] >> 16) & 0xFF) == 2 && ((rig.aux.dig_calls[2].words[0] >> 16) & 0xFF) == 3);
  CHECK(rig.aux.panel_writes == 0);     // (the eDP panel is not powered: it is the integrated GPU's)
  // the connectors: eDP skipped (no EDID read), HDMI has the monitor, the DisplayPort sink has none
  CHECK(display.connectors().size() == 3 && display.connector() == 1 && display.edid(0).empty() && display.edid(1).size() == 128 && display.edid(2).empty());
  // the mode: the preferred 1080p, 8 bits, combo PHY PLL 2 (UNIPHY1 link A), CRTC 0
  CHECK(display.mode_count() == 2 && display.current_mode() == 0 && display.width() == 1920 && display.height() == 1080 && display.bpc() == 8);
  CHECK(display.pll() == AmdModeSet::kComboPhyPll0 + 2 && display.crtc() == 0 && display.mode_stage() == ModeStage::Done);
  // the mode set's command tables in the driver's order (after the four of the start): power gating off, locked, disabled; the source; the pixel clock and the
  // timing; overscan and scaler; the transmitter off; enabled, unblanked, unlocked; the encoder set up, its panel mode, the transmitter on
  const std::vector<uint32_t> want = {13, 76, 76, 76, 13, 44, 35, 42, 12, 49, 40, 33, 76, 35, 34, 44, 4, 4, 76};
  CHECK(order == want);
  if (order != want) { for (uint32_t t : order) printf(" %u", t); printf("\n"); }
  const std::vector<FakeAuxCard::ModeCall>& mc = rig.aux.mode_calls;
  CHECK(mc.size() == 12);
  if (mc.size() == 12) {
    CHECK(mc[1].table == 13 && mc[1].words[0] == 0u && mc[2].table == 44 && mc[2].words[0] == (1u << 8) && mc[3].table == 35 && mc[3].words[0] == 0u);
    CHECK(mc[4].table == 42 && mc[4].words[0] == (0u | 0x0Au << 8 | 3u << 16 | 2u << 24));      // CRTC 0 from DIG 2 (ASIC_INT_DIG3_ENCODER_ID 0x0A), HDMI, 8 bits
    CHECK(mc[5].table == 12 && mc[5].words[0] == 1485000 && (mc[5].words[1] & 0xFF) == 22 && ((mc[5].words[1] >> 8) & 0xFF) == 0x20 && ((mc[5].words[1] >> 16) & 0xFF) == 3 &&
          mc[5].words[2] == 0);
    CHECK(mc[6].table == 49 && mc[6].words[0] == (1920u | 280u << 16) && mc[6].words[1] == (1080u | 45u << 16) && mc[6].words[2] == (88u | 44u << 16) && mc[6].words[5] == 0);
    CHECK(mc[9].table == 35 && mc[9].words[0] == (1u << 8) && mc[10].table == 34 && mc[10].words[0] == 0u && mc[11].table == 44 && mc[11].words[0] == 0u);
  }
  const std::vector<FakeAuxCard::DigCall>& dc = rig.aux.dig_calls;
  CHECK(dc.size() == 7);
  if (dc.size() == 7) {
    CHECK(dc[3].table == 2 && (dc[3].words[0] >> 24) == 0);                                       // the transmitter off
    CHECK(dc[4].table == 0 && dc[4].words[0] == (2u | 0x0Fu << 8 | 3u << 16 | 4u << 24) && dc[4].words[1] == 14850 && dc[4].words[2] == 2u);    // stream setup: DIG 2, HDMI, 4 lanes, 8 bpc
    CHECK(dc[5].table == 0 && dc[5].words[0] == (2u | 0x10u << 8));                               // the panel mode (external)
    CHECK(dc[6].table == 2 && dc[6].words[0] == (14850u | 2u << 16 | 1u << 24));                  // the transmitter on: 148.5 MHz, PHY 2
  }
  // the scanout: CRTC 0's graphics plane on, at the surface 32 MiB into video memory, 1920 pixels a row (a multiple of 64)
  CHECK(rig.card.get(kGrphEnable) == 1 && rig.card.get(kGrphPrimary) == static_cast<uint32_t>(kGpuBase + kSurfaceOffset) && rig.card.get(kGrphPrimaryHigh) == 0xF);
  CHECK(rig.card.get(kGrphPitch) == 1920 && rig.card.get(kGrphXEnd) == 1920 && rig.card.get(kGrphYEnd) == 1080);
  CHECK(display.surface_address() == kGpuBase + kSurfaceOffset && display.pitch_bytes() == 1920 * 4);
  // the surface was cleared; what the CPU draws lands in BAR 0 at the surface's offset, and a flush is a write of the HDP flush register
  CHECK(display.pixels().data() == rig.bar0.data() + kSurfaceOffset && display.pixels().size() == 1920ull * 4 * 1080);
  CHECK(rig.bar0[kSurfaceOffset] == 0 && rig.bar0[kSurfaceOffset + 1920ull * 4 * 1080 - 1] == 0 && rig.bar0[kSurfaceOffset + 1920ull * 4 * 1080] == 0xCD);
  display.pixels()[4] = 0x7F;
  const uint32_t flushes = rig.card.count(kHdpFlush);
  display.flush();
  CHECK(rig.card.count(kHdpFlush) == flushes + 1 && rig.bar0[kSurfaceOffset + 4] == 0x7F);
  // the watermarks were programmed (DMIF allocation asked for on CRTC 0)
  CHECK(rig.card.written(0x321));
  // another mode: 720p, same output; the CRTC was on, so it is blanked first; the plane's size follows (1280 is a multiple of 64)
  rig.clear_logs();
  CHECK(display.set_mode(1) == Status::Ok && display.current_mode() == 1 && display.width() == 1280 && display.height() == 720);
  CHECK(rig.aux.table_order == std::vector<uint32_t>({13, 44, 34, 35, 42, 12, 49, 40, 33, 76, 35, 34, 44, 4, 4, 76}));
  CHECK(rig.card.get(kGrphPitch) == 1280 && rig.card.get(kGrphYEnd) == 720);
  CHECK(display.set_mode(5) == Status::BadIndex);
  // the shutdown: the transmitter off, blanked, disabled, the plane off, power gated, the combo PHY PLL off (CRTC "invalid", PLL 22, no clock)
  rig.clear_logs();
  display.shutdown();
  CHECK(!display.running() && rig.card.get(kGrphEnable) == 0);
  CHECK(rig.aux.table_order == std::vector<uint32_t>({76, 34, 35, 13, 12}));
  CHECK(calls_of(rig.aux, 13) == std::vector<uint32_t>({1u << 8}));
  const std::vector<FakeAuxCard::ModeCall>& off = rig.aux.mode_calls;
  CHECK(!off.empty() && off.back().table == 12 && off.back().words[0] == 0 && (off.back().words[1] & 0xFF) == 22 && (off.back().words[2] & 0xFF) == 0xFF);
  // a card that was POSTed already is not POSTed again
  Rig posted;
  posted.card.post();
  posted.monitor.present = true;
  posted.monitor.edid = edid_with_timings({t1080});
  Display again(posted.inputs());
  CHECK(again.start() == Status::Ok && !again.posted() && posted.card.posts == 0);
}

// --- DisplayPort: the sink's EDID over AUX, the link configured from its DPCD, trained, the video on ---------------------------------------------
void test_displayport() {
  Rig rig;
  rig.aux.sink.monitor_present = true;
  rig.aux.sink.edid = edid_with_timings({t1080});
  Display display(rig.inputs());
  CHECK(display.start() == Status::Ok && display.connector() == 2 && display.pll() == AmdModeSet::kDpDto);
  // 1080p at 8 bits: four lanes at 1.62 Gbit/s; trained with TPS3; external DP panel mode
  CHECK(display.link().outcome.trained() && display.link().config.lanes == 4 && display.link().config.rate_khz == 162000 && display.link().panel_mode == 0);
  // the start's four, then the mode set with the encoder's DP video off before the transmitter, and the training (start, pattern 1, the drive, pattern 3, complete) and the video on
  const std::vector<uint32_t> want = {13, 76, 76, 76, 13, 44, 35, 42, 12, 49, 40, 33, 4, 76, 35, 34, 44, 4, 4, 76, 4, 4, 76, 4, 4, 4};
  CHECK(rig.aux.table_order == want);
  if (rig.aux.table_order != want) { for (uint32_t t : rig.aux.table_order) printf(" %u", t); printf(" stage %d\n", int(display.mode_stage())); }
  const std::vector<FakeAuxCard::DigCall>& dc = rig.aux.dig_calls;
  std::vector<uint32_t> encoder_actions;
  for (const FakeAuxCard::DigCall& c : dc) if (c.table == 0) encoder_actions.push_back((c.words[0] >> 8) & 0xFF);
  CHECK(encoder_actions == std::vector<uint32_t>({0x0C, 0x0F, 0x10, 0x08, 0x09, 0x13, 0x0B, 0x0D}));
  // the stream setup: DIG 3, DP, four lanes, 148.5 MHz, 8 bpc, 1.62 GHz (6 x 270 MHz)
  for (const FakeAuxCard::DigCall& c : dc)
    if (c.table == 0 && ((c.words[0] >> 8) & 0xFF) == 0x0F) CHECK(c.words[0] == (3u | 0x0Fu << 8 | 0u << 16 | 4u << 24) && c.words[1] == 14850 && c.words[2] == (2u | 6u << 8));
  // the pixel clock: the DP DTO, DP mode
  const std::vector<FakeAuxCard::ModeCall>& mc = rig.aux.mode_calls;
  bool clock_seen = false;
  for (const FakeAuxCard::ModeCall& c : mc)
    if (c.table == 12) { clock_seen = true; CHECK(c.words[0] == 1485000 && (c.words[1] & 0xFF) == 11 && ((c.words[1] >> 16) & 0xFF) == 0); }
  CHECK(clock_seen);
  // the sink: D0, the link rate and lane count written, the training pattern off at the end
  std::vector<uint8_t>& dpcd = rig.aux.sink.dpcd;
  CHECK(dpcd[0x600] == 0x01 && dpcd[0x100] == 0x06 && (dpcd[0x101] & 0x1F) == 4 && dpcd[0x102] == 0);
  // the shutdown: DP video off, the sink to D3, the transmitter off ... and the DP DTO left alone (no SetPixelClock)
  rig.clear_logs();
  display.shutdown();
  CHECK(dpcd[0x600] == 0x02);
  CHECK(rig.aux.table_order == std::vector<uint32_t>({4, 76, 34, 35, 13}));
  // a sink that never locks: the link training fails, the start reports the mode set and switches everything off again
  Rig bad;
  bad.aux.sink.monitor_present = true;
  bad.aux.sink.edid = edid_with_timings({t1080});
  bad.aux.sink.dpcd[0x202] = bad.aux.sink.dpcd[0x203] = 0x00;
  Display failing(bad.inputs());
  CHECK(failing.start() == Status::ModeSet && !failing.running() && bad.card.get(kGrphEnable) == 0);
}

// --- refusals ------------------------------------------------------------------------------------------------------------------------------------
void test_refusals() {
  {
    // no monitor anywhere: no mode setting table runs (only the display block's start), NoMonitor
    Rig rig;
    Display display(rig.inputs());
    CHECK(display.start() == Status::NoMonitor && !display.running());
    CHECK(rig.aux.table_order == std::vector<uint32_t>({13, 76, 76, 76}) && rig.card.get(kGrphEnable) == 0);
  }
  {
    // a monitor whose only mode the output cannot carry
    Rig rig;
    rig.monitor.present = true;
    rig.monitor.edid = edid_with_timings({t4k});
    Display display(rig.inputs());
    CHECK(display.start() == Status::NoMode);
  }
  {
    // not an AtomBIOS image: nothing written
    Rig rig;
    rig.vbios.assign(rig.vbios.size(), 0x5A);
    Display display(rig.inputs());
    CHECK(display.start() == Status::NoBios && rig.card.writes.empty());
  }
  {
    // too little video memory for the surface
    Rig rig;
    rig.card.memsize_mib = 64;
    rig.monitor.present = true;
    rig.monitor.edid = edid_with_timings({t1080});
    Display display(rig.inputs());
    CHECK(display.start() == Status::TooLittleMemory && !rig.card.written(kApertureLow));
  }
  {
    // a BAR 0 view smaller than the driver's part of it, no registers
    Rig rig;
    Display::Inputs in = rig.inputs();
    in.aperture = in.aperture.first(kApertureBytes - 1);
    Display small(in);
    CHECK(small.start() == Status::NoAccess && rig.card.writes.empty());
    in = rig.inputs();
    in.registers = nullptr;
    Display none(in);
    CHECK(none.start() == Status::NoAccess);
    CHECK(none.set_mode(0) == Status::NotRunning);
  }
}

// Truncated video BIOS images, and bit flips in the data tables (connectors, I2C lines, firmware info): start() stays inside the image and ends in
// some status.
void test_vbios_damage() {
  const std::vector<uint8_t> good = make_vbios();
  for (size_t len = 0; len <= good.size(); len += 7) {
    Rig rig;
    rig.card.post();
    rig.monitor.present = true;
    rig.monitor.edid = edid_with_timings({t1080});
    rig.vbios.assign(good.begin(), good.begin() + static_cast<long>(len));
    Display display(rig.inputs());
    (void)display.start();
  }
  uint32_t rng = 99;
  for (int round = 0; round < 300; ++round) {
    Rig rig;
    rig.card.post();
    rig.monitor.present = true;
    rig.monitor.edid = edid_with_timings({t1080});
    for (int flips = 0; flips < 1 + round % 4; ++flips) {
      rng = rng * 1664525u + 1013904223u;
      const uint32_t bit = 0x200 * 8 + (rng >> 8) % ((0x6A0 - 0x200) * 8);     // the master data list to the end of the I2C table
      rig.vbios[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    }
    Display display(rig.inputs());
    const Status s = display.start();
    CHECK(s == Status::Ok || s == Status::NoConnectors || s == Status::NoMonitor || s == Status::NoMode || s == Status::ModeSet || s == Status::NoBios);
    display.shutdown();
  }
}
#endif

// --- the IGraphicsProcessingUnit -------------------------------------------------------------------------------------------------------------
void test_gpu() {
  TestAllocator alloc;
  PciDeviceInfo device{};
  device.vendor_id = 0x1002;
  device.device_id = 0x67DF;      // Polaris 10 (RX 480/580)
  device.class_code = kPciClassDisplay;
  CHECK(AmdSecondaryGpu::supports(device));
  PciDeviceInfo other = device;
  other.device_id = 0x694C;       // Vega M
  CHECK(AmdSecondaryGpu::supports(other));
  other.device_id = 0x6987;       // Polaris 12
  CHECK(AmdSecondaryGpu::supports(other));
  other.device_id = 0x687F;       // Vega 10 (DCE 12): not this driver's
  CHECK(!AmdSecondaryGpu::supports(other));
  other.device_id = 0x731F;       // Navi 10 (DCN 2)
  CHECK(!AmdSecondaryGpu::supports(other));
  other = device;
  other.vendor_id = 0x10DE;
  CHECK(!AmdSecondaryGpu::supports(other));

  GpuContext context{nullptr, nullptr, device, &alloc, nullptr};
  Rig rig;
  rig.monitor.present = true;
  rig.monitor.edid = edid_with_timings({t1080, t720});
  AmdSecondaryGpu gpu(context);
  CHECK(strstr(gpu.name(), "Polaris") != nullptr);
#ifndef TCOS_AMD_NATIVE
  // built without the flag: both ways in refuse at once, nothing is written to the card
  CHECK(!gpu.initialize() && strstr(gpu.status(), "TCOS_AMD_NATIVE") != nullptr);
  CHECK(!gpu.initialize_with(rig.inputs()) && rig.card.writes.empty() && rig.card.regs.size() == 6);
  CHECK(gpu.mode_count() == 0 && gpu.connector_count() == 0 && gpu.width() == 0);
  gpu.shutdown();
#else
  CHECK(!gpu.initialize());           // (no register mapper or PCI bus in the context)
  CHECK(rig.card.writes.empty());
  CHECK(gpu.initialize_with(rig.inputs()) && strcmp(gpu.status(), "running") == 0);
  CHECK(gpu.mode_count() == 2 && gpu.width() == 1920 && gpu.height() == 1080 && gpu.current_mode() == 0);
  DisplayMode dm;
  CHECK(gpu.mode_info(1, &dm) && dm.width == 1280 && dm.height == 720 && !gpu.mode_info(2, &dm));
  uint64_t total = 0, used = 0;
  CHECK(gpu.video_memory(&total, &used) && total == (256ull << 20));
  // the monitors: the driven HDMI connector first, then the two others
  CHECK(gpu.connector_count() == 3);
  ConnectorInfo info;
  CHECK(gpu.connector_info(0, &info) && info.connected && info.type == ConnectorType::Hdmi && strcmp(info.name, "HDMI-A-1") == 0 && info.preferred_width == 1920);
  CHECK(gpu.connector_info(1, &info) && !info.connected && gpu.connector_info(2, &info) && !info.connected && !gpu.connector_info(3, &info));
  uint8_t edid[256] = {};
  CHECK(gpu.read_edid(0, ByteView(edid)) == 128 && memcmp(edid, rig.monitor.edid.data(), 128) == 0 && gpu.read_edid(1, ByteView(edid)) == 0);
  MonitorMode modes[4];
  CHECK(gpu.device_modes(0, modes, 4) == 2 && modes[0].preferred && modes[0].width == 1920 && modes[0].refresh_millihz == 60000 && gpu.device_modes(1, modes, 4) == 0);
  CHECK(gpu.mode_available(0, 1280, 720) && !gpu.mode_available(0, 1024, 768) && !gpu.mode_available(1, 1280, 720));
  // drawing through the CPU's view: a pixel lands in BAR 0 at the surface (B, G, R, x), present() flushes the host data path
  gpu.put_pixel(2, 0, 0x00112233);
  const uint32_t flushes = rig.card.count(kHdpFlush);
  gpu.present();
  CHECK(rig.card.count(kHdpFlush) == flushes + 1);
  CHECK(rig.bar0[kSurfaceOffset + 8] == 0x33 && rig.bar0[kSurfaceOffset + 9] == 0x22 && rig.bar0[kSurfaceOffset + 10] == 0x11);
  // another mode by size; the framebuffer follows
  CHECK(gpu.set_display_mode(0, 1280, 720) && gpu.width() == 1280 && gpu.height() == 720 && gpu.current_mode() == 1);
  CHECK(!gpu.set_display_mode(0, 1024, 768) && !gpu.set_display_mode(1, 1920, 1080));
  CHECK(gpu.set_mode(0) && gpu.width() == 1920);
  PciAddress where{};
  CHECK(gpu.pci_location(&where));
  gpu.shutdown();
  CHECK(gpu.mode_count() == 0 && gpu.connector_count() == 0 && rig.card.get(kGrphEnable) == 0);
  // no monitor: initialize fails and leaves nothing behind
  Rig empty;
  AmdSecondaryGpu lonely(context);
  CHECK(!lonely.initialize_with(empty.inputs()) && strcmp(lonely.status(), status_name(Status::NoMonitor)) == 0 && lonely.connector_count() == 0);
#endif
}

}  // namespace

int main() {
  test_modes();
  test_edid_damage();
#ifdef TCOS_AMD_NATIVE
  test_hdmi();
  test_displayport();
  test_refusals();
  test_vbios_damage();
#endif
  test_gpu();
  if (g_failed) {
    printf("AmdSecondaryGpuTest: %d of %d checks FAILED\n", g_failed, g_checks);
    return 1;
  }
  printf("AmdSecondaryGpuTest: all %d checks passed\n", g_checks);
  return 0;
}
