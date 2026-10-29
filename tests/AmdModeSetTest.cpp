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

// AmdModeSet against an AtomBIOS image whose mode setting tables hand their parameter dwords to the simulated card: the exact words of SetPixelClock, SetCRTC_Timing,
// SelectCRTC_Source, EnableCRTC, BlankCRTC, UpdateCRTC_DoubleBufferRegisters and the DIG encoder and transmitter control tables, and the refusals.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdModeSet.hpp"
#include "AtomBiosImage.hpp"

using namespace hardware;
using namespace atomimage;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

// Table slot k copies its n parameter dwords to registers 0x7400 + 0x10 * k; the last one written ends a call.
struct Slot { uint32_t index; uint32_t words; uint8_t frev, crev; };
static const Slot kTables[] = {
    {12, 4, 1, 7}, {34, 2, 1, 1}, {35, 1, 1, 1}, {39, 6, 1, 1}, {42, 1, 1, 3}, {44, 1, 1, 1}, {74, 2, 1, 4}, {75, 2, 1, 4}, {76, 3, 1, 5},
};

struct Card final : IAtomHost {
  struct Call {
    uint32_t table;
    std::vector<uint32_t> words;
  };
  std::vector<Call> calls;
  uint32_t regs[0x100] = {};
  uint32_t read_register(uint32_t) override { return 0; }
  void write_register(uint32_t reg, uint32_t value) override {
    if (reg < 0x7400 || reg >= 0x7500) return;
    regs[reg - 0x7400] = value;
    const uint32_t slot = (reg - 0x7400) / 0x10;
    if (slot >= sizeof kTables / sizeof kTables[0]) return;
    if ((reg - 0x7400) % 0x10 == kTables[slot].words - 1) {
      Call c{kTables[slot].index, {}};
      for (uint32_t i = 0; i < kTables[slot].words; ++i) c.words.push_back(regs[slot * 0x10 + i]);
      calls.push_back(c);
    }
  }
  uint32_t read_mc(uint32_t) override { return 0; }
  void write_mc(uint32_t, uint32_t) override {}
  uint32_t read_pll(uint32_t) override { return 0; }
  void write_pll(uint32_t, uint32_t) override {}
  void delay_us(uint32_t) override {}
  void delay_ms(uint32_t) override {}
  uint64_t now_us() override { return ++clock_; }
  uint64_t clock_ = 0;
};

struct Rig {
  TestAllocator alloc;
  Card card;
  std::vector<uint8_t> image;
  std::unique_ptr<AtomBios> bios;
  // `crev_override`: the revision of the table with this index replaced (0: the table is absent)
  explicit Rig(uint32_t override_index = 0xFFFF, uint8_t override_crev = 0, uint8_t override_frev = 1) {
    Image img;
    uint32_t slot = 0;
    for (const Slot& t : kTables) {
      uint8_t crev = t.crev, frev = t.frev;
      if (t.index == override_index) { crev = override_crev; frev = override_frev; }
      if (crev) {
        Code c;
        for (uint32_t i = 0; i < t.words; ++i) c.move(REG, 0x7400 + slot * 0x10 + i, PS, i);
        c.eot();
        img.tables.push_back({t.index, t.words * 4, t.words * 4, c, frev, crev});
      }
      ++slot;
    }
    image = img.build();
    bios.reset(new AtomBios(card, ConstByteView(image.data(), image.size()), alloc));
  }
};

static DisplayMode mode_1080p() {
  DisplayMode m;
  m.pixel_clock_khz = 148500;
  m.h_display = 1920; m.h_sync_start = 2008; m.h_sync_end = 2052; m.h_total = 2200;
  m.v_display = 1080; m.v_sync_start = 1084; m.v_sync_end = 1089; m.v_total = 1125;
  return m;
}

static ModeSetOutput output(uint8_t encoder_mode) {
  ModeSetOutput o;
  o.crtc = 2;
  o.pll = AmdModeSet::kPpll1;
  o.encoder_id = 0x1E;
  o.dig_encoder = 3;
  o.link_b = true;
  o.connector_object_id = 19;
  o.hpd = 2;
  o.encoder_mode = encoder_mode;
  o.lanes = 2;
  o.link_rate_khz = 270000;
  return o;
}

static void test_pixel_clock() {
  Rig r;
  AmdModeSet s(r.card, *r.bios);
  DisplayMode m = mode_1080p();
  ModeSetOutput o = output(AmdModeSet::kModeDp);
  CHECK(s.set_pixel_clock(o, m));
  // 148.5 MHz in 100 Hz units; PPLL1, encoder 0x1E, mode DP, no misc; CRTC 2, no deep colour
  CHECK(r.card.calls.size() == 1 && r.card.calls[0].table == 12 && r.card.calls[0].words == std::vector<uint32_t>({1485000, 0x1Eu << 8, 2, 0}));
  // HDMI deep colour: 10 bits 5:4, 12 bits 3:2, 8 bits none
  o = output(AmdModeSet::kModeHdmi);
  const struct { uint32_t bpc, ratio; } depths[] = {{8, 0}, {10, 1}, {12, 2}, {16, 0}, {6, 0}};
  for (const auto& d : depths) {
    r.card.calls.clear();
    o.bits_per_color = d.bpc;
    CHECK(s.set_pixel_clock(o, m) && r.card.calls[0].words[2] == (2u | d.ratio << 8) && ((r.card.calls[0].words[1] >> 16) & 0xFF) == 3);
  }
  // DVI: dual link above 165 MHz
  o = output(AmdModeSet::kModeDvi);
  r.card.calls.clear();
  CHECK(s.set_pixel_clock(o, m) && (r.card.calls[0].words[1] >> 24) == 0);
  m.pixel_clock_khz = 165001;
  CHECK(s.set_pixel_clock(o, m) && (r.card.calls[1].words[1] >> 24) == 8);
  m.pixel_clock_khz = 165000;
  CHECK(s.set_pixel_clock(o, m) && (r.card.calls[2].words[1] >> 24) == 0);
  // refused: a CRTC the card does not have, a clock that does not fit
  r.card.calls.clear();
  o.crtc = 6;
  CHECK(!s.set_pixel_clock(o, m) && r.card.calls.empty());
  o.crtc = 0;
  m.pixel_clock_khz = 0xFFFFFFFFu;
  CHECK(!s.set_pixel_clock(o, m) && r.card.calls.empty());
  // other revisions: refused; no table; another format revision
  const struct { uint8_t crev, frev; } refused[] = {{6, 1}, {8, 1}, {1, 1}, {7, 2}, {0, 0}};
  for (const auto& rf : refused) {
    Rig x(12, rf.crev, rf.frev);
    AmdModeSet y(x.card, *x.bios);
    CHECK(!y.set_pixel_clock(output(0), mode_1080p()) && x.card.calls.empty());
  }
}

static void test_crtc_timing() {
  Rig r;
  AmdModeSet s(r.card, *r.bios);
  DisplayMode m = mode_1080p();
  ModeSetOutput o = output(AmdModeSet::kModeDp);
  CHECK(s.set_crtc_timing(o, m));
  // totals and display; sync start and width (end - start); misc 0 and the CRTC at byte 2 of the fifth word
  CHECK(r.card.calls[0].table == 39 && r.card.calls[0].words ==
        std::vector<uint32_t>({2200u | 1920u << 16, 2008u | 44u << 16, 1125u | 1080u << 16, 1084u | 5u << 16, 0x20000u, 0}));
  // the flags: vertical sync low 4, horizontal sync low 2, interlace 0x80, double clock 0x100
  m.v_sync_negative = true;
  CHECK(s.set_crtc_timing(o, m) && (r.card.calls[1].words[4] & 0xFFFF) == 0x04);
  m.h_sync_negative = true;
  CHECK(s.set_crtc_timing(o, m) && (r.card.calls[2].words[4] & 0xFFFF) == 0x06);
  m.interlaced = true;
  m.double_scan = true;
  CHECK(s.set_crtc_timing(o, m) && (r.card.calls[3].words[4] & 0xFFFF) == 0x186);
  // refused: a sync that ends before it starts, a CRTC that does not exist
  const size_t n = r.card.calls.size();
  m.h_sync_end = m.h_sync_start - 1;
  CHECK(!s.set_crtc_timing(o, m));
  m = mode_1080p();
  m.v_sync_end = m.v_sync_start - 1;
  CHECK(!s.set_crtc_timing(o, m));
  o.crtc = 6;
  CHECK(!s.set_crtc_timing(o, mode_1080p()) && r.card.calls.size() == n);
  Rig x(39, 0);
  CHECK(!AmdModeSet(x.card, *x.bios).set_crtc_timing(output(0), mode_1080p()));
}

static void test_crtc_controls() {
  Rig r;
  AmdModeSet s(r.card, *r.bios);
  ModeSetOutput o = output(AmdModeSet::kModeHdmi);
  // SelectCRTC_Source V3: CRTC 2, DIG 3 is ASIC_INT_DIG4_ENCODER_ID 0x0B, mode HDMI, 8 bits is 2
  CHECK(s.select_crtc_source(o) && r.card.calls[0].table == 42 && r.card.calls[0].words == std::vector<uint32_t>({2u | 0x0Bu << 8 | 3u << 16 | 2u << 24}));
  const uint8_t ids[7] = {0x03, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};
  for (uint8_t d = 0; d < 7; ++d) CHECK(AmdModeSet::dig_encoder_id(d) == ids[d]);
  CHECK(AmdModeSet::dig_encoder_id(7) == 0);
  const struct { uint32_t bpc, code; } depths[] = {{6, 1}, {8, 2}, {10, 3}, {12, 4}, {16, 5}, {7, 2}};
  for (const auto& d : depths) {
    r.card.calls.clear();
    o.bits_per_color = d.bpc;
    CHECK(s.select_crtc_source(o) && (r.card.calls[0].words[0] >> 24) == d.code);
  }
  // V2 has no depth
  {
    Rig x(42, 2);
    CHECK(AmdModeSet(x.card, *x.bios).select_crtc_source(o) && (x.card.calls[0].words[0] >> 24) == 0);
    Rig y(42, 1);
    CHECK(!AmdModeSet(y.card, *y.bios).select_crtc_source(o));
  }
  o.dig_encoder = 7;
  CHECK(!s.select_crtc_source(o));
  o = output(0);
  o.crtc = 6;
  CHECK(!s.select_crtc_source(o));
  // EnableCRTC, BlankCRTC, UpdateCRTC_DoubleBufferRegisters
  r.card.calls.clear();
  CHECK(s.enable_crtc(5, true) && s.enable_crtc(1, false) && s.blank_crtc(3, true) && s.blank_crtc(0, false) && s.lock_crtc(4, true) && s.lock_crtc(2, false));
  CHECK(r.card.calls.size() == 6);
  CHECK(r.card.calls[0].table == 35 && r.card.calls[0].words == std::vector<uint32_t>({5u | 1u << 8}));
  CHECK(r.card.calls[1].words == std::vector<uint32_t>({1u}));
  CHECK(r.card.calls[2].table == 34 && r.card.calls[2].words == std::vector<uint32_t>({3u | 1u << 8, 0}));
  CHECK(r.card.calls[3].words == std::vector<uint32_t>({0u, 0}));
  CHECK(r.card.calls[4].table == 44 && r.card.calls[4].words == std::vector<uint32_t>({4u | 1u << 8}) && r.card.calls[5].words == std::vector<uint32_t>({2u}));
  r.card.calls.clear();
  CHECK(!s.enable_crtc(6, true) && !s.blank_crtc(6, true) && !s.lock_crtc(6, true) && r.card.calls.empty());
  Rig x(35, 0), y(34, 0), z(44, 0);
  CHECK(!AmdModeSet(x.card, *x.bios).enable_crtc(0, true) && !AmdModeSet(y.card, *y.bios).blank_crtc(0, true) && !AmdModeSet(z.card, *z.bios).lock_crtc(0, true));
}

static void test_encoder() {
  Rig r;
  AmdModeSet s(r.card, *r.bios);
  const DisplayMode m = mode_1080p();
  ModeSetOutput o = output(AmdModeSet::kModeDp);
  // DP on DIG 3 (encoder 2 table): setup: pixel clock 14850, 2.7 GHz (1) | DIG 3 << 4, action 0x0F; mode DP, two lanes, 8 bits, HPD 3
  CHECK(s.encoder(o, m, AmdModeSet::kEncoderSetup));
  CHECK(r.card.calls[0].table == 75 && r.card.calls[0].words == std::vector<uint32_t>({14850u | 0x31u << 16 | 0x0Fu << 24, 0u | 2u << 8 | 2u << 16 | 3u << 24}));
  // DIG 0 is the first table; DP video on/off
  o.dig_encoder = 0;
  r.card.calls.clear();
  CHECK(s.encoder(o, m, AmdModeSet::kEncoderDpVideoOn) && s.encoder(o, m, AmdModeSet::kEncoderDpVideoOff));
  CHECK(r.card.calls[0].table == 74 && (r.card.calls[0].words[0] >> 24) == 0x0D && (r.card.calls[1].words[0] >> 24) == 0x0C && ((r.card.calls[0].words[0] >> 16) & 0xFF) == 0x01);
  // HDMI: no link rate in the configuration, four lanes, mode 3; dual link DVI eight lanes
  o = output(AmdModeSet::kModeHdmi);
  r.card.calls.clear();
  CHECK(s.encoder(o, m, AmdModeSet::kEncoderSetup));
  CHECK(r.card.calls[0].words == std::vector<uint32_t>({14850u | 0x30u << 16 | 0x0Fu << 24, 3u | 4u << 8 | 2u << 16 | 3u << 24}));
  o = output(AmdModeSet::kModeDvi);
  o.dual_link = true;
  CHECK(s.encoder(o, m, AmdModeSet::kEncoderEnable) && (r.card.calls[1].words[1] & 0xFF00) == 8u << 8);
  // a rate that is not DP's: refused for DP; 5.4 and 3.24 GHz are V4's
  o = output(AmdModeSet::kModeDp);
  o.link_rate_khz = 216000;
  CHECK(!s.encoder(o, m, AmdModeSet::kEncoderSetup));
  o.link_rate_khz = 540000;
  CHECK(s.encoder(o, m, AmdModeSet::kEncoderSetup) && ((r.card.calls.back().words[0] >> 16) & 3) == 2);
  o.link_rate_khz = 324000;
  CHECK(s.encoder(o, m, AmdModeSet::kEncoderSetup) && ((r.card.calls.back().words[0] >> 16) & 3) == 3);
  // V3 tables have no HPD id and no 5.4 GHz
  Rig v3(75, 3);
  AmdModeSet s3(v3.card, *v3.bios);
  o.link_rate_khz = 270000;
  CHECK(s3.encoder(o, m, AmdModeSet::kEncoderSetup) && (v3.card.calls[0].words[1] >> 24) == 0);
  o.link_rate_khz = 540000;
  CHECK(!s3.encoder(o, m, AmdModeSet::kEncoderSetup));
  // refused: DIG 7, other revisions, no table
  o.dig_encoder = 7;
  CHECK(!s.encoder(o, m, AmdModeSet::kEncoderSetup));
  Rig v5(75, 5), none(75, 0);
  o = output(AmdModeSet::kModeDp);
  CHECK(!AmdModeSet(v5.card, *v5.bios).encoder(o, m, 1) && !AmdModeSet(none.card, *none.bios).encoder(o, m, 1));
}

static void test_transmitter() {
  const DisplayMode m = mode_1080p();
  {
    // V1.5: DP on DIG 3: symbol clock = rate / 10 = 27000, DIG 3 as the PHY id, action; two lanes, connector 19, mode DP, config (HPD3 sel 0x30 | coherent 2); DIG 3 select bit
    Rig r;
    AmdModeSet s(r.card, *r.bios);
    ModeSetOutput o = output(AmdModeSet::kModeDp);
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterEnable));
    CHECK(r.card.calls[0].table == 76 && r.card.calls[0].words == std::vector<uint32_t>({27000u | 3u << 16 | 1u << 24, 2u | 19u << 8 | 0u << 16 | 0x32u << 24, 1u << 3}));
    // HDMI: the pixel clock over ten, scaled by the depth (10 bits: 148500 / 8 * 10 / 10 = 18562), mode 3, four lanes, PLL source 1 (2 << 2 is for source 2)
    o = output(AmdModeSet::kModeHdmi);
    o.bits_per_color = 10;
    o.reference_clock_source = 1;
    r.card.calls.clear();
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterSetup));
    CHECK(r.card.calls[0].words == std::vector<uint32_t>({18562u | 3u << 16 | 10u << 24, 4u | 19u << 8 | 3u << 16 | (0x30u | 2u | 4u) << 24, 1u << 3}));
    // HDMI at 8 bits: the pixel clock over ten; DVI dual link: eight lanes and no coherent bit
    o.bits_per_color = 8;
    o.reference_clock_source = 0;
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterEnableOutput) && (r.card.calls[1].words[0] & 0xFFFF) == 14850);
    o = output(AmdModeSet::kModeDvi);
    o.dual_link = true;
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterDisable) && (r.card.calls[2].words[1] & 0xFF) == 8 && (r.card.calls[2].words[1] >> 24) == 0x30);
    o.dig_encoder = 7;
    CHECK(!s.transmitter(o, m, 1));
  }
  {
    // V4: symbol clock, config (coherent 2 | link B 4 | front end 0 for DIG 2 / 8 for DIG 3 | PLL << 4 | transmitter << 6), action, lanes
    Rig r(76, 4);
    AmdModeSet s(r.card, *r.bios);
    ModeSetOutput o = output(AmdModeSet::kModeDp);
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterEnable));
    CHECK(r.card.calls[0].words[0] == (27000u | (2u | 4u | 8u | 1u << 6) << 16 | 1u << 24) && r.card.calls[0].words[1] == 2);
    o = output(AmdModeSet::kModeDvi);
    o.dig_encoder = 4;
    o.link_b = false;
    o.dual_link = true;
    o.reference_clock_source = 2;
    CHECK(s.transmitter(o, m, AmdModeSet::kTransmitterEnable) && ((r.card.calls[1].words[0] >> 16) & 0xFF) == (1u | 2u << 4 | 2u << 6) && r.card.calls[1].words[1] == 8);
    o.dig_encoder = 6;
    CHECK(!s.transmitter(o, m, 1));
  }
  // other revisions, another format revision, no table
  const struct { uint8_t crev, frev; } refused[] = {{3, 1}, {6, 1}, {5, 2}, {0, 0}};
  for (const auto& rf : refused) {
    Rig x(76, rf.crev, rf.frev);
    CHECK(!AmdModeSet(x.card, *x.bios).transmitter(output(0), m, 1) && x.card.calls.empty());
  }
}

int main() {
  test_pixel_clock();
  test_crtc_timing();
  test_crtc_controls();
  test_encoder();
  test_transmitter();
  printf(g_failed ? "AmdModeSetTest: %d FAILED\n" : "AmdModeSetTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
